// Development interposer: always delegates to the actual fsync unless a test
// explicitly requests EIO. No asynchronous bypass or successful fake sync.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/syscall.h>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>
namespace {
using Sync=int(*)(int);
int kernel_sync(int fd){return int(::syscall(SYS_fsync,fd));}
Sync real_sync=kernel_sync;
std::atomic<unsigned long long> calls{0},ns{0},wal_calls{0},wal_ns{0},hot_calls{0},hot_ns{0},failures{0};
std::atomic<bool> held{false};
thread_local bool nested=false;
int audit_sync(int fd) {
  if(nested)return real_sync(fd);
  nested=true;
  char path[4096]{};::fcntl(fd,F_GETPATH,path);
  const size_t n=std::strlen(path);
  const bool hot=std::strstr(path,"/hot/log.log");
  const bool wal=n>4 && std::strcmp(path+n-4,".log")==0;
  const char* mode=std::getenv("SYNC_AUDIT_MODE");
  const char* kind=std::getenv("SYNC_AUDIT_KIND");
  const bool selected=(kind&&std::strcmp(kind,"f2")==0)?hot:wal;
  const char* arm=std::getenv("SYNC_AUDIT_ARM_PATH");
  const bool armed=!arm || ::access(arm,F_OK)==0;
  if(mode && selected && armed && !held.exchange(true)) {
    const char* notice=std::getenv("SYNC_AUDIT_NOTICE_FD");
    const char* release=std::getenv("SYNC_AUDIT_RELEASE_FD");
    if(notice&&release) {
      const char byte='S';if(::write(std::atoi(notice),&byte,1)!=1)_exit(126);
      char input;ssize_t bytes;do{bytes=::read(std::atoi(release),&input,1);}while(bytes<0&&errno==EINTR);
      if(bytes!=1)_exit(127);
    }
    if(std::strcmp(mode,"fail")==0){++failures;nested=false;errno=EIO;return -1;}
  }
  const auto begin=std::chrono::steady_clock::now();
  int result=real_sync(fd);int error=errno;
  const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
  ++calls;ns+=elapsed;if(wal){++wal_calls;wal_ns+=elapsed;}if(hot){++hot_calls;hot_ns+=elapsed;}if(result<0)++failures;
  nested=false;errno=error;return result;
}
__attribute__((destructor)) void report(){
  std::fprintf(stderr,"SYNC_AUDIT fsync_calls=%llu fsync_ns=%llu wal_calls=%llu wal_ns=%llu hot_calls=%llu hot_ns=%llu failures=%llu\n",calls.load(),ns.load(),wal_calls.load(),wal_ns.load(),hot_calls.load(),hot_ns.load(),failures.load());
}
__attribute__((used)) static struct {const void* replacement;const void* replacee;} interpose
__attribute__((section("__DATA,__interpose")))={reinterpret_cast<const void*>(&audit_sync),reinterpret_cast<const void*>(&fsync)};
}
