// Experimental F2-owned commit support. Copied only into an opt-in build tree.
#pragma once
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace FASTER { namespace core { namespace experimental {
inline std::atomic<uint64_t> sync_calls{0}, sync_ns{0}, write_calls{0}, write_bytes{0};
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
inline uint32_t CRC(const char* data, size_t size) {
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
  BatchLog(const std::filesystem::path& directory, uint32_t lane) : lane_(lane) {
    auto path = directory / ("F2-COMMIT-" + std::to_string(lane));
    fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_APPEND | O_CLOEXEC, 0600);
    if (fd_ < 0) throw std::runtime_error("open F2 commit log failed");
    buffer_.reserve(kMaxFrame);
    buffer_.resize(kHeader);
  }
  ~BatchLog() { if (fd_ >= 0) ::close(fd_); }
  BatchLog(const BatchLog&) = delete;
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
  uint64_t Commit() {
    if (failed_) throw std::runtime_error("F2 commit lane is poisoned");
    if (!count_) return durable_;
    char* out = buffer_.data();
    Encode(out, 0x32434d46, 4); Encode(out+4, 1, 4);
    Encode(out+8, buffer_.size()+4, 4); Encode(out+12, count_, 4);
    Encode(out+16, lane_, 4); Encode(out+20, 0, 4); Encode(out+24, last_, 8);
    Encode(out+32, CRC(out, 32), 4);
    const auto crc = CRC(out, buffer_.size());
    const size_t end = buffer_.size(); buffer_.resize(end+4);
    Encode(buffer_.data()+end, crc, 4);
    try { WriteAll(fd_, buffer_.data(), buffer_.size()); Sync(fd_); }
    catch (...) { failed_ = true; throw; }
    durable_ = last_;
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
    std::vector<char> frame(kHeader);
    while (true) {
      const auto n = read(frame.data(), kHeader, offset);
      if (!n) break;
      if (n != kHeader) { Repair(offset); break; }
      if (Decode(frame.data(),4) != 0x32434d46 || Decode(frame.data()+4,4) != 1 ||
          Decode(frame.data()+16,4) != lane_ || Decode(frame.data()+32,4) != CRC(frame.data(),32))
        throw std::runtime_error("corrupt F2 commit header");
      const size_t size = Decode(frame.data()+8,4), count = Decode(frame.data()+12,4);
      if (size < kHeader+4 || size > kMaxFrame || !count)
        throw std::runtime_error("invalid F2 commit frame size");
      frame.resize(size);
      if (read(frame.data()+kHeader,size-kHeader,offset+kHeader) != size-kHeader) {
        Repair(offset); break;
      }
      if (Decode(frame.data()+size-4,4) != CRC(frame.data(),size-4))
        throw std::runtime_error("corrupt F2 commit payload");
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
      durable_ = last_; offset += size;
      frame.resize(kHeader);
    }
    return records;
  }
  uint64_t last_serial() const { return last_; }
  uint64_t Bytes() const {
    struct stat st{};if(::fstat(fd_,&st)) throw std::runtime_error("stat commit log failed");return uint64_t(st.st_size);
  }
  void ResetAfterCheckpoint() {
    if (count_) throw std::runtime_error("cannot reset uncommitted log batch");
    Repair(0);
  }
 private:
  void Repair(uint64_t offset) {
    if (::ftruncate(fd_, off_t(offset)) < 0) throw std::runtime_error("repair commit tail failed");
    Sync(fd_);
  }
  int fd_ = -1;
  uint32_t lane_;
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
