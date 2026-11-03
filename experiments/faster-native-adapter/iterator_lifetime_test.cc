// Regression: native scan callbacks must remain valid after the iterator exits.
// The old implementation faults when another session releases a delayed epoch.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "core/log_scan.h"

namespace fc = FASTER::core;
struct Key { uint64_t word; uint32_t size() const { return sizeof(*this); } };
struct Value { uint64_t word; uint32_t size() const { return sizeof(*this); } };
struct File {
  struct Request {
    void* buffer;
    uint32_t bytes;
    fc::AsyncIOCallback callback;
    fc::IAsyncContext* context;
  };
  std::vector<Request> pending;
  unsigned completed = 0;

  fc::Status ReadAsync(uint64_t, void* buffer, uint32_t bytes,
                       fc::AsyncIOCallback callback, fc::IAsyncContext& context) {
    fc::IAsyncContext* copy = nullptr;
    auto status = context.DeepCopy(copy);
    if (status == fc::Status::Ok) pending.push_back({buffer, bytes, callback, copy});
    return status;
  }
  void Complete() {
    for (auto& request : pending) {
      std::memset(request.buffer, 0, request.bytes);
      request.callback(request.context, fc::Status::Ok, request.bytes);
      ++completed;
    }
    pending.clear();
  }
};
struct Hlog {
  static constexpr uint64_t kPageSize = uint64_t(fc::Address::kMaxOffset) + 1;
  uint32_t sector_size = 512;
  fc::AtomicAddress head_address{fc::Address(0, 0)};
  File file_value;
  File* file = &file_value;
  std::vector<uint8_t> bytes = std::vector<uint8_t>(3 * kPageSize);
  uint8_t* Page(uint32_t page) { return bytes.data() + page * kPageSize; }
};
struct Disk { bool TryComplete() { return false; } };
struct Store { using key_t = Key; using value_t = Value; using hlog_t = Hlog; using disk_t = Disk; };

static void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

static void DelayedEpochCallback() {
  fc::LightEpoch epoch;
  Hlog hlog;
  Disk disk;
  std::atomic<bool> protected_old{false}, release_old{false};
  // Keep the observer in the old epoch until the scan's destructor has returned.
  std::thread foreground([&] {
    epoch.Protect();
    protected_old.store(true);
    while (!release_old.load()) std::this_thread::yield();
    epoch.ProtectAndDrain();
    epoch.Unprotect();
  });
  while (!protected_old.load()) std::this_thread::yield();
  epoch.Protect();
  {
    fc::ConcurrentLogPageIterator<Store> iterator(
        &hlog, &disk, &epoch, fc::Address(0, 0), fc::Address(3, 0));
    uint32_t old_page = fc::Address::kMaxPage;
    for (uint32_t id = 0; id < 3; ++id) {
      bool available = false;
      auto* page = iterator.GetNextPage(old_page, available);
      Check(available && page && page->id() == id, "resident page scan failed");
      fc::Address address;
      Check(page->GetNextRecord(address) == nullptr, "empty page contains records");
      old_page = id;
    }
    bool available = true;
    Check(!iterator.GetNextPage(old_page, available) && !available, "scan did not finish");
  }
  epoch.Unprotect();
  release_old.store(true);
  foreground.join();
  epoch.ProtectAndDrain();
  epoch.Unprotect();
}

static void DelayedIoCallback() {
  fc::LightEpoch epoch;
  Hlog hlog;
  Disk disk;
  hlog.head_address.store(fc::Address(3, 0));
  epoch.Protect();
  {
    fc::ConcurrentLogPageIterator<Store> iterator(
        &hlog, &disk, &epoch, fc::Address(0, 0), fc::Address(3, 0));
    Check(hlog.file->pending.size() == 3, "disk reads were not scheduled");
  }
  // The destination pages and callback contexts must survive iterator teardown.
  hlog.file->Complete();
  Check(hlog.file->completed == 3, "disk callbacks did not finish");
  epoch.Unprotect();
}

int main() {
  for (unsigned iteration = 0; iteration < 20; ++iteration) {
    DelayedEpochCallback();
    DelayedIoCallback();
  }
  std::cout << "PASS delayed epoch callback and delayed I/O after scan destruction: 20 each\n";
}
