// Feasibility adapter. FASTER/F2 operations retain their native async durability boundary.
#include "faster_backend.h"

#include "core/f2.h"
#include "device/file_system_disk.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#endif

#include <chrono>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fc = FASTER::core;
namespace fi = FASTER::index;
namespace fs = std::filesystem;
using FStatus = fc::Status;
void Trace(const char* stage, uint64_t serial = 0) {
  if (std::getenv("F2_ADAPTER_TRACE")) {
    std::fprintf(stderr, "F2_TRACE %s serial=%llu\n", stage,
                 static_cast<unsigned long long>(serial));
    std::fflush(stderr);
  }
}

struct Key {
  uint32_t length;
  uint32_t size() const { return uint32_t(sizeof(Key) + length); }
  const uint8_t* data() const { return reinterpret_cast<const uint8_t*>(this + 1); }
  fc::KeyHash GetHash() const {
    return fc::KeyHash{fc::FasterHashHelper<uint8_t>::compute(data(), length)};
  }
  bool operator==(const Key& other) const {
    return length == other.length && std::memcmp(data(), other.data(), length) == 0;
  }
  bool operator!=(const Key& other) const { return !(*this == other); }
};
struct ShallowKey {
  std::string bytes;
  uint32_t size() const { return uint32_t(sizeof(Key) + bytes.size()); }
  fc::KeyHash GetHash() const {
    return fc::KeyHash{fc::FasterHashHelper<uint8_t>::compute(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())};
  }
  void write_deep_key_at(Key* target) const {
    target->length = uint32_t(bytes.size());
    std::memcpy(target + 1, bytes.data(), bytes.size());
  }
  bool operator==(const Key& other) const {
    return bytes.size() == other.length && std::memcmp(bytes.data(), other.data(), other.length) == 0;
  }
  bool operator!=(const Key& other) const { return !(*this == other); }
};
struct Value {
  uint32_t length;
  uint32_t reserved;
  uint32_t size() const { return uint32_t(sizeof(Value) + length); }
  const char* data() const { return reinterpret_cast<const char*>(this + 1); }
};
struct Completion {
  std::atomic<bool> done{false};
  std::atomic<FStatus> status{FStatus::Corruption};
  std::string value;
};
class ReadContext : public fc::IAsyncContext {
 public:
  using key_t = Key;
  using value_t = Value;
  ReadContext(const std::string& key, std::shared_ptr<Completion> r) : key_{key}, result(std::move(r)) {}
  ReadContext(const ReadContext& other) : key_(other.key_), result(other.result) {}
  const ShallowKey& key() const { return key_; }
  void Get(const Value& value) { result->value.assign(value.data(), value.length); }
  void GetAtomic(const Value& value) { Get(value); }
  std::shared_ptr<Completion> result;
 protected:
  FStatus DeepCopy_Internal(fc::IAsyncContext*& copy) override {
    return fc::IAsyncContext::DeepCopy_Internal(*this, copy);
  }
 private:
  ShallowKey key_;
};
class UpsertContext : public fc::IAsyncContext {
 public:
  using key_t = Key;
  using value_t = Value;
  UpsertContext(const std::string& key, const std::string& value, std::shared_ptr<Completion> r)
      : result(std::move(r)), key_{key}, value_(value) {}
  UpsertContext(const UpsertContext& other) : result(other.result), key_(other.key_), value_(other.value_) {}
  const ShallowKey& key() const { return key_; }
  uint32_t value_size() const { return uint32_t(sizeof(Value) + value_.size()); }
  void Put(Value& value) {
    value.length = uint32_t(value_.size());
    value.reserved = 0;
    std::memcpy(&value + 1, value_.data(), value_.size());
  }
  bool PutAtomic(Value&) { return false; }
  std::shared_ptr<Completion> result;
 protected:
  FStatus DeepCopy_Internal(fc::IAsyncContext*& copy) override {
    return fc::IAsyncContext::DeepCopy_Internal(*this, copy);
  }
 private:
  ShallowKey key_;
  std::string value_;
};
class DeleteContext : public fc::IAsyncContext {
 public:
  using key_t = Key;
  using value_t = Value;
  DeleteContext(const std::string& key, std::shared_ptr<Completion> r) : result(std::move(r)), key_{key} {}
  DeleteContext(const DeleteContext& other) : result(other.result), key_(other.key_) {}
  const ShallowKey& key() const { return key_; }
  uint32_t value_size() const { return sizeof(Value); }
  std::shared_ptr<Completion> result;
 protected:
  FStatus DeepCopy_Internal(fc::IAsyncContext*& copy) override {
    return fc::IAsyncContext::DeepCopy_Internal(*this, copy);
  }
 private:
  ShallowKey key_;
};

#ifdef F2_SYNC_EXPERIMENT
#include "f2_sync_runtime.inc"
#else
#include "f2_runtime.inc"
#endif

#ifdef F2_WAL_TEST_HOOKS
extern "C" void F2WalSetTestHook(void(*hook)(const char*)) {
  fc::experimental::wal_test_hook.store(hook,std::memory_order_release);
}
extern "C" void F2WalDeferNextWrite() {
  fc::experimental::wal_deferred=false;fc::experimental::wal_later_completed=false;
  fc::experimental::wal_release_deferred=false;fc::experimental::wal_defer_next=true;
}
extern "C" bool F2WalDeferredWriteReady() {return fc::experimental::wal_deferred.load();}
extern "C" bool F2WalLaterWriteCompleted() {return fc::experimental::wal_later_completed.load();}
extern "C" void F2WalReleaseDeferredWrite() {fc::experimental::wal_release_deferred=true;}
extern "C" bool F2WalCheckCrc() {
  auto software=fc::experimental::CRCSoftware;
  if(software("123456789",9)!=0xe3069283U)return false;
  std::string bytes((1<<20)+64,'\0');uint64_t state=0x123456789abcdefULL;
  for(auto& byte:bytes){state^=state<<13;state^=state>>7;state^=state<<17;byte=char(state);}
  for(size_t offset=0;offset<16;++offset) {
    for(size_t size:{size_t(0),size_t(1),size_t(7),size_t(8),size_t(9),size_t(31),size_t(32),size_t(4095),size_t(4096),size_t(65535),size_t(1<<20)}) {
      if(software(bytes.data()+offset,size)!=fc::experimental::CRC(bytes.data()+offset,size))return false;
    }
  }
  return true;
}
#endif

#if defined(F2_DATA_PREFIX) || defined(F2_WAL_TEST_HOOKS)
// Test-only fingerprint of the real memory index bucket and tag. No alternate
// hash function or collision shortcut is used in the engine.
extern "C" uint64_t F2PrefixHashFingerprint(const char* bytes,size_t length,uint64_t entries) {
  fi::MemHashIndex<Disk>::key_hash_t hash(fc::KeyHash{fc::FasterHashHelper<uint8_t>::compute(
    reinterpret_cast<const uint8_t*>(bytes),length)});
  return (uint64_t(hash.tag())<<32)|hash.hash_table_index(entries);
}
#endif
