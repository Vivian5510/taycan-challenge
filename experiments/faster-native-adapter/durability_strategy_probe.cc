// Explicit asynchronous batch experiment. Never a score for synchronous KVEngine.
// Both routes execute the same opaque 32 B keys / 256 B values, Put/overwrite/Delete.
#include "faster_backend.cc"
#include <csignal>
#include <ctime>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>

namespace {
namespace ex = FASTER::core::experimental;
using Clock = std::chrono::steady_clock;

class Barrier {
 public:
  explicit Barrier(unsigned count) : count_(count) {}
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    auto generation = generation_;
    if (++waiting_ == count_) { ++generation_; waiting_ = 0; cv_.notify_all(); }
    else cv_.wait(lock, [&] { return generation_ != generation; });
  }
 private:
  unsigned count_, waiting_ = 0, generation_ = 0;
  std::mutex mutex_;
  std::condition_variable cv_;
};
struct Options {
  fs::path directory;
  std::string mode = "checkpoint", phase = "write", fault;
  uint64_t keys = 256, rounds = 3, batch = 64, threads = 1, seed = 20261030;
  bool wakeup = true;
};
Options Parse(int argc, char** argv) {
  Options o;
  for (int i=1; i<argc; ++i) {
    std::string a(argv[i]); const auto eq = a.find('=');
    if (eq == std::string::npos || a.substr(0,2) != "--") throw std::runtime_error("expected --name=value");
    auto k=a.substr(2,eq-2), v=a.substr(eq+1);
    if (k=="directory") o.directory=v;
    else if (k=="mode") o.mode=v;
    else if (k=="phase") o.phase=v;
    else if (k=="fault") o.fault=v;
    else if (k=="keys") o.keys=std::stoull(v);
    else if (k=="rounds") o.rounds=std::stoull(v);
    else if (k=="batch") o.batch=std::stoull(v);
    else if (k=="threads") o.threads=std::stoull(v);
    else if (k=="seed") o.seed=std::stoull(v);
    else if (k=="wakeup") o.wakeup=std::stoull(v)!=0;
    else throw std::runtime_error("unknown option: "+k);
  }
  if (o.directory.empty() || (o.mode!="checkpoint" && o.mode!="commit-log") ||
      (o.phase!="write" && o.phase!="recover") || !o.keys || o.keys>100000 ||
      !o.threads || o.threads>8 || o.keys%o.threads || !o.batch ||
      o.batch>o.keys/o.threads || (o.keys/o.threads)%o.batch || !o.rounds || o.rounds>10 ||
      (!o.fault.empty() && o.fault!="after-ack" && o.fault!="after-final-ack" && o.fault!="before-commit" && o.fault!="before-publish"))
    throw std::runtime_error("invalid experiment options");
  if (!o.fault.empty() && o.threads!=1) throw std::runtime_error("fault probe requires one session");
  return o;
}
void Check(FStatus s) { if (s!=FStatus::Ok) throw std::runtime_error(fc::StatusStr(s)); }
uint64_t Mix(uint64_t x) {
  x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31);
}
std::string Bytes(size_t size, uint64_t seed) {
  std::string out(size,'\0');
  for (size_t i=0; i<size; ++i) { seed=Mix(seed+i+0x9e3779b97f4a7c15ULL); out[i]=char(seed); }
  return out;
}
std::string MakeKey(uint64_t id, uint64_t seed) {
  auto key=Bytes(32,Mix(seed^id)); ex::Encode(key.data(),id,8); return key;
}
std::string MakeValue(uint64_t id, uint64_t round, uint64_t seed) { return Bytes(256,Mix(seed^id^(round<<32))); }
bool Deleted(uint64_t id, uint64_t round) { return round%3==2 && id%4==0; }

