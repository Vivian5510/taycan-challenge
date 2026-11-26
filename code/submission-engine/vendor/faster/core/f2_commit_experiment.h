// Experimental F2-owned commit support. Copied only into an opt-in build tree.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <nmmintrin.h>
#endif
#ifdef __linux__
#include <linux/fs.h>
#include <libaio.h>
#include <sys/uio.h>
#endif

namespace FASTER { namespace core { namespace experimental {
inline std::atomic<uint64_t> sync_calls{0}, sync_ns{0}, write_calls{0}, write_bytes{0};
#ifdef F2_WAL_TEST_HOOKS
inline std::atomic<bool> wal_defer_next{false},wal_deferred{false},wal_release_deferred{false},wal_later_completed{false};
#endif
inline void Sync(int fd, bool directory = false) {
  if(std::getenv("F2_SYNC_FAIL_SYNC")) throw std::runtime_error("injected sync error");
  const auto begin = std::chrono::steady_clock::now();
  int code;
  do {
#ifdef __APPLE__
#ifdef F2_MAC_STANDARD_SYNC
    code = ::fsync(fd);
#else
    code = directory ? ::fsync(fd) : ::fcntl(fd, F_FULLFSYNC);
#endif
#else
    code = directory ? ::fsync(fd) : ::fdatasync(fd);
#endif
  } while (code < 0 && errno == EINTR);
  ++sync_calls;
  sync_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - begin).count();
  if (code < 0) throw std::runtime_error("commit sync failed");
}
inline void SyncDirectory(const std::filesystem::path& path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("open commit directory failed");
  try { Sync(fd, true); } catch (...) { ::close(fd); throw; }
  ::close(fd);
}
inline void WriteAll(int fd, const char* data, size_t size) {
  if(std::getenv("F2_SYNC_FAIL_WRITE"))throw std::runtime_error("injected WAL write error");
  while (size) {
    ssize_t n = ::write(fd, data, size);
    ++write_calls;
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("commit write failed");
    write_bytes += size_t(n);
    data += n;
    size -= size_t(n);
  }
}
inline uint32_t CRCSoftware(const char* data, size_t size) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> result{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (unsigned bit = 0; bit < 8; ++bit) c = (c >> 1) ^ ((c & 1) ? 0x82f63b78U : 0);
      result[i] = c;
    }
    return result;
  }();
  uint32_t c = ~0U;
  for (size_t i = 0; i < size; ++i) c = table[(c ^ uint8_t(data[i])) & 255] ^ (c >> 8);
  return ~c;
}
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("sse4.2"))) inline uint32_t CRCHardware(const char* data,size_t size) {
  uint64_t crc=~uint32_t{0};
  while(size>=8) {
    uint64_t word;std::memcpy(&word,data,8);crc=_mm_crc32_u64(crc,word);
    data+=8;size-=8;
  }
  uint32_t tail=uint32_t(crc);
  while(size--)tail=_mm_crc32_u8(tail,uint8_t(*data++));
  return ~tail;
}
#endif
inline uint32_t CRC(const char* data,size_t size) {
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  static const bool hardware=[] {
    const char* mode=std::getenv("F2_WAL_CRC_HW");
    return (!mode||std::strcmp(mode,"0")!=0)&&__builtin_cpu_supports("sse4.2");
  }();
  if(hardware)return CRCHardware(data,size);
#endif
  return CRCSoftware(data,size);
}
inline void Encode(char* out, uint64_t value, size_t n) {
  for (size_t i = 0; i < n; ++i) out[i] = char(value >> (8*i));
}
inline uint64_t Decode(const char* in, size_t n) {
  uint64_t value = 0;
  for (size_t i = 0; i < n; ++i) value |= uint64_t(uint8_t(in[i])) << (8*i);
  return value;
}

