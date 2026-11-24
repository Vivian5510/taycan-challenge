#include "environment/native_file.h"
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <thread>
#include <cstdio>
#include <algorithm>
#include <array>
#ifdef F2_COMMIT_EXPERIMENT
#include "core/f2_commit_experiment.h"
#ifdef F2_BATCHED_FILE_SYNC
#include "core/f2_sync_experiment.h"
#endif
#endif

namespace FASTER { namespace environment {
using core::Status;
Status File::Open(FileCreateDisposition disposition, const FileOptions& options, bool* exists) {
  struct stat info{};
  const bool present = ::stat(filename_.c_str(), &info) == 0;
  if (exists) *exists = present;
  if (!present && disposition == FileCreateDisposition::OpenExisting && exists)
    return Status::Ok;
  int flags = O_RDWR | O_CLOEXEC;
  if (disposition == FileCreateDisposition::CreateOrTruncate) flags |= O_CREAT | O_TRUNC;
  else if (disposition == FileCreateDisposition::OpenOrCreate) flags |= O_CREAT;
  fd_ = ::open(filename_.c_str(), flags, 0600);
  if (fd_ < 0) return Status::IOError;
  bool unbuffered=options.unbuffered;
#ifdef F2_BUFFERED_HOT_LOG
  if(filename_.find("/hot/log.log")!=std::string::npos)unbuffered=false;
#endif
  if (unbuffered && ::fcntl(fd_, F_NOCACHE, 1) != 0) {
    Close();
    return Status::IOError;
  }
  // Upstream also carries this flag; the diagnostic backend does not use it.
  if (options.delete_on_close) {
    Close();
    throw std::runtime_error("native diagnostic backend does not support delete_on_close");
  }
  return Status::Ok;
}
Status File::Close() {
  if (fd_ < 0) return Status::Ok;
  const int descriptor = fd_;
  fd_ = -1;
  return ::close(descriptor) == 0 ? Status::Ok : Status::IOError;
}
Status File::Delete() {
  return ::unlink(filename_.c_str()) == 0 ? Status::Ok : Status::IOError;
}
#ifndef F2_NATIVE_POSIX_AIO
Status QueueIoHandler::Schedule(bool read, int fd, uint64_t offset, uint32_t length,
                               uint8_t* buffer, core::IAsyncContext& context,
                               core::AsyncIOCallback callback, const std::string& path) {
  core::IAsyncContext* copy = nullptr;
  const auto copied = context.DeepCopy(copy);
  if (copied != Status::Ok) return copied;
  std::lock_guard<std::mutex> lock(mutex_);
  jobs_.emplace_back([=] {
#ifdef F2_NATIVE_IO_PROFILE
    const auto started=std::chrono::steady_clock::now();
    const bool reader_write=!read && native_read_scope;
#endif
    size_t done = 0;
    Status status = Status::Ok;
    while (done < length) {
#ifdef F2_BATCHED_FILE_SYNC
      if(!read && std::getenv("F2_SYNC_FAIL_WRITE")){status=Status::IOError;break;}
#endif
      const ssize_t count = read
          ? ::pread(fd, buffer + done, length - done, off_t(offset + done))
          : ::pwrite(fd, buffer + done, length - done, off_t(offset + done));
#ifdef F2_COMMIT_EXPERIMENT
      if (!read) {
        ++core::experimental::write_calls;
        if (count > 0) core::experimental::write_bytes += uint64_t(count);
#ifdef F2_DATA_PREFIX
        if(count>0 && path.find("/hot/log.log")!=std::string::npos) {
          ++core::experimental::prefix_write_calls;
          core::experimental::prefix_write_bytes+=uint64_t(count);
        }
#endif
      }
#endif
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 || (!read && count == 0)) {
        status = Status::IOError;
        break;
      }
      if (count == 0) break;
      done += size_t(count);
    }
    if (!read && status == Status::Ok) {
#ifdef F2_COMMIT_EXPERIMENT
#ifdef F2_BATCHED_FILE_SYNC
#ifdef F2_DATA_PREFIX
      if(!core::experimental::MarkDirtyDataDescriptor(path,fd))status=Status::IOError;
#endif
      core::experimental::MarkDirty(path);
#else
      try { core::experimental::Sync(fd); } catch (...) { status = Status::IOError; }
#endif
#else
      int result;
      do {
#ifdef F2_MAC_STANDARD_SYNC
        result = ::fsync(fd);
#else
        result = ::fcntl(fd, F_FULLFSYNC);
#endif
      } while (result < 0 && errno == EINTR);
      if (result < 0) status = Status::IOError;
#endif
    }
#ifdef F2_BATCHED_FILE_SYNC
    if(status!=Status::Ok)core::experimental::MarkIoFailure(path);
#endif
#ifdef F2_NATIVE_IO_PROFILE
    if(!read)++write_completions_;
    if(reader_write){++read_thread_writes_;read_thread_write_ns_+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();}
#endif
    callback(copy, status, done);
  });
  return Status::Ok;
}
bool QueueIoHandler::TryComplete() {
  std::function<void()> job;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (jobs_.empty()) return false;
    job = std::move(jobs_.front());
    jobs_.pop_front();
  }
  job();
  return true;
}
#else
#include "native_aio_file.inc"
#endif
void QueueIoHandler::Drain() {
  for(;;) {
    {std::lock_guard<std::mutex> lock(mutex_);
#ifdef F2_NATIVE_POSIX_AIO
      if(aio_jobs_.empty())break;
#else
      if(jobs_.empty())break;
#endif
    }
    if(!TryComplete())std::this_thread::yield();
  }
}
QueueIoHandler::~QueueIoHandler() {
#if F2_AIO_COMPLETION_BATCH > 1
  std::fprintf(stderr,"F2_AIO_BATCH limit=%u polls=%llu callbacks=%llu multi_polls=%llu\n",
    F2_AIO_COMPLETION_BATCH,(unsigned long long)aio_completion_polls_,
    (unsigned long long)aio_completion_callbacks_,(unsigned long long)aio_multi_completion_polls_);
#endif
  // FasterKv drains before destroying its hash/log buffers. A handler with
  // live requests at this point is a lifetime bug, never silently drop it.
#ifdef F2_NATIVE_POSIX_AIO
  if(!aio_jobs_.empty())std::terminate();
#endif
#ifdef F2_NATIVE_IO_PROFILE
  std::fprintf(stderr,"F2_NATIVE_IO mode=%u read_thread_writes=%llu read_thread_write_ns=%llu write_completions=%llu",
#ifdef F2_NATIVE_POSIX_AIO
      1,
#else
      0,
#endif
      (unsigned long long)read_thread_writes_.load(),(unsigned long long)read_thread_write_ns_.load(),
      (unsigned long long)write_completions_.load());
#ifdef F2_NATIVE_POSIX_AIO
  std::fprintf(stderr," submissions=%llu retries=%llu peak_inflight=%zu",(unsigned long long)aio_submissions_,(unsigned long long)aio_retries_,aio_peak_);
#endif
  std::fprintf(stderr,"\n");
#endif
}
}}
