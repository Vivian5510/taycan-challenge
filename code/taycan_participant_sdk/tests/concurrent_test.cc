#include <algorithm>
#include <atomic>
#include <thread>

#include "test_helper.h"

TEST(Concurrent, DisjointKeys) {
  TestDirectory dir;
  std::unique_ptr<KVEngine> engine;
  ASSERT_TRUE(open_engine(dir.path, engine));
  std::atomic<bool> failed{false};
  std::vector<std::thread> threads;
  for (uint64_t shard = 0; shard < 4; ++shard) {
    threads.emplace_back([&, shard] {
      DBHelper db;
      for (uint64_t id = shard; id < FLAGS_num_keys && !failed; id += 4) {
        for (const auto version : {1u, 2u, 0u, 3u}) {
          auto result = db.write(*engine, id, version);
          if (result) {
            result = db.check(*engine, id, version);
          }
          if (!result) {
            ADD_FAILURE() << result.message();
            failed = true;
            return;
          }
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  ASSERT_FALSE(failed);
  DBHelper db;
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.check(*engine, id, 3));
  }
}

namespace {
uint64_t load(const char* bytes) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= uint64_t(static_cast<unsigned char>(bytes[i])) << (8 * i);
  }
  return value;
}

// Get has already finished; validate all bytes, not just the version header.
::testing::AssertionResult check_value(DBHelper& db, uint64_t id,
                                       uint64_t& version) {
  if (db.output.size() != FLAGS_value_size) {
    return ::testing::AssertionFailure() << "wrong value size";
  }
  version = load(db.output.data() + 8);
  make_value(db.value, id, version);
  if (db.output != db.value) {
    return ::testing::AssertionFailure()
           << "seed=" << FLAGS_seed << " key_id=" << id
           << " version=" << version << " corrupt value";
  }
  return ::testing::AssertionSuccess();
}

}  // namespace

TEST(Concurrent, SingleWriterVersionBounds) {
  TestDirectory dir;
  std::unique_ptr<KVEngine> engine;
  ASSERT_TRUE(open_engine(dir.path, engine));
  constexpr uint64_t kWindowSize = 64;
  std::vector<std::atomic<uint64_t>> started(FLAGS_num_keys);
  std::vector<std::atomic<uint64_t>> completed(FLAGS_num_keys);
  std::atomic<uint64_t> window_begin{0};
  DBHelper initial;
  for (size_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(initial.write(*engine, id, 1));
    started[id] = 1;
    completed[id] = 1;
  }
  std::atomic<unsigned> ready{0};
  std::atomic<bool> start{false}, done{false}, failed{false};
  std::vector<uint64_t> reads(FLAGS_reader_threads, 0);
  std::vector<std::thread> readers;
  for (size_t reader = 0; reader < reads.size(); ++reader) {
    readers.emplace_back([&, reader] {
      DBHelper db;
      Generator rng(FLAGS_seed + reader);
      ++ready;
      while (!start) {
        std::this_thread::yield();
      }
      do {
        // One atomic snapshot keeps the last, possibly partial window valid.
        const uint64_t begin = window_begin.load();
        const uint64_t count = std::min(kWindowSize, FLAGS_num_keys - begin);
        const uint64_t id = begin + rng.next() % count;
        make_key(db.key, id);
        const auto lower = completed[id].load();
        const auto status = engine->Get(db.key, &db.output);
        const auto upper = started[id].load();
        uint64_t version = 0;
        const auto result = check_value(db, id, version);
        if (!status.IsOk() || !result || version < lower || version > upper) {
          ADD_FAILURE() << "seed=" << FLAGS_seed << " key_id=" << id
                        << " bounds=[" << lower << ',' << upper
                        << "] version=" << version << ' ' << status.ToString()
                        << ' ' << result.message();
          failed = true;
          break;
        }
        ++reads[reader];
      } while (!done && !failed);
    });
  }
  std::thread writer([&] {
    DBHelper db;
    ++ready;
    while (!start) {
      std::this_thread::yield();
    }
    for (uint64_t begin = 0; begin < FLAGS_num_keys && !failed;
         begin += kWindowSize) {
      const uint64_t end =
          begin + std::min(kWindowSize, FLAGS_num_keys - begin);
      // Readers still accessing the previous window remain valid: version
      // records are retained for every key throughout the test.
      window_begin = begin;
      for (uint64_t round = 1; round <= FLAGS_rounds && !failed; ++round) {
        const uint64_t version = round + 1;
        for (uint64_t id = begin; id < end && !failed; ++id) {
          started[id] = version;
          const auto result = db.write(*engine, id, version);
          if (!result) {
            ADD_FAILURE() << result.message();
            failed = true;
            break;
          }
          completed[id] = version;
        }
      }
    }
    done = true;
  });
  while (ready != FLAGS_reader_threads + 1) {
    std::this_thread::yield();
  }
  start = true;
  writer.join();
  for (auto& reader : readers) {
    reader.join();
  }
  ASSERT_FALSE(failed);
  for (auto count : reads) {
    EXPECT_GT(count, 0u);
  }
  for (size_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(initial.check(*engine, id, uint64_t(FLAGS_rounds) + 1));
  }
  engine.reset();
  ASSERT_TRUE(open_engine(dir.path, engine));
  for (size_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(initial.check(*engine, id, uint64_t(FLAGS_rounds) + 1));
  }
}
