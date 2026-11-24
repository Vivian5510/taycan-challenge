#include "test_helper.h"

TEST(Random, WriteAndRead) {
  TestDirectory dir;
  DBHelper db;
  std::unique_ptr<KVEngine> engine;
  ASSERT_TRUE(open_engine(dir.path, engine));
  KeyOrder order(1);
  for (uint64_t i = 0; i < FLAGS_num_keys; ++i) {
    ASSERT_TRUE(db.write(*engine, order.next(), 1));
  }
  Generator rng(FLAGS_seed);
  for (uint64_t i = 0; i < FLAGS_num_keys; ++i) {
    ASSERT_TRUE(db.check(*engine, rng.next() % FLAGS_num_keys, 1));
  }
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.check(*engine, id, 1));
  }
}
