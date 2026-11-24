#include "test_helper.h"

TEST(Mixed, ReferenceVersions) {
  TestDirectory dir;
  DBHelper db;
  std::unique_ptr<KVEngine> engine;
  ASSERT_TRUE(open_engine(dir.path, engine));
  std::vector<uint32_t> versions(FLAGS_num_keys, 1);
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.write(*engine, id, 1));
  }
  Generator rng(FLAGS_seed);
  for (uint64_t i = 0; i < FLAGS_num_keys * 3; ++i) {
    const auto id = rng.next() % FLAGS_num_keys;
    switch (rng.next() % 3) {
      case 0:
        ASSERT_TRUE(db.write(*engine, id, i + 2));
        versions[id] = i + 2;
        break;
      case 1:
        ASSERT_TRUE(db.write(*engine, id, 0));
        versions[id] = 0;
        break;
      default:
        ASSERT_TRUE(db.check(*engine, id, versions[id]));
    }
  }
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.check(*engine, id, versions[id]));
  }
}