FStatus Apply(Store& store, uint32_t op, const std::string& key, const std::string& value, uint64_t serial) {
  auto done=std::make_shared<Completion>();
  FStatus s;
  if (op==1) {
    UpsertContext context{key,value,done};
    s=store.Upsert(context,CompleteOperation<UpsertContext>,serial);
  } else {
    DeleteContext context{key,done};
    s=store.Delete(context,CompleteOperation<DeleteContext>,serial);
  }
  while (s==FStatus::Pending && !done->done.load(std::memory_order_acquire)) store.CompletePending(false);
  return s==FStatus::Pending ? done->status.load() : s;
}
uint64_t Touch(Store& store, uint64_t serial=1) {
  auto done=std::make_shared<Completion>(); ReadContext c{"experiment-session-touch",done};
  auto s=store.Read(c,CompleteOperation<ReadContext>,serial);
  while (s==FStatus::Pending && !done->done.load()) store.CompletePending(false);
  s=s==FStatus::Pending ? done->status.load() : s;
  if (s!=FStatus::NotFound) Check(s);
  store.CompletePending(true); return serial;
}
std::unique_ptr<Store> OpenStore(const Options& o) {
  Store::HotIndexConfig hot{4096}; Store::ColdIndexConfig cold{4096,12ULL<<20,1.0/3.0};
  fc::ReadCacheConfig rc{}; rc.enabled=false;
  auto compaction=fc::DEFAULT_F2_COMPACTION_CONFIG;
  compaction.hot_store.enabled=compaction.cold_store.enabled=false;
  auto store=std::make_unique<Store>(hot,12ULL<<20,(o.directory/"hot").string()+"/",
    cold,12ULL<<20,(o.directory/"cold").string()+"/",1.0/3.0,0,rc,compaction);
  store->ExperimentEnableWakeup(o.wakeup);
  return store;
}
void Publish(const fs::path& directory, const fc::Guid& token) {
  // File contents are synchronized by the F2 file backend. Also persist new
  // checkpoint directories before publishing the recovery token.
  std::vector<fs::path> dirs;
  for (const auto& entry:fs::recursive_directory_iterator(directory))
    if (entry.is_directory()) dirs.push_back(entry.path());
  std::sort(dirs.begin(),dirs.end(),[](const auto& a,const auto& b){return a.native().size()>b.native().size();});
  for (const auto& p:dirs) ex::SyncDirectory(p);
  const std::string data=token.ToString()+"\n";
  auto temp=directory/"FASTER-CURRENT.tmp";
  int fd=::open(temp.c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC,0600);
  if (fd<0) throw std::runtime_error("open token failed");
  try { ex::WriteAll(fd,data.data(),data.size()); ex::Sync(fd); }
  catch (...) { ::close(fd); throw; }
  ::close(fd);
  if (::rename(temp.c_str(),(directory/"FASTER-CURRENT").c_str())) throw std::runtime_error("publish token failed");
  ex::SyncDirectory(directory);
}
fc::Guid Token(const fs::path& directory) {
  std::ifstream in(directory/"FASTER-CURRENT"); std::string s; std::getline(in,s);
  if (s.size()!=36) throw std::runtime_error("missing recovery token");
  return fc::Guid::Parse(s);
}
fc::Guid Checkpoint(Store& store,uint64_t serial) {
  fc::Guid token; store.ExperimentPrepareCheckpoint();
  if (!store.ExperimentBeginCheckpoint(token)) throw std::runtime_error("checkpoint not accepted");
  store.ExperimentWaitCheckpoint(serial); return token;
}
double Seconds(Clock::time_point t) { return std::chrono::duration<double>(Clock::now()-t).count(); }
struct CounterSnapshot { uint64_t sync, sync_time, writes, bytes; };
CounterSnapshot Snapshot() { return {ex::sync_calls.load(),ex::sync_ns.load(),ex::write_calls.load(),ex::write_bytes.load()}; }
void Stats(const char* name, CounterSnapshot before) {
  auto after=Snapshot();
  std::cout<<",\""<<name<<"\":{\"sync_calls\":"<<after.sync-before.sync
    <<",\"sync_seconds\":"<<double(after.sync_time-before.sync_time)/1e9
    <<",\"write_calls\":"<<after.writes-before.writes<<",\"write_bytes\":"<<after.bytes-before.bytes<<"}";
}
void Kill() { std::cout.flush(); std::cerr.flush(); ::kill(::getpid(),SIGKILL); }
void RunWrite(const Options& o) {
  if (fs::exists(o.directory) && !fs::is_empty(o.directory)) throw std::runtime_error("write requires fresh directory");
  fs::create_directories(o.directory);
  const auto open_begin=Clock::now(); auto store=OpenStore(o);
  store->StartSession(); Touch(*store); auto token=Checkpoint(*store,1); Publish(o.directory,token); store->StopSession();
  if (o.mode=="commit-log") store->ExperimentOpenCommitLog(o.directory.string(),o.threads);
  const double open_seconds=Seconds(open_begin);
  Barrier barrier(unsigned(o.threads)); std::vector<std::thread> workers;
  std::vector<std::vector<double>> latencies(o.threads);
  std::vector<double> commit_times(o.threads,0);
  Clock::time_point begin;
  auto before=Snapshot();
  for (uint32_t lane=0; lane<o.threads; ++lane) workers.emplace_back([&,lane] {
    try {
      store->StartSession(); uint64_t serial=Touch(*store);
      const uint64_t per_lane=o.keys/o.threads;
      std::vector<std::string> keys;
      for (uint64_t n=0;n<per_lane;++n) keys.push_back(MakeKey(n*o.threads+lane,o.seed));
      latencies[lane].reserve(per_lane*o.rounds);
      barrier.Wait();
      if (!lane) { begin=Clock::now(); before=Snapshot(); }
      barrier.Wait();
      uint64_t groups=0;
      for (uint64_t round=0; round<o.rounds; ++round) for (uint64_t first=0;first<per_lane;first+=o.batch) {
        std::vector<Clock::time_point> issued; issued.reserve(o.batch);
        for (uint64_t n=first;n<first+o.batch;++n) {
          const auto id=n*o.threads+lane;
          const uint32_t op=Deleted(id,round)?2:1;
          const auto value=op==1?MakeValue(id,round,o.seed):std::string{};
          ++serial; issued.push_back(Clock::now());
          auto native=[&]{return Apply(*store,op,keys[n],value,serial);};
          Check(o.mode=="commit-log"?store->ExperimentStage(lane,op,keys[n],value,serial,native):native());
        }
        if (o.fault=="before-commit" && groups==1) Kill();
        const auto commit_begin=Clock::now();
        if (o.mode=="commit-log") {
          const auto durable=store->ExperimentCommitBatch(lane);
          if (durable!=serial) throw std::runtime_error("wrong log frontier");
        } else {
          store->ExperimentPrepareCheckpoint(); barrier.Wait();
          if (!lane && !store->ExperimentBeginCheckpoint(token)) throw std::runtime_error("checkpoint not accepted");
          barrier.Wait(); store->ExperimentWaitCheckpoint(serial); barrier.Wait();
          if (!lane) {
            if (o.fault=="before-publish" && groups==1) Kill();
            Publish(o.directory,token);
          }
          barrier.Wait();
        }
        commit_times[lane]+=Seconds(commit_begin);
        const auto ack=Clock::now();
        for (auto start:issued) latencies[lane].push_back(std::chrono::duration<double,std::micro>(ack-start).count());
        ++groups;
        if ((o.fault=="after-ack" && groups==1) ||
            (o.fault=="after-final-ack" && groups==per_lane/o.batch*o.rounds)) {
          std::cout<<"{\"type\":\"ack\",\"completed\":"<<groups*o.batch<<"}"<<std::endl; Kill();
        }
      }
      barrier.Wait(); store->CompletePending(true); store->StopSession();
    } catch(const std::exception& e) { std::cerr<<"WORKER_FAIL "<<e.what()<<std::endl; std::_Exit(2); }
  });
  for(auto& thread:workers) thread.join();
  const double elapsed=Seconds(begin);
  auto measured=Snapshot();
  std::vector<double> latency;
  for(auto& list:latencies) latency.insert(latency.end(),list.begin(),list.end());
  std::sort(latency.begin(),latency.end());
  auto quantile=[&](double p){return latency[std::min(latency.size()-1,size_t(std::ceil(latency.size()*p)-1))];};
  std::cout<<std::setprecision(12)<<"{\"type\":\"result\",\"status\":\"PASS\",\"mode\":\""<<o.mode
    <<"\",\"keys\":"<<o.keys<<",\"rounds\":"<<o.rounds<<",\"threads\":"<<o.threads<<",\"batch_per_session\":"<<o.batch
    <<",\"wakeup\":"<<(o.wakeup?"true":"false")<<",\"operations\":"<<latency.size()<<",\"seconds\":"<<elapsed
    <<",\"ops_per_second\":"<<latency.size()/elapsed<<",\"open_seconds\":"<<open_seconds
    <<",\"ack_p50_us\":"<<quantile(.5)<<",\"ack_p99_us\":"<<quantile(.99)
    <<",\"commit_seconds_summed\":"<<std::accumulate(commit_times.begin(),commit_times.end(),0.0);
  Stats("measure_io",before);
  // Report deferred checkpoint work for log route instead of hiding it at exit.
  const auto consolidate_begin=Clock::now(); auto consolidate_before=Snapshot();
  store->StartSession(); Touch(*store); token=Checkpoint(*store,1); Publish(o.directory,token); store->StopSession();
  std::cout<<",\"consolidate_seconds\":"<<Seconds(consolidate_begin);
  Stats("consolidate_io",consolidate_before);
  std::cout<<"}"<<std::endl;
}
void RunRecover(const Options& o) {
  auto begin=Clock::now(); auto store=OpenStore(o); uint32_t version=0; std::vector<fc::Guid> sessions;
  Check(store->Recover(Token(o.directory),version,sessions));
  uint64_t serial=sessions.empty()?0:store->ContinueSession(sessions.front());
  if (sessions.empty()) store->StartSession(); Touch(*store,++serial);
  uint64_t replayed=0;
  if (o.mode=="commit-log") {
    for (uint32_t i=0;i<o.threads;++i)
      if (!fs::exists(o.directory/("F2-COMMIT-"+std::to_string(i)))) throw std::runtime_error("missing commit lane");
    store->ExperimentOpenCommitLog(o.directory.string(),o.threads);
    replayed=store->ExperimentRecoverCommitLog([&](uint32_t op,const std::string& key,const std::string& value) {
      Check(Apply(*store,op,key,value,++serial));
    });
  }
  const double restore_seconds=Seconds(begin);
  for(uint64_t id=0;id<o.keys;++id) {
    auto done=std::make_shared<Completion>(); ReadContext context{MakeKey(id,o.seed),done};
    auto status=store->Read(context,CompleteOperation<ReadContext>,++serial);
    while(status==FStatus::Pending && !done->done.load()) store->CompletePending(false);
    status=status==FStatus::Pending?done->status.load():status;
    if (Deleted(id,o.rounds-1)) {
      if (status!=FStatus::NotFound) throw std::runtime_error("deletion lost: "+std::to_string(id));
    } else {
      Check(status);
      if (done->value!=MakeValue(id,o.rounds-1,o.seed)) throw std::runtime_error("value mismatch: "+std::to_string(id));
    }
  }
  store->CompletePending(true); store->StopSession();
  std::cout<<std::setprecision(12)<<"{\"type\":\"recovery\",\"status\":\"PASS\",\"keys\":"<<o.keys
    <<",\"replayed\":"<<replayed<<",\"restore_seconds\":"<<restore_seconds<<"}"<<std::endl;
}
}
int main(int argc,char** argv) {
  try { auto o=Parse(argc,argv); if(o.phase=="write") RunWrite(o);else RunRecover(o);return 0; }
  catch(const std::exception& e) { std::cerr<<"PROBE_FAIL "<<e.what()<<std::endl;return 1; }
}
