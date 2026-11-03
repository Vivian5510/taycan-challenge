#include "test_helper.h"

TEST(Delete, MissingAndRepeated) {
  TestDirectory dir;
  DBHelper db;
  std::unique_ptr<KVEngine> engine;
  ASSERT_TRUE(open_engine(dir.path, engine));
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.write(*engine, id, 1));
    ASSERT_TRUE(db.write(*engine, id, 0));
    ASSERT_TRUE(db.check(*engine, id, 0));
    ASSERT_TRUE(db.write(*engine, id, 0));
    ASSERT_TRUE(db.write(*engine, id + FLAGS_num_keys, 0));
    ASSERT_TRUE(db.check(*engine, id + FLAGS_num_keys, 0));
  }
}
