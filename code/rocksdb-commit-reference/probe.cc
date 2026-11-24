#include "rocksdb_commit.h"
#include <chrono>
#include <csignal>
#include <iostream>
#include <map>
#include <thread>
#include <vector>
#include <unistd.h>

namespace r = ROCKSDB_NAMESPACE;
std::string Key(uint64_t id) {
  std::string key(32, '\0');
  for (unsigned i=0;i<8;++i) key[i]=char(id>>(8*i));
  return key;
}
std::string Value(uint64_t id) {
  std::string value(256, '\0');
  uint64_t state=id+1;
  for (auto& c:value) { state=state*6364136223846793005ULL+1; c=char(state>>56); }
  return value;
}
struct State : r::WriteBatch::Handler {
  std::map<std::string,std::string> values;
  r::Status PutCF(uint32_t cf,const r::Slice& key,const r::Slice& value) override {
    if (cf) return r::Status::InvalidArgument("unexpected CF");
    values[key.ToString()]=value.ToString(); return r::Status::OK();
  }
  r::Status DeleteCF(uint32_t cf,const r::Slice& key) override {
    if (cf) return r::Status::InvalidArgument("unexpected CF");
    values.erase(key.ToString()); return r::Status::OK();
  }
};
int main(int argc,char** argv) {
  try {
    if (argc<3) throw std::runtime_error("MODE DIRECTORY [THREADS COUNT]");
    const std::string mode=argv[1],directory=argv[2];
    if (mode=="verify" || mode=="recover-benchmark") {
      State state; auto operations=reference::Recover(directory+"/000001.log",state);
      uint64_t count=argc>3?std::stoull(argv[3]):64;
      for (uint64_t id=0;id<count;++id) {
        if (mode=="verify" && id%3==0) {
          if (state.values.count(Key(id))) throw std::runtime_error("delete lost");
        } else if (state.values[Key(id)]!=Value(id)) throw std::runtime_error("value lost");
      }
      auto expected=mode=="verify"?count+(count+2)/3+1:count;
      if (operations!=expected) throw std::runtime_error("operation count mismatch");
      if (mode=="verify" && state.values["large"]!=std::string(100000,'v')) throw std::runtime_error("fragmented record lost");
      std::cout<<"{\"status\":\"PASS\",\"recovered_operations\":"<<operations<<"}\n";
      return 0;
    }
    reference::Counters stats;
    reference::Committer commit(directory,stats);
    if (mode=="write-kill") {
      for (uint64_t id=0;id<64;++id) { r::WriteBatch b;reference::Check(b.Put(Key(id),Value(id)));reference::Check(commit.Write(b)); }
      for (uint64_t id=0;id<64;id+=3) { r::WriteBatch b;reference::Check(b.Delete(Key(id)));reference::Check(commit.Write(b)); }
      r::WriteBatch large; reference::Check(large.Put("large",std::string(100000,'v')));reference::Check(commit.Write(large));
      std::cout<<"ACK 87"<<std::endl;::kill(::getpid(),SIGKILL);
    }
    unsigned threads=argc>3?std::stoul(argv[3]):4;
    uint64_t count=argc>4?std::stoull(argv[4]):1024;
    if (mode!="load" && mode!="sync-error" && mode!="append-error") throw std::runtime_error("unknown mode");
    if (!threads || threads>128 || count<threads || count>10000000) throw std::runtime_error("invalid threads/count");
    std::atomic<unsigned> ready{0};std::atomic<bool> start{false};std::atomic<uint64_t> errors{0};
    if (mode=="sync-error") stats.fail_sync=true;
    if (mode=="append-error") stats.fail_append=true;
    std::vector<std::thread> writers;
    for (unsigned thread=0;thread<threads;++thread) writers.emplace_back([&,thread]{
      ++ready; while(!start.load()) std::this_thread::yield();
      for (uint64_t id=thread;id<count;id+=threads) {
        r::WriteBatch batch;reference::Check(batch.Put(Key(id),Value(id)));
        if (!commit.Write(batch).ok()) ++errors;
      }
    });
    while(ready!=threads) std::this_thread::yield();
    auto begin=std::chrono::steady_clock::now();start=true;
    for(auto& writer:writers) writer.join();
    double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    bool injected=mode=="sync-error"||mode=="append-error";
    if (injected) {
      stats.fail_sync=false;stats.fail_append=false;
      r::WriteBatch late;reference::Check(late.Put("later","later"));
      if (errors!=count||commit.Write(late).ok()) throw std::runtime_error("failure propagation/closed state mismatch");
    } else if(errors) throw std::runtime_error("unexpected write failure");
    std::cout<<"{\"status\":\"PASS\",\"threads\":"<<threads<<",\"attempted_operations\":"<<count
      <<",\"operations\":"<<count-errors.load()<<",\"seconds\":"<<seconds<<",\"ops_per_second\":"<<(count-errors.load())/seconds
      <<",\"groups\":"<<commit.groups()<<",\"file_syncs\":"<<stats.file_syncs.load()
      <<",\"directory_syncs\":"<<stats.directory_syncs.load()<<",\"append_calls\":"<<stats.append_calls.load()
      <<",\"append_bytes\":"<<stats.append_bytes.load()<<",\"errors\":"<<errors.load()<<"}\n";
    return 0;
  } catch (const std::exception& e) { std::cerr<<e.what()<<"\n";return 1; }
}