// One lane has one owner session. Writers on separate lanes share no write lock.
// Stage copies complete arbitrary bytes into a reusable, bounded batch buffer.
class BatchLog {
 public:
  static constexpr size_t kHeader = 36, kRecord = 24, kMaxFrame = 1 << 20;
  BatchLog(const std::filesystem::path& directory, uint32_t lane, bool prewritten = false,
           bool pipeline_format = false)
      : lane_(lane), prewritten_(prewritten), pipeline_format_(pipeline_format) {
#ifdef __linux__
    auto enabled=[](const char* name,bool fallback){const char* value=std::getenv(name);return value?value[0]=='1'&&!value[1]:fallback;};
#ifdef F2_WAL_OPTIMIZED
    const bool optimized=prewritten_;
#else
    const bool optimized=false;
#endif
    dsync_=enabled("F2_WAL_RWF_DSYNC",optimized);
    direct_=enabled("F2_WAL_DIRECT",optimized&&dsync_);
    aio_=enabled("F2_WAL_AIO",optimized&&direct_);
    if(aio_&&!direct_)throw std::runtime_error("AIO WAL requires direct durable writes");
    if(dsync_&&!prewritten_)
      throw std::runtime_error("durable WAL sync requires prewritten block frames");
    if(direct_&&(!prewritten_||!dsync_))
      throw std::runtime_error("direct WAL requires prewritten durable frames");
#endif
    auto path = directory / ("F2-COMMIT-" + std::to_string(lane));
    // Extent preparation already has one explicit Sync(). Keep its descriptor
    // buffered so a 4 MiB zero write does not acquire a second durability barrier.
    fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC |
                 (prewritten_ ? 0 : O_APPEND), 0600);
    if (fd_ < 0) throw std::runtime_error("open F2 commit log failed");
    if (prewritten_) {
      struct statvfs fs {};
      if (::fstatvfs(fd_, &fs) < 0 || !fs.f_frsize || kBlockBytes % fs.f_frsize) {
        ::close(fd_);
        throw std::runtime_error("prewritten WAL requires compatible filesystem block size");
      }
    }
    buffer_.reserve(kMaxFrame);
    buffer_.resize(kHeader);
#ifdef __linux__
    if(dsync_) {
      // O_DSYNC is per open-file description; it also governs Linux AIO writes.
      // Use a separate descriptor for durable frames and keep allocation above
      // on its original write + fdatasync path.
      direct_fd_=::open(path.c_str(),O_RDWR|O_DSYNC|O_CLOEXEC|(direct_?O_DIRECT:0));
      if(direct_fd_<0) {::close(fd_);fd_=-1;throw std::runtime_error("open durable WAL failed");}
    }
    if(direct_) {
      void* memory=nullptr;
      if(::posix_memalign(&memory,kBlockBytes,kMaxFrame+2*kBlockBytes)!=0) {
        ::close(direct_fd_);::close(fd_);direct_fd_=fd_=-1;
        throw std::runtime_error("allocate aligned WAL buffer failed");
      }
      direct_buffer_.reset(static_cast<char*>(memory));
      if(aio_&&::io_setup(pipeline_format_?8:2,&aio_context_)!=0) {
        ::close(direct_fd_);::close(fd_);direct_fd_=fd_=-1;
        throw std::runtime_error("create WAL AIO context failed");
      }
    }
#endif
  }
  ~BatchLog() {
#ifdef __linux__
    if(aio_context_)::io_destroy(aio_context_);
#endif
    if(direct_fd_>=0)::close(direct_fd_);if (fd_ >= 0) ::close(fd_);
  }
  BatchLog(const BatchLog&) = delete;
  // Stable storage for a submitted frame, including its iovec and iocb. The
  // log retains it until completion; caller request memory is never used by I/O.
  struct PendingWrite {
    std::unique_ptr<char,void(*)(void*)> data{nullptr,::free};
#ifdef __linux__
    struct iovec vector{};
    struct iocb request{};
#endif
    uint64_t first=0,last=0,offset=0;
    size_t size=0;
    bool done=false;
    bool deferred=false;
    int64_t result=0;
    std::chrono::steady_clock::time_point submitted;
  };
  bool CanPipeline() const {return pipeline_format_&&prewritten_&&dsync_&&direct_&&aio_;}
  bool CanOverlapApply() const {return prewritten_&&dsync_&&direct_&&aio_;}
  size_t AsyncInFlight() const {return async_inflight_.load(std::memory_order_acquire);}
  std::shared_ptr<PendingWrite> Submit() {
#ifdef __linux__
    if(!CanPipeline()||failed_||!count_||async_writes_.size()>=8)
      throw std::runtime_error("invalid asynchronous WAL submission");
    if(std::getenv("F2_SYNC_FAIL_WRITE")||std::getenv("F2_SYNC_FAIL_SYNC"))
      throw std::runtime_error("injected WAL durable write error");
    const auto begin=std::chrono::steady_clock::now();
    auto job=std::make_shared<PendingWrite>();
    job->last=last_;job->first=last_-count_+1;
    char* out=buffer_.data();
    Encode(out,0x32434d46,4);Encode(out+4,1,4);
    Encode(out+8,buffer_.size()+4,4);Encode(out+12,count_,4);
    Encode(out+16,lane_,4);Encode(out+20,0,4);Encode(out+24,last_,8);
    Encode(out+32,CRC(out,32),4);
    const uint32_t crc=CRC(out,buffer_.size());
    const size_t end=buffer_.size();buffer_.resize(end+4);
    Encode(buffer_.data()+end,crc,4);EncodeBlocks(buffer_);
    job->size=buffer_.size();job->offset=offset_.load(std::memory_order_relaxed);
    void* memory=nullptr;
    if(::posix_memalign(&memory,kBlockBytes,job->size)!=0)
      throw std::runtime_error("allocate pipeline WAL frame failed");
    job->data.reset(static_cast<char*>(memory));
    std::memcpy(job->data.get(),buffer_.data(),job->size);
    encode_ns_+=Elapsed(begin);
    const auto prepare_begin=std::chrono::steady_clock::now();
    PrepareExtent(job->offset+job->size);prepare_ns_+=Elapsed(prepare_begin);
    job->vector={job->data.get(),job->size};
#ifdef F2_WAL_TEST_HOOKS
    if(std::getenv("F2_WAL_SHORT_WRITE")&&job->size>kBlockBytes)job->vector.iov_len-=kBlockBytes;
#endif
    ::io_prep_pwritev(&job->request,direct_fd_,&job->vector,1,off_t(job->offset));
    job->request.data=job.get();
    struct iocb* requests[]={&job->request};
    // Allocate the retaining deque entry before handing its buffer to the kernel.
    async_writes_.push_back(job);job->submitted=std::chrono::steady_clock::now();
    int submitted=1;
#ifdef F2_WAL_TEST_HOOKS
    if(wal_defer_next.exchange(false)) {job->deferred=true;wal_deferred=true;}
    else
#endif
    {do{submitted=::io_submit(aio_context_,1,requests);}while(submitted==-EINTR);}
    submit_ns_+=Elapsed(job->submitted);
    if(submitted!=1) {async_writes_.pop_back();failed_=true;throw std::runtime_error("submit pipeline WAL AIO failed");}
    const auto active=job->deferred?async_inflight_.load():async_inflight_.fetch_add(1,std::memory_order_release)+1;
    max_inflight_=std::max<uint64_t>(max_inflight_,active);
    if(active>1)++overlap_submissions_;
    offset_.store(job->offset+job->size,std::memory_order_relaxed);
    ++write_calls;++dsync_calls_;buffer_.resize(kHeader);count_=0;
    return job;
#else
    throw std::runtime_error("pipeline WAL requires Linux");
#endif
  }
  void Poll() {
#ifdef __linux__
#ifdef F2_WAL_TEST_HOOKS
    for(const auto& job:async_writes_)if(job->deferred&&wal_release_deferred.load()) {
      struct iocb* requests[]={&job->request};int submitted;
      do{submitted=::io_submit(aio_context_,1,requests);}while(submitted==-EINTR);
      if(submitted!=1)throw std::runtime_error("submit deferred WAL failed");
      job->deferred=false;async_inflight_.fetch_add(1,std::memory_order_release);
    }
#endif
    struct io_event events[8]{};struct timespec zero{};
    int count=::io_getevents(aio_context_,0,8,events,&zero);
    if(count<0) {if(count==-EINTR)return;failed_=true;throw std::runtime_error("poll pipeline WAL failed");}
    for(int i=0;i<count;++i) {
      auto* job=static_cast<PendingWrite*>(events[i].data);
      if(job->done)throw std::runtime_error("duplicate pipeline WAL completion");
      job->result=events[i].res2==0?int64_t(events[i].res):-1;job->done=true;
      async_inflight_.fetch_sub(1,std::memory_order_release);
      if(job->result>0)write_bytes+=uint64_t(job->result);
      persist_ns_+=Elapsed(job->submitted);
      if(!async_writes_.empty()&&job!=async_writes_.front().get()&&!async_writes_.front()->done)++out_of_order_;
#ifdef F2_WAL_TEST_HOOKS
      if(!async_writes_.empty()&&async_writes_.front()->deferred&&job!=async_writes_.front().get())wal_later_completed=true;
#endif
    }
#endif
  }
  void Complete(const std::shared_ptr<PendingWrite>& job) {
    if(async_writes_.empty()||async_writes_.front()!=job||!job->done||job->result!=int64_t(job->size)) {
      failed_=true;throw std::runtime_error("pipeline WAL incomplete, short, or out of order publication");
    }
    durable_=job->last;++commits_;async_writes_.pop_front();
  }
  void Stage(uint32_t op, const std::string& key, const std::string& value, uint64_t serial) {
    if (failed_ || (op != 1 && op != 2) || key.empty() || (op == 2 && !value.empty()) ||
        serial <= last_ || buffer_.size()+kRecord+key.size()+value.size()+4 > kMaxFrame)
      throw std::runtime_error("invalid F2 commit record or batch too large");
    const size_t at = buffer_.size();
    buffer_.resize(at+kRecord+key.size()+value.size());
    char* out = buffer_.data()+at;
    Encode(out, op, 4); Encode(out+4, key.size(), 4); Encode(out+8, value.size(), 4);
    Encode(out+12, 0, 4); Encode(out+16, serial, 8);
    std::memcpy(out+kRecord, key.data(), key.size());
    std::memcpy(out+kRecord+key.size(), value.data(), value.size());
    last_ = serial;
    ++count_;
  }
  uint64_t Commit(const std::function<void()>& after_submit={}) {
    if (failed_) throw std::runtime_error("F2 commit lane is poisoned");
    if(after_submit&&!CanOverlapApply())throw std::runtime_error("WAL/apply overlap requires direct durable AIO");
    if (!count_) return durable_;
    const auto encode_begin=std::chrono::steady_clock::now();
    char* out = buffer_.data();
    Encode(out, 0x32434d46, 4); Encode(out+4, 1, 4);
    Encode(out+8, buffer_.size()+4, 4); Encode(out+12, count_, 4);
    Encode(out+16, lane_, 4); Encode(out+20, 0, 4); Encode(out+24, last_, 8);
    Encode(out+32, CRC(out, 32), 4);
    const auto crc = CRC(out, buffer_.size());
    const size_t end = buffer_.size(); buffer_.resize(end+4);
    Encode(buffer_.data()+end, crc, 4);
    const uint64_t offset = offset_.load(std::memory_order_relaxed);
    try {
      if (prewritten_) {
        // Separate frames at filesystem block boundaries so a write to the
        // next frame cannot tear a block of the preceding committed frame.
        EncodeBlocks(buffer_);
        encode_ns_+=Elapsed(encode_begin);
        const auto prepare_begin=std::chrono::steady_clock::now();
        PrepareExtent(offset + buffer_.size());
        prepare_ns_+=Elapsed(prepare_begin);
        const auto persist_begin=std::chrono::steady_clock::now();
        if(dsync_)WriteDurable(buffer_.data(),buffer_.size(),offset,after_submit);
        else {WriteAt(buffer_.data(), buffer_.size(), offset);Sync(fd_);}
        persist_ns_+=Elapsed(persist_begin);
      } else {
        encode_ns_+=Elapsed(encode_begin);
        const auto persist_begin=std::chrono::steady_clock::now();
        if(dsync_)WriteDurable(buffer_.data(),buffer_.size(),uint64_t(-1));
        else {WriteAll(fd_, buffer_.data(), buffer_.size());Sync(fd_);}
        persist_ns_+=Elapsed(persist_begin);
      }
      if (prewritten_) offset_.store(offset + buffer_.size(), std::memory_order_relaxed);
    }
    catch (...) { failed_ = true; throw; }
    durable_ = last_;
    ++commits_;
    buffer_.resize(kHeader); count_ = 0;
    return durable_;
  }
  template<class Apply>
  uint64_t Recover(Apply apply, uint64_t skip_until = 0) {
    uint64_t offset = 0, records = 0;
    auto read = [&](char* out, size_t bytes, uint64_t pos) {
      size_t done = 0;
      while (done < bytes) {
        auto n = ::pread(fd_, out+done, bytes-done, off_t(pos+done));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throw std::runtime_error("read F2 commit log failed");
        if (!n) break;
        done += size_t(n);
      }
      return done;
    };
    auto has_frame_header = [&](const char* data) {
      return Decode(data,4) == 0x32434d46 && Decode(data+4,4) == 1 &&
             Decode(data+16,4) == lane_ && Decode(data+32,4) == CRC(data,32);
    };
    auto check_prewritten_tail = [&](uint64_t next_slot) {
      // v3 may contain a complete later frame behind an uncommitted hole. No
      // request after a hole was ACKed: apply/ACK publication is strictly FIFO.
      if(pipeline_format_)return;
      const uint64_t end = FileSize();
      std::array<char, kHeader> header;
      for (uint64_t slot = next_slot; slot + kHeader <= end; slot += kBlockBytes) {
        if (read(header.data(), header.size(), slot) == header.size() && has_frame_header(header.data()))
          throw std::runtime_error("corrupt prewritten WAL before a later frame");
      }
    };
    std::vector<char> frame(kHeader);
    while (true) {
      const auto n = read(frame.data(), kHeader, offset);
      if (!n) break;
      if (n != kHeader) { Repair(offset); break; }
      if (prewritten_ && std::all_of(frame.begin(), frame.end(), [](char c) { return c == 0; })) {
        check_prewritten_tail(offset + kBlockBytes);
        if(pipeline_format_)Repair(offset);
        break;
      }
      if (!has_frame_header(frame.data())) {
        if (prewritten_) {
          check_prewritten_tail(offset + kBlockBytes);
          Repair(offset);
          break;
        }
        throw std::runtime_error("corrupt F2 commit header");
      }
      const size_t size = Decode(frame.data()+8,4), count = Decode(frame.data()+12,4);
      if (size < kHeader+4 || size > kMaxFrame || !count)
        throw std::runtime_error("invalid F2 commit frame size");
      const size_t stored_size = prewritten_ ? AlignedSize(size) : size;
      frame.resize(stored_size);
      if (read(frame.data()+kHeader,stored_size-kHeader,offset+kHeader) != stored_size-kHeader) {
        Repair(offset); break;
      }
      const bool blocks_valid = !prewritten_ || DecodeBlocks(frame, size);
      if (!blocks_valid || Decode(frame.data()+size-4,4) != CRC(frame.data(),size-4)) {
        if (prewritten_) {
          check_prewritten_tail(offset + stored_size);
          Repair(offset);
          break;
        }
        throw std::runtime_error("corrupt F2 commit payload");
      }
      size_t at = kHeader;
      for (size_t i=0; i<count; ++i) {
        if (at+kRecord > size-4) throw std::runtime_error("short F2 commit record");
        const char* p = frame.data()+at;
        const auto op = Decode(p,4), k = Decode(p+4,4), v = Decode(p+8,4), serial = Decode(p+16,8);
        if ((op != 1 && op != 2) || !k || (op == 2 && v) || k+v > size-4-at-kRecord || serial <= last_)
          throw std::runtime_error("invalid F2 commit record");
        if (serial > skip_until) apply(uint32_t(op), std::string(p+kRecord,k), std::string(p+kRecord+k,v));
        last_ = serial; ++records; at += kRecord+k+v;
      }
      if (at != size-4 || last_ != Decode(frame.data()+24,8))
        throw std::runtime_error("F2 commit frame count mismatch");
      durable_ = last_; offset += stored_size;
      frame.resize(kHeader);
    }
    if (prewritten_) {
      offset_.store(offset, std::memory_order_relaxed);
      allocated_ = FileSize();
    }
    return records;
  }
  uint64_t last_serial() const { return last_; }
  void PrintProfile() const {
    std::fprintf(stderr,"F2_WAL_WRITE commits=%llu prewritten=%u dsync=%u direct=%u aio=%u encode_ns=%llu prepare_ns=%llu persist_ns=%llu dsync_calls=%llu\n",
      (unsigned long long)commits_,unsigned(prewritten_),unsigned(dsync_),unsigned(direct_),
      unsigned(aio_),
      (unsigned long long)encode_ns_,(unsigned long long)prepare_ns_,
      (unsigned long long)persist_ns_,(unsigned long long)dsync_calls_);
    if(pipeline_format_)std::fprintf(stderr,"F2_WAL_AIO_PIPELINE max_inflight=%llu overlap_submissions=%llu completion_reorder=%llu submit_ns=%llu\n",
      (unsigned long long)max_inflight_,(unsigned long long)overlap_submissions_,
      (unsigned long long)out_of_order_,(unsigned long long)submit_ns_);
    if(overlap_calls_)std::fprintf(stderr,"F2_WAL_SAME_BATCH calls=%llu apply_callback_ns=%llu wait_after_apply_ns=%llu ready_at_first_poll=%llu\n",
      (unsigned long long)overlap_calls_,(unsigned long long)overlap_apply_ns_,
      (unsigned long long)overlap_wait_ns_,(unsigned long long)overlap_ready_);
  }
  uint64_t Bytes() const {
    return prewritten_ ? offset_.load(std::memory_order_relaxed) : FileSize();
  }
  void ResetAfterCheckpoint() {
    if (count_||!async_writes_.empty()) throw std::runtime_error("cannot reset uncommitted log batch");
    Repair(0);
    offset_.store(0, std::memory_order_relaxed);
    allocated_ = 0;
  }
 private:
  static constexpr size_t kBlockBytes = 4096;
  static constexpr size_t kContinuationBytes = kBlockBytes - 1;
  static constexpr size_t kExtentBytes = 4 << 20;
  static uint64_t Elapsed(std::chrono::steady_clock::time_point from) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-from).count();
  }
  void WriteDurable(const char* data,size_t size,uint64_t offset,const std::function<void()>& after_submit={}) {
#ifdef __linux__
    if(std::getenv("F2_SYNC_FAIL_WRITE")||std::getenv("F2_SYNC_FAIL_SYNC"))
      throw std::runtime_error("injected WAL durable write error");
    if(direct_) {
      if(size>kMaxFrame+2*kBlockBytes||size%kBlockBytes||offset%kBlockBytes)
        throw std::runtime_error("unaligned direct WAL frame");
      std::memcpy(direct_buffer_.get(),data,size);data=direct_buffer_.get();
    }
    struct iovec vector{const_cast<char*>(data),size};
#ifdef F2_WAL_TEST_HOOKS
    if(std::getenv("F2_WAL_SHORT_WRITE")&&size>kBlockBytes)vector.iov_len-=kBlockBytes;
#endif
    ssize_t written;
    std::exception_ptr apply_error;
    if(aio_) {
      struct iocb request{};
      ::io_prep_pwritev(&request,direct_fd_,&vector,1,off_t(offset));
      struct iocb* requests[]={&request};
      int submitted;
#ifdef F2_WAL_TEST_HOOKS
      bool deferred=wal_defer_next.exchange(false);
      if(deferred) {submitted=1;wal_deferred=true;}
      else
#endif
      do{submitted=::io_submit(aio_context_,1,requests);}while(submitted==-EINTR);
      if(submitted!=1)throw std::runtime_error("submit durable WAL AIO failed");
      if(after_submit) {
        const auto begin=std::chrono::steady_clock::now();
        try{after_submit();}catch(...){apply_error=std::current_exception();}
        ++overlap_calls_;overlap_apply_ns_+=Elapsed(begin);
      }
      const auto wait_begin=std::chrono::steady_clock::now();
      struct io_event event{};struct timespec zero{};
      bool first_poll=true;
      for(;;) {
#ifdef F2_WAL_TEST_HOOKS
        if(deferred) {
          if(!wal_release_deferred.load(std::memory_order_acquire)) {std::this_thread::yield();continue;}
          do{submitted=::io_submit(aio_context_,1,requests);}while(submitted==-EINTR);
          if(submitted!=1)throw std::runtime_error("submit deferred same-batch WAL failed");
          deferred=false;
        }
#endif
        int count=::io_getevents(aio_context_,1,1,&event,&zero);
        if(after_submit&&first_poll&&count==1)++overlap_ready_;
        first_poll=false;
        if(count==1)break;
        if(count<0&&count!=-EINTR)throw std::runtime_error("complete durable WAL AIO failed");
#if defined(__x86_64__)
        __builtin_ia32_pause();
#endif
      }
      if(after_submit)overlap_wait_ns_+=Elapsed(wait_begin);
      written=event.res2==0?ssize_t(event.res):-1;
    } else {
      do {written=::pwritev(direct_fd_,&vector,1,off_t(offset));}
      while(written<0&&errno==EINTR);
    }
    ++write_calls;++dsync_calls_;
    if(written>0)write_bytes+=uint64_t(written);
    // A short durable write is an unknown in-flight operation; fail closed.
    // The block-framed recovery path validates and discards its incomplete tail.
    if(written!=ssize_t(size))throw std::runtime_error("durable WAL write failed or was short");
    // Even if F2 apply threw, the stack iocb/iovec and shared direct buffer must
    // stay alive until the submitted I/O completes. Only then propagate failure.
    if(apply_error)std::rethrow_exception(apply_error);
#else
    throw std::runtime_error("durable WAL writes require Linux");
#endif
  }
  static size_t AlignedSize(size_t size) {
    if (size <= kBlockBytes) return kBlockBytes;
    const size_t continuation = size - kBlockBytes;
    return kBlockBytes * (1 + (continuation + kContinuationBytes - 1) / kContinuationBytes);
  }
  static void EncodeBlocks(std::vector<char>& frame) {
    const size_t size = frame.size();
    frame.resize(AlignedSize(size), 0);
    // Reserve the first byte of each continuation block so a frame header
    // inside an arbitrary value cannot look like the start of another batch.
    for (size_t block = frame.size() / kBlockBytes; --block > 0;) {
      const size_t start = block * kBlockBytes;
      const size_t source = start - (block - 1);
      const size_t bytes = std::min(size - source, kContinuationBytes);
      std::memmove(frame.data() + start + 1, frame.data() + source, bytes);
      frame[start] = 0;
    }
  }
  static bool DecodeBlocks(std::vector<char>& frame, size_t size) {
    for (size_t start = kBlockBytes; start < frame.size(); start += kBlockBytes) {
      if (frame[start] != 0) return false;
      const size_t destination = start - (start / kBlockBytes - 1);
      const size_t bytes = std::min(size - destination, kContinuationBytes);
      std::memmove(frame.data() + destination, frame.data() + start + 1, bytes);
    }
    return true;
  }
  uint64_t FileSize() const {
    struct stat st{};if(::fstat(fd_,&st)) throw std::runtime_error("stat commit log failed");return uint64_t(st.st_size);
  }
  void WriteAt(const char* data, size_t size, uint64_t offset) {
    if(std::getenv("F2_SYNC_FAIL_WRITE"))throw std::runtime_error("injected WAL pwrite error");
    while (size) {
      const ssize_t n = ::pwrite(fd_, data, size, off_t(offset));
      ++write_calls;
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) throw std::runtime_error("commit pwrite failed");
      write_bytes += size_t(n);
      data += n;
      size -= size_t(n);
      offset += size_t(n);
    }
  }
  void PrepareExtent(uint64_t required) {
    if (required <= allocated_) return;
    static const std::array<char, kExtentBytes> zeros{};
    while(required>allocated_) {
      WriteAt(zeros.data(), zeros.size(), allocated_);
      Sync(fd_);
      allocated_+=kExtentBytes;
    }
  }
  void Repair(uint64_t offset) {
    if (::ftruncate(fd_, off_t(offset)) < 0) throw std::runtime_error("repair commit tail failed");
    Sync(fd_);
  }
  int fd_ = -1;
  int direct_fd_=-1;
  struct FreeBuffer {void operator()(char* pointer) const {::free(pointer);}};
  std::unique_ptr<char,FreeBuffer> direct_buffer_;
  bool dsync_=false,direct_=false;
  bool aio_=false;
#ifdef __linux__
  io_context_t aio_context_=0;
#endif
  uint64_t commits_=0,encode_ns_=0,prepare_ns_=0,persist_ns_=0,dsync_calls_=0;
  std::deque<std::shared_ptr<PendingWrite>> async_writes_;
  std::atomic<size_t> async_inflight_{0};
  uint64_t max_inflight_=0,overlap_submissions_=0,out_of_order_=0,submit_ns_=0;
  uint64_t overlap_calls_=0,overlap_apply_ns_=0,overlap_wait_ns_=0,overlap_ready_=0;
  uint32_t lane_;
  bool prewritten_;
  bool pipeline_format_=false;
  // Pipeline apply can check the reclaim limit while a later batch syncs.
  std::atomic<uint64_t> offset_{0};
  uint64_t allocated_ = 0;
  std::vector<char> buffer_;
  uint64_t last_ = 0, durable_ = 0, count_ = 0;
  bool failed_ = false;
};

struct CheckpointWaiter {
  bool done = false;
  Status status = Status::Corruption;
  uint64_t serial = 0;
};
inline thread_local CheckpointWaiter waiter;
inline void CheckpointDone(Status status, uint64_t serial) {
  waiter = {true, status, serial};
}
}}} // FASTER::core::experimental
