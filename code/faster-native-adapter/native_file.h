// macOS diagnostic backend; not an upstream FASTER performance implementation.
#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <atomic>
#include <list>
#include <chrono>
#ifdef F2_NATIVE_POSIX_AIO
#include <aio.h>
#include <signal.h>
#endif
#include <unistd.h>
#include "file_common.h"

namespace FASTER { namespace environment {
constexpr const char* kPathSeparator = "/";
class QueueFile;
#ifdef F2_NATIVE_IO_PROFILE
inline thread_local unsigned native_read_scope=0;
struct NativeIoReadScope {
  NativeIoReadScope(){++native_read_scope;}
  ~NativeIoReadScope(){--native_read_scope;}
};
#endif
class QueueIoHandler {
 public:
  using async_file_t = QueueFile;
  QueueIoHandler() = default;
  explicit QueueIoHandler(size_t) {}
  ~QueueIoHandler();
  void Drain();
  bool TryComplete();
  core::Status Schedule(bool read, int fd, uint64_t offset, uint32_t length,
                        uint8_t* buffer, core::IAsyncContext& context,
                        core::AsyncIOCallback callback, const std::string& path = {});
 private:
  std::mutex mutex_;
  std::deque<std::function<void()>> jobs_;
#ifdef F2_NATIVE_IO_PROFILE
  std::atomic<uint64_t> read_thread_writes_{0},read_thread_write_ns_{0},write_completions_{0};
#endif
#ifdef F2_NATIVE_POSIX_AIO
  struct AioJob {
    struct aiocb control{};
    bool read=false,submitted=false,inline_completed=false;
    int fd=-1,error=0;
    uint64_t offset=0;
    uint32_t length=0,done=0;
    uint8_t* buffer=nullptr;
    core::IAsyncContext* context=nullptr;
    core::AsyncIOCallback callback=nullptr;
    std::string path;
    ~AioJob(){if(fd>=0)::close(fd);}
  };
  void SubmitQueuedLocked();
  std::list<std::unique_ptr<AioJob>> aio_jobs_;
  size_t aio_inflight_=0,aio_peak_=0;
  uint64_t aio_submissions_=0,aio_retries_=0;
  uint64_t aio_completion_polls_=0,aio_completion_callbacks_=0,aio_multi_completion_polls_=0;
#endif
};

class File {
 public:
  File() = default;
  explicit File(std::string name) : filename_(std::move(name)) {}
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&& other) noexcept : fd_(other.fd_), filename_(std::move(other.filename_)) {
    other.fd_ = -1;
  }
  File& operator=(File&& other) noexcept {
    if (this != &other) {
      Close();
      fd_ = other.fd_;
      filename_ = std::move(other.filename_);
      other.fd_ = -1;
    }
    return *this;
  }
  ~File() { Close(); }
  core::Status Close();
  core::Status Delete();
  uint64_t size() const {
    struct stat info{};
    return ::fstat(fd_, &info) == 0 ? uint64_t(info.st_size) : 0;
  }
  size_t device_alignment() const { return 512; }
  const std::string& filename() const { return filename_; }
 protected:
  core::Status Open(FileCreateDisposition disposition, const FileOptions& options, bool* exists);
  int fd_ = -1;
  std::string filename_;
};

class QueueFile : public File {
 public:
  QueueFile() = default;
  explicit QueueFile(const std::string& name) : File(name) {}
  QueueFile(QueueFile&& other) noexcept : File(std::move(other)), handler_(other.handler_) {}
  QueueFile& operator=(QueueFile&& other) noexcept {
    File::operator=(std::move(other));
    handler_ = other.handler_;
    return *this;
  }
  core::Status Open(FileCreateDisposition disposition, const FileOptions& options,
                    QueueIoHandler* handler, bool* exists = nullptr) {
    handler_ = handler;
    return File::Open(disposition, options, exists);
  }
  core::Status Read(size_t offset, uint32_t length, uint8_t* buffer,
                    core::IAsyncContext& context, core::AsyncIOCallback callback) const {
    return handler_->Schedule(true, fd_, offset, length, buffer, context, callback, filename_);
  }
  core::Status Write(size_t offset, uint32_t length, const uint8_t* buffer,
                     core::IAsyncContext& context, core::AsyncIOCallback callback) {
    return handler_->Schedule(false, fd_, offset, length, const_cast<uint8_t*>(buffer), context, callback, filename_);
  }
 private:
  QueueIoHandler* handler_ = nullptr;
};
}}
