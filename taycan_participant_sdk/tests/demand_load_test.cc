#include <atomic>
#include <thread>

#include "test_helper.h"

TEST(DemandLoad, BoundedRecoveryAndReads) {
  TestDirectory dir;
  DBHelper db;
  std::unique_ptr<KVEngine> engine;
  ASSERT_TRUE(open_engine(dir.path, engine));
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.write(*engine, id, 1));
  }
  engine.reset();
  ASSERT_TRUE(open_engine(dir.path, engine));
  std::atomic<bool> failed{false};
  std::vector<std::thread> readers;
  for (uint64_t reader = 0; reader < 4; ++reader) {
    readers.emplace_back([&, reader] {
      DBHelper db;
      // Each reader covers every key twice, using its own traversal orders.
      for (uint64_t pass = 0; pass < 2 && !failed; ++pass) {
        KeyOrder order(7 + reader * 2 + pass);
        for (uint64_t i = 0; i < FLAGS_num_keys && !failed; ++i) {
          const auto result = db.check(*engine, order.next(), 1);
          if (!result) {
            ADD_FAILURE() << "reader=" << reader << ": " << result.message();
            failed = true;
            return;
          }
        }
      }
    });
  }
  for (auto& reader : readers) {
    reader.join();
  }
  ASSERT_FALSE(failed);
  engine.reset();
}
