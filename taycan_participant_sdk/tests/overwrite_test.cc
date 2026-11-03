#include "test_helper.h"

TEST(Overwrite, LatestVersion) {
  TestDirectory dir;
  DBHelper db;
  std::unique_ptr<KVEngine> engine;
  ASSERT_TRUE(open_engine(dir.path, engine));
  for (uint32_t version = 1; version <= FLAGS_rounds; ++version) {
    for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
      ASSERT_TRUE(db.write(*engine, id, version));
    }
    // Check intermediate rounds without adding a full read pass each time.
    for (const uint64_t id :
         {uint64_t{0}, FLAGS_num_keys / 2, FLAGS_num_keys - 1}) {
      ASSERT_TRUE(db.check(*engine, id, version));
    }
  }
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.check(*engine, id, FLAGS_rounds));
  }
}
