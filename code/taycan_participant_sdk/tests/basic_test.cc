#include <array>

#include "test_helper.h"

TEST(Basic, BinaryKeysAndValues) {
  TestDirectory dir;
  std::unique_ptr<KVEngine> engine;
  ASSERT_TRUE(open_engine(dir.path, engine));
  std::string output;
  for (const auto& key :
       {std::string("k"), std::string("a\0b", 3), std::string(1023, 'k')}) {
    ASSERT_TRUE(engine->Get(key, &output).IsNotFound());
    ASSERT_TRUE(engine->Delete(key).IsOk());
    // Reuse output while values grow and shrink, including back to empty.
    for (const auto& value : {std::string(), std::string("v\0x", 3),
                              std::string(100 * 1024 - 1, 'v'),
                              std::string("short"), std::string()}) {
      ASSERT_TRUE(engine->Put(key, value).IsOk());
      ASSERT_TRUE(engine->Get(key, &output).IsOk());
      ASSERT_EQ(output, value);
    }
    ASSERT_TRUE(engine->Delete(key).IsOk());
    ASSERT_TRUE(engine->Delete(key).IsOk());
    ASSERT_TRUE(engine->Get(key, &output).IsNotFound());
  }
}

TEST(Basic, SimilarKeysRemainIndependent) {
  TestDirectory dir;
  std::unique_ptr<KVEngine> engine;
  ASSERT_TRUE(open_engine(dir.path, engine));
  const std::array<std::string, 4> keys = {"a", "ab", std::string("a\0", 2),
                                           std::string("a\0b", 3)};
  std::array<std::string, 4> values;
  std::string output;
  for (size_t i = 0; i < keys.size(); ++i) {
    values[i] = "value-" + std::to_string(i);
    ASSERT_TRUE(engine->Put(keys[i], values[i]).IsOk());
  }
  for (size_t i = 0; i < keys.size(); ++i) {
    ASSERT_TRUE(engine->Get(keys[i], &output).IsOk());
    ASSERT_EQ(output, values[i]) << "key_index=" << i;
  }

  for (size_t changed = 0; changed < keys.size(); ++changed) {
    values[changed] = "updated-" + std::to_string(changed);
    ASSERT_TRUE(engine->Put(keys[changed], values[changed]).IsOk());
    for (size_t i = 0; i < keys.size(); ++i) {
      ASSERT_TRUE(engine->Get(keys[i], &output).IsOk());
      ASSERT_EQ(output, values[i]) << "key_index=" << i;
    }

    ASSERT_TRUE(engine->Delete(keys[changed]).IsOk());
    for (size_t i = 0; i < keys.size(); ++i) {
      const auto status = engine->Get(keys[i], &output);
      if (i == changed) {
        ASSERT_TRUE(status.IsNotFound());
      } else {
        ASSERT_TRUE(status.IsOk());
        ASSERT_EQ(output, values[i]) << "key_index=" << i;
      }
    }
    // Restore this key so the next iteration also starts with all keys present.
    ASSERT_TRUE(engine->Put(keys[changed], values[changed]).IsOk());
  }
}
