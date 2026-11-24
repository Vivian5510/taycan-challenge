#include <atomic>
#include <thread>

#include "test_helper.h"

TEST(MemoryLimit, BoundedWritesAndRecovery) {
  TestDirectory dir;
  DBHelper db;
  std::unique_ptr<KVEngine> engine;
  ASSERT_TRUE(open_engine(dir.path, engine));
  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  for (uint64_t shard = 0; shard < 4; ++shard) {
    workers.emplace_back([&, shard] {
      DBHelper db;
      // A key belongs to exactly one worker, so readback needs no test locks.
      for (uint64_t id = shard; id < FLAGS_num_keys && !failed; id += 4) {
        auto result = db.write(*engine, id, 1);
        if (result) {
          result = db.write(*engine, id, id % 3 ? 2 : 0);
        }
        if (result && ((id - shard) / 4) % 64 == 0) {
          // Read an earlier key from this worker's own shard.
          const auto old = shard + ((id - shard) / 4 / 2) * 4;
          result = db.check(*engine, old, old % 3 ? 2 : 0);
        }
        if (!result) {
          ADD_FAILURE() << "shard=" << shard << ": " << result.message();
          failed = true;
          return;
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  ASSERT_FALSE(failed);

  for (int reopen = 0; reopen < 2; ++reopen) {
    if (reopen) {
      engine.reset();
      ASSERT_TRUE(open_engine(dir.path, engine));
    }
    KeyOrder order(17);
    for (uint64_t i = 0; i < FLAGS_num_keys; ++i) {
      const auto id = order.next();
      ASSERT_TRUE(db.check(*engine, id, id % 3 ? 2 : 0));
    }
  }
  engine.reset();
}
