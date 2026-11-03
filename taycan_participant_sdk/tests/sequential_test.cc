#include "test_helper.h"

TEST(Sequential, WriteAndRead) {
  TestDirectory dir;
  DBHelper db;
  std::unique_ptr<KVEngine> engine;
  ASSERT_TRUE(open_engine(dir.path, engine));
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.write(*engine, id, 1));
  }
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.check(*engine, id, 1));
  }
}
