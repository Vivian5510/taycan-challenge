// F2 synchronous commit support. Each public mutation is assigned one serial;
// persistence and recovery state belong to the F2 instance.
#pragma once
#include "f2_commit_experiment.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <vector>
#include <cassert>
#include <cstdlib>
#include <csignal>
#ifdef F2_DATA_PREFIX
#include "f2_prefix_experiment.h"
#endif
#ifdef F2_LINUX_DATA_PREFIX
#include <climits>
#include <linux/futex.h>
#include <sys/syscall.h>
#endif
#ifdef F2_DURABLE_COMPACTION
#include "f2_gc_trace.h"
#endif

namespace FASTER { namespace core { namespace experimental {
class ReadWriteGate {
 public:
  void set_atomic_readers(bool enabled) { atomic_readers_=enabled; }

  void lock_shared() {
    if(!atomic_readers_) { mutex_gate_.lock_shared(); return; }
    for(;;) {
      uint64_t state=state_.load(std::memory_order_acquire);
      if(state&kWriter) {
        std::unique_lock<std::mutex> wait(wait_mutex_);
        changed_.wait(wait,[&]{return !(state_.load(std::memory_order_acquire)&kWriter);});
        continue;
      }
      if(state_.compare_exchange_weak(state,state+1,std::memory_order_acquire,
                                       std::memory_order_relaxed)) return;
    }
  }

  void unlock_shared() {
    if(!atomic_readers_) { mutex_gate_.unlock_shared(); return; }
    const uint64_t previous=state_.fetch_sub(1,std::memory_order_release);
    assert((previous&kReaderMask)>0);
    if((previous&kWriter) && (previous&kReaderMask)==1) {
      std::lock_guard<std::mutex> wait(wait_mutex_);
      changed_.notify_all();
    }
  }

  bool try_lock_shared() {
    if(!atomic_readers_) return mutex_gate_.try_lock_shared();
    uint64_t state=state_.load(std::memory_order_acquire);
    while(!(state&kWriter)) {
      if(state_.compare_exchange_weak(state,state+1,std::memory_order_acquire,
                                       std::memory_order_relaxed)) return true;
    }
    return false;
  }

  void lock() {
    if(!atomic_readers_) { mutex_gate_.lock(); return; }
    writer_mutex_.lock();
    state_.fetch_or(kWriter,std::memory_order_acq_rel);
    std::unique_lock<std::mutex> wait(wait_mutex_);
    changed_.wait(wait,[&]{return (state_.load(std::memory_order_acquire)&kReaderMask)==0;});
  }

  bool try_lock() {
    if(!atomic_readers_) return mutex_gate_.try_lock();
    if(!writer_mutex_.try_lock()) return false;
    bool acquired=false;
    {
      std::lock_guard<std::mutex> wait(wait_mutex_);
      const uint64_t previous=state_.fetch_or(kWriter,std::memory_order_acq_rel);
      acquired=(previous&kReaderMask)==0;
      if(!acquired) state_.fetch_and(kReaderMask,std::memory_order_release);
    }
    if(acquired) return true;
    writer_mutex_.unlock();
    changed_.notify_all();
    return false;
  }

  void unlock() {
    if(!atomic_readers_) { mutex_gate_.unlock(); return; }
    {
      std::lock_guard<std::mutex> wait(wait_mutex_);
      state_.store(0,std::memory_order_release);
    }
    writer_mutex_.unlock();
    changed_.notify_all();
  }

