#include "test_helper.h"

TEST(Recovery, ReopenTwice) {
  TestDirectory dir;
  DBHelper db;
  std::unique_ptr<KVEngine> engine;
  ASSERT_TRUE(open_engine(dir.path, engine));
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.write(*engine, id, 1));
    ASSERT_TRUE(db.write(*engine, id, id % 3 ? 2 : 0));
  }
  engine.reset();
  ASSERT_TRUE(open_engine(dir.path, engine));
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.check(*engine, id, id % 3 ? 2 : 0));
  }

  // After recovery, restore deleted keys (id % 3 == 0), overwrite some live
  // keys (== 1), and delete the remaining live keys (== 2).
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.write(*engine, id, id % 3 == 2 ? 0 : 3));
  }
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.check(*engine, id, id % 3 == 2 ? 0 : 3));
  }

  engine.reset();
  ASSERT_TRUE(open_engine(dir.path, engine));
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.check(*engine, id, id % 3 == 2 ? 0 : 3));
  }
}
