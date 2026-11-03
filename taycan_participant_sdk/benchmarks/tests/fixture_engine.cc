// Test fixture only. Never linked into the submission or the real benchmark.
#include "kv_engine.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {
class Fixture final : public KVEngine {
  std::mutex mutex_;
  std::unordered_map<std::string, std::string> values_;
  std::string fault_ = std::getenv("TAYCAN_BENCH_FAULT") ? std::getenv("TAYCAN_BENCH_FAULT") : "";
 public:
  Status Put(const std::string& key, const std::string& value) override {
    if (fault_ == "put_error") return Status::IOError("injected prepare error");
    if (fault_ == "slow_prepare") std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::lock_guard<std::mutex> lock(mutex_);
    values_[key] = value;
    return Status::OK();
  }
  Status Get(const std::string& key, std::string* value) override {
    if (fault_ == "missing") return Status::NotFound();
    if (fault_ == "throw") throw std::runtime_error("injected worker exception");
    if (fault_ == "slow_get") std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = values_.find(key);
    if (found == values_.end()) return Status::NotFound();
    *value = found->second;
    if (fault_ == "corruption") value->back() ^= 1;
    return Status::OK();
  }
  Status Delete(const std::string& key) override {
    std::lock_guard<std::mutex> lock(mutex_);
    values_.erase(key);
    return Status::OK();
  }
};
}
Status KVEngine::Open(const std::string& directory, uint64_t, KVEngine** output) {
  std::filesystem::create_directories(directory);
  *output = new Fixture;
  return Status::OK();
}