 private:
  static constexpr uint64_t kWriter=uint64_t{1}<<63;
  static constexpr uint64_t kReaderMask=~kWriter;
  bool atomic_readers_=false;
  std::shared_mutex mutex_gate_;
  alignas(64) std::atomic<uint64_t> state_{0};
  std::mutex writer_mutex_;
  std::mutex wait_mutex_;
  std::condition_variable changed_;
};

inline void Fault(const char* boundary) {
  const char* requested=std::getenv("F2_SYNC_FAULT");
  if(requested && std::string(requested)==boundary) {
    std::fprintf(stderr,"F2_SYNC_FAULT %s\n",boundary);std::fflush(stderr);::kill(::getpid(),SIGKILL);
  }
}
inline std::mutex dirty_mutex;
inline std::set<std::string> dirty_paths;
inline std::set<std::string> failed_paths;
#ifdef F2_LINUX_DATA_PREFIX
struct PrefixFileGeneration {
  uint64_t dirty=0;
  uint64_t durable=0;
};
inline std::map<std::string,PrefixFileGeneration> prefix_file_generations;
#endif
#ifdef F2_DATA_PREFIX
struct PrefixSyncDescriptor {
  explicit PrefixSyncDescriptor(int descriptor):fd(descriptor){}
  ~PrefixSyncDescriptor(){if(fd>=0)::close(fd);}
  int fd;
};
inline std::map<std::string,std::shared_ptr<PrefixSyncDescriptor>> dirty_data_descriptors;
// dup shares the actual write file description. Holding it until sync also
// protects a dirty segment if F2 closes its original descriptor meanwhile.
inline bool MarkDirtyDataDescriptor(const std::string& path,int fd) {
  if(path.find("/hot/log.log")==std::string::npos)return true;
  std::lock_guard<std::mutex> lock(dirty_mutex);
  if(dirty_data_descriptors.count(path))return true;
  int copy=::fcntl(fd,F_DUPFD_CLOEXEC,0);
  if(copy<0)return false;
  dirty_data_descriptors.emplace(path,std::make_shared<PrefixSyncDescriptor>(copy));
  return true;
}
#endif
inline void MarkIoFailure(const std::string& path) {
  std::lock_guard<std::mutex> lock(dirty_mutex);failed_paths.insert(path);
}
inline void MarkDirty(const std::string& path) {
  std::lock_guard<std::mutex> lock(dirty_mutex); dirty_paths.insert(path);
#ifdef F2_LINUX_DATA_PREFIX
  if(path.find("/hot/log.log")!=std::string::npos)
    ++prefix_file_generations[path].dirty;
#endif
}
inline void SyncPath(const std::filesystem::path& path) {
#ifdef F2_DATA_PREFIX
  std::shared_ptr<PrefixSyncDescriptor> descriptor;
  {std::lock_guard<std::mutex> lock(dirty_mutex);
    auto found=dirty_data_descriptors.find(path.string());
    if(found!=dirty_data_descriptors.end()){descriptor=std::move(found->second);dirty_data_descriptors.erase(found);}
  }
  if(descriptor){Sync(descriptor->fd);return;}
#endif
  int fd=::open(path.c_str(),O_RDWR|O_CLOEXEC);
  if(fd<0) throw std::runtime_error("open sync dependency failed: "+path.string());
  try { Sync(fd); } catch (...) { ::close(fd);throw; }
  ::close(fd);
}
inline void FlushStore(const std::filesystem::path& root,const std::string& token) {
  std::set<std::string> files;
  const auto prefix=root.string()+"/";
  { std::lock_guard<std::mutex> lock(dirty_mutex);
    for(const auto& failed:failed_paths)
      if(failed.compare(0,prefix.size(),prefix)==0)
        throw std::runtime_error("persistent I/O failure: "+failed);
    for(auto it=dirty_paths.begin();it!=dirty_paths.end();) {
      if(it->compare(0,prefix.size(),prefix)==0) { files.insert(*it);it=dirty_paths.erase(it); }
      else ++it;
    }
  }
  std::vector<std::filesystem::path> dirs;
  for(const auto& item:std::filesystem::recursive_directory_iterator(root)) {
    const auto path=item.path();
    if(item.is_directory()) dirs.push_back(path);
    // CPR files are written with stdio, bypassing the asynchronous file backend.
    else if(item.is_regular_file() && path.string().find(token)!=std::string::npos) files.insert(path.string());
  }
  for(const auto& file:files) SyncPath(file);
  std::sort(dirs.begin(),dirs.end(),[](const auto& a,const auto& b){return a.native().size()>b.native().size();});
  for(const auto& dir:dirs) SyncDirectory(dir);
  SyncDirectory(root);
}
inline void PublishRoot(const std::filesystem::path& root,const std::string& token,uint64_t cutoff,const std::string& format) {
  std::string payload=token+"\n"+std::to_string(cutoff)+"\n"+format+"\n";
  std::string content=payload+std::to_string(CRC(payload.data(),payload.size()))+"\n";
  auto tmp=root/"F2-SYNC-CURRENT.tmp";
  int fd=::open(tmp.c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC,0600);
  if(fd<0) throw std::runtime_error("create F2 sync root failed");
  try { WriteAll(fd,content.data(),content.size());Sync(fd); } catch(...) {::close(fd);throw;}
  ::close(fd);
  if(::rename(tmp.c_str(),(root/"F2-SYNC-CURRENT").c_str())) throw std::runtime_error("publish F2 sync root failed");
  SyncDirectory(root);
}
inline void PurgeCheckpoints(const std::filesystem::path& root,const std::string& token) {
  std::vector<std::filesystem::path> obsolete;
  for(const auto& item:std::filesystem::recursive_directory_iterator(root)) {
    if(!item.is_directory()) continue;
    const auto parent=item.path().parent_path().filename().string();
    if((parent=="cpr-checkpoints" || parent=="index-checkpoints") && item.path().filename()!=token)
      obsolete.push_back(item.path());
  }
  for(const auto& path:obsolete) std::filesystem::remove_all(path);
}
inline void PurgeHybridMetadata(const std::filesystem::path& root,const std::string& base,const std::string& latest) {
  std::vector<std::filesystem::path> obsolete;
  for(const auto& item:std::filesystem::recursive_directory_iterator(root)) {
    if(!item.is_directory())continue;
    const auto parent=item.path().parent_path().filename().string();
    if((parent=="cpr-checkpoints"||parent=="index-checkpoints") &&
        item.path().filename()!=base && item.path().filename()!=latest) obsolete.push_back(item.path());
  }
  for(const auto& path:obsolete)std::filesystem::remove_all(path);
}
#ifdef F2_LINUX_DATA_PREFIX
class BatchCompletion {
 public:
  Status Wait() {
    uint32_t result;
    while((result=result_.load(std::memory_order_acquire))==kPending) {
      const long rc=::syscall(SYS_futex,&result_,FUTEX_WAIT_PRIVATE,kPending,nullptr,nullptr,0);
      if(rc<0 && errno!=EAGAIN && errno!=EINTR)std::terminate();
    }
    return result==kSuccess?Status::Ok:Status::IOError;
  }
  void Complete(bool failed) {
    result_.store(failed?kFailed:kSuccess,std::memory_order_release);
    if(::syscall(SYS_futex,&result_,FUTEX_WAKE_PRIVATE,INT_MAX,nullptr,nullptr,0)<0)
      std::terminate();
  }
 private:
  static constexpr uint32_t kPending=0,kSuccess=1,kFailed=2;
  static_assert(std::atomic<uint32_t>::is_always_lock_free && sizeof(std::atomic<uint32_t>)==4);
  alignas(uint32_t) std::atomic<uint32_t> result_{kPending};
};
#endif
// One pending synchronous mutation. The leader picks up all waiting requests
// within a bounded window and commits them together as one WAL frame.
struct SyncRequest {
  uint32_t op;
  const std::string& key;
  const std::string& value;
  std::function<Status(uint64_t)> apply;
  bool done=false;
  Status status=Status::Corruption;
  SyncRequest* lf_next=nullptr; // Treiber-stack link for lock-free enqueue variant
  // Set by the leader when draining this request into a batch. Pipeline variant
  // uses this to prevent group members from attempting to become a new leader
  // while the current leader is still doing stage/sync/apply.
  bool in_group=false;
};
class SyncState {
 public:
  std::filesystem::path root;
  std::string format;
  bool journal=false;
  std::atomic<bool> leading{false}; // atomic so lock-free path can CAS
  uint64_t full_checkpoints=0, groups=0, operations=0;
  std::atomic<bool> poisoned{false};
  uint64_t serial=0, reserved_serial=0, cutoff=0, reclaim_bytes=1<<20;
  std::atomic<uint64_t> applied_serial{0};
  std::atomic<uint64_t> durable_serial{0};
  std::unique_ptr<BatchLog> log;
  std::function<void()> touch;
  std::mutex mutex;
  std::condition_variable cv;
  std::condition_variable collect_cv;
  std::deque<SyncRequest*> pending;
  ReadWriteGate gate;
  // Architecture-experiment knobs (read once from env in SyncConfigure)
  // F2_GC_ADAPTIVE=1   : drain early when min_batch requests are ready
  // F2_GC_PIPELINE=1   : release leadership after drain so next group collects
  //                       during current group's stage+sync+apply; stage_mutex
  //                       serialises the WAL/serial/apply phases
  // F2_GC_LOCKFREE=1   : followers enqueue via atomic Treiber stack (no mutex)
  bool opt_adaptive=false;
  size_t opt_min_batch=4;
  uint64_t group_wait_us=50;
  bool opt_pipeline=false;
  bool opt_lockfree=false;
  bool opt_atomic_readers=true;
  bool profile_reads=false;
  bool reuse_read_guid=false;
  bool reuse_write_guid=false;
  std::atomic<uint64_t> profile_read_samples{0};
  std::atomic<uint64_t> profile_gate_wait_ns{0},profile_session_start_ns{0};
  std::atomic<uint64_t> profile_read_await_ns{0},profile_complete_pending_ns{0};
  std::atomic<uint64_t> profile_session_stop_ns{0},profile_get_total_ns{0};
  std::mutex stage_mutex; // pipeline: serialises stage+sync+apply across leaders
  std::atomic<SyncRequest*> lf_stack{nullptr}; // lock-free Treiber stack head
  std::atomic<size_t> lf_count{0};             // approximate queue depth for adaptive
#ifdef F2_DATA_PREFIX
  PrefixState prefix;
#endif
#ifdef F2_LINUX_DATA_PREFIX
  bool opt_batch_notify=false;
  bool opt_batch_takeover=false;
  bool opt_batch_profile=false;
  std::chrono::steady_clock::time_point batch_first_at{},batch_full_at{},batch_completed_at{};
  uint64_t batch_collect_ns=0,batch_ready_delay_ns=0,batch_gap_ns=0;
  uint64_t batch_checkpoint_ns=0,batch_publication_ns=0;
  bool collecting=false;
  uint64_t batch_direct_owners=0,batch_fallback_owners=0;
  std::shared_ptr<BatchCompletion> collecting_batch,active_batch;
  bool opt_async_writer=false;
  uint64_t async_wait_us=100;
  bool durable_stop=false;
  bool durable_error=false;
  std::mutex durable_mutex;
  std::condition_variable durable_cv;
  std::deque<std::shared_ptr<PrefixState::DurabilityJob>> durable_jobs;
  std::thread durable_thread;
#endif
};
}}}
