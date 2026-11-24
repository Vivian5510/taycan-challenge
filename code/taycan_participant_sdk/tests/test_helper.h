#pragma once

#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "kv_engine.h"
#include "test_options.h"
#include "workload.h"

class TestDirectory {
 public:
  TestDirectory() {
    auto parent = FLAGS_data_dir.empty()
                      ? std::filesystem::temp_directory_path()
                      : std::filesystem::path(FLAGS_data_dir);
    std::filesystem::create_directories(parent);
    auto pattern = (parent / "taycan-test-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    const char* result = mkdtemp(name.data());
    if (!result) {
      throw std::runtime_error("mkdtemp failed");
    }
    path = result;
  }
  ~TestDirectory() {
    if (::testing::Test::HasFailure()) {
      std::cerr << "Preserved failed test directory: " << path << '\n';
      return;
    }
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  std::filesystem::path path;
};

inline ::testing::AssertionResult open_engine(
    const std::filesystem::path& path, std::unique_ptr<KVEngine>& engine) {
  KVEngine* raw = nullptr;
  auto status = KVEngine::Open(path.string(), FLAGS_mem_limit_bytes, &raw);
  engine.reset(raw);
  if (!status.IsOk() || !raw) {
    return ::testing::AssertionFailure() << "Open: " << status.ToString();
  }
  return ::testing::AssertionSuccess();
}

// Reuse buffers to keep test-owned allocations bounded.
class DBHelper {
 public:
  DBHelper() : key(FLAGS_key_size, '\0'), value(FLAGS_value_size, '\0') {
    output.reserve(FLAGS_value_size);
  }
  ::testing::AssertionResult write(KVEngine& engine, uint64_t id,
                                   uint64_t version) {
    make_key(key, id);
    make_value(value, id, version);
    auto status = version ? engine.Put(key, value) : engine.Delete(key);
    if (!status.IsOk()) {
      return failure(id, version) << status.ToString();
    }
    return ::testing::AssertionSuccess();
  }
  ::testing::AssertionResult check(KVEngine& engine, uint64_t id,
                                   uint64_t version) {
    make_key(key, id);
    const auto status = engine.Get(key, &output);
    if (!version) {
      if (!status.IsNotFound()) {
        return failure(id, version) << status.ToString();
      }
    } else {
      make_value(value, id, version);
      if (!status.IsOk() || output != value) {
        return failure(id, version)
               << status.ToString() << "; value mismatch or read failed";
      }
    }
    return ::testing::AssertionSuccess();
  }
  std::string key, value, output;

 private:
  ::testing::AssertionResult failure(uint64_t id, uint64_t version) {
    return ::testing::AssertionFailure()
           << "seed=" << FLAGS_seed << " key_id=" << id
           << " version=" << version << ": ";
  }
};
