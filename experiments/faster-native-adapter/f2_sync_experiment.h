// F2 synchronous commit support. Each public mutation is assigned one serial;
// persistence and recovery state belong to the F2 instance.
#pragma once
#include "f2_commit_experiment.h"
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <cstdlib>
#include <csignal>
#ifdef F2_DATA_PREFIX
#include "f2_prefix_experiment.h"
#endif
#ifdef F2_DURABLE_COMPACTION
#include "f2_gc_trace.h"
#endif

namespace FASTER { namespace core { namespace experimental {
inline void Fault(const char* boundary) {
  const char* requested=std::getenv("F2_SYNC_FAULT");
  if(requested && std::string(requested)==boundary) {
    std::fprintf(stderr,"F2_SYNC_FAULT %s\n",boundary);std::fflush(stderr);::kill(::getpid(),SIGKILL);
  }
}
inline std::mutex dirty_mutex;
inline std::set<std::string> dirty_paths;
inline std::set<std::string> failed_paths;
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
// One pending synchronous mutation. The leader picks up all waiting requests
// within a bounded window and commits them together as one WAL frame.
struct SyncRequest {
  uint32_t op;
  const std::string& key;
  const std::string& value;
  std::function<Status(uint64_t)> apply;
  bool done=false;
  Status status=Status::Corruption;
};
class SyncState {
 public:
  std::filesystem::path root;
  std::string format;
  bool journal=false, leading=false;
  uint64_t full_checkpoints=0, groups=0, operations=0;
  std::atomic<bool> poisoned{false};
  uint64_t serial=0, cutoff=0, reclaim_bytes=1<<20;
  std::unique_ptr<BatchLog> log;
  std::function<void()> touch;
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<SyncRequest*> pending;
  std::shared_mutex gate;
#ifdef F2_DATA_PREFIX
  PrefixState prefix;
#endif
};
}}}
