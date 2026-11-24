#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "test_helper.h"

namespace {
// Background deletion can race with enumeration; retry the entire snapshot.
bool directory_bytes(const std::filesystem::path& path, uint64_t& bytes) {
  std::error_code error;
  bytes = 0;
  std::filesystem::recursive_directory_iterator it(path, error), end;
  while (!error && it != end) {
    if (it->is_regular_file(error)) {
      const auto size = it->file_size(error);
      if (!error) {
        bytes += size;
      }
    }
    if (!error) {
      it.increment(error);
    }
  }
  return !error;
}
}  // namespace

TEST(Compaction, ConcurrentUpdatesAndSpaceReclamation) {
  TestDirectory dir;
  std::array<std::mutex, 64> locks;
  std::vector<uint32_t> versions(FLAGS_num_keys, 0);
  std::array<DBHelper, 3> buffers;
  DBHelper verify;
  std::unique_ptr<KVEngine> engine;
  ASSERT_TRUE(open_engine(dir.path, engine));
  std::atomic<bool> done{false}, failed{false};
  auto writer = [&](unsigned shard) {
    auto& db = buffers[shard];
    for (uint32_t round = 1; round <= FLAGS_rounds && !failed; ++round) {
      for (uint64_t id = shard; id < FLAGS_num_keys && !failed; id += 2) {
        std::lock_guard<std::mutex> lock(locks[id % locks.size()]);
        // Keep tombstones in the final state to detect stale-value
        // resurrection.
        const bool deleted =
            (round % 4 == 0 || round == FLAGS_rounds) && id % 3 == 0;
        const uint32_t version = deleted ? 0 : round;
        const auto result = db.write(*engine, id, version);
        if (!result) {
          ADD_FAILURE() << result.message();
          failed = true;
        } else {
          versions[id] = version;
        }
      }
    }
  };
  std::thread first(writer, 0), second(writer, 1);
  std::thread reader([&] {
    auto& db = buffers[2];
    Generator rng(FLAGS_seed);
    while (!done && !failed) {
      const auto id = rng.next() % FLAGS_num_keys;
      std::lock_guard<std::mutex> lock(locks[id % locks.size()]);
      const auto result = db.check(*engine, id, versions[id]);
      if (!result) {
        ADD_FAILURE() << result.message();
        failed = true;
      }
    }
  });
  // Finish writes, stop the reader, then validate the final state.
  first.join();
  second.join();
  done = true;
  reader.join();
  ASSERT_FALSE(failed);
  uint64_t live = 0;
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    if (versions[id]) {
      live += uint64_t(FLAGS_key_size) + FLAGS_value_size;
    }
  }
  const auto limit =
      static_cast<uint64_t>(live * FLAGS_space_ratio) + FLAGS_space_slack_bytes;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(FLAGS_reclaim_timeout_seconds);
  uint64_t bytes = 0;
  bool measured = false;
  do {
    measured = directory_bytes(dir.path, bytes);
    if (measured && bytes <= limit) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  } while (std::chrono::steady_clock::now() < deadline);
  ASSERT_TRUE(measured) << "Cannot measure stable directory size";
  ASSERT_LE(bytes, limit) << "live_bytes=" << live << " (engine remains open)";
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(verify.check(*engine, id, versions[id]));
  }
  engine.reset();
  ASSERT_TRUE(open_engine(dir.path, engine));
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(verify.check(*engine, id, versions[id]));
  }
  engine.reset();
  std::cout << "directory_bytes=" << bytes << " live_bytes=" << live << '\n';
}
