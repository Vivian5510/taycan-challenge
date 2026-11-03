#include <algorithm>
#include <atomic>
#include <future>
#include <thread>

#include "test_helper.h"

TEST(Checkpoint, IndependentCopy) {
  TestDirectory dir;
  DBHelper db;
  std::unique_ptr<KVEngine> engine;
  ASSERT_TRUE(open_engine(dir.path / "original", engine));
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.write(*engine, id, 1));
  }
  for (uint64_t id = 0; id < FLAGS_num_keys; id += 8) {
    ASSERT_TRUE(db.write(*engine, id, 0));
  }
  const auto status = engine->CreateCheckpoint((dir.path / "copy").string());
  if (status.ToString() == "IOError: Checkpoint is not supported") {
    GTEST_SKIP() << "Checkpoint not implemented";
  }
  ASSERT_TRUE(status.IsOk()) << status.ToString();

  // Overwrite every live key and resurrect deleted keys in the original.
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.write(*engine, id, 2));
  }
  engine.reset();
  std::filesystem::remove_all(dir.path / "original");
  ASSERT_TRUE(open_engine(dir.path / "copy", engine));
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.check(*engine, id, id % 8 ? 1 : 0));
  }

  // The copy must persist its own updates, deletions and a new key.
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.write(*engine, id, id % 5 ? 3 : 0));
  }
  ASSERT_TRUE(db.write(*engine, FLAGS_num_keys, 4));
  engine.reset();
  ASSERT_TRUE(open_engine(dir.path / "copy", engine));
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.check(*engine, id, id % 5 ? 3 : 0));
  }
  ASSERT_TRUE(db.check(*engine, FLAGS_num_keys, 4));
}

TEST(Checkpoint, ConcurrentReadWrite) {
  TestDirectory dir;
  DBHelper db;
  std::unique_ptr<KVEngine> engine;
  ASSERT_TRUE(open_engine(dir.path / "original", engine));
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.write(*engine, id, 1));
  }

  std::promise<void> writer_ready, reader_ready, start;
  const auto started = start.get_future().share();
  std::atomic<bool> stop{false}, failed{false};
  std::atomic<uint64_t> completed{0};

  std::thread writer([&] {
    DBHelper db;
    const auto first = db.write(*engine, 0, 2);
    if (!first) {
      ADD_FAILURE() << first.message();
      failed = true;
    } else {
      completed = 1;
    }
    // Always notify, even on failure, so the main thread can join both workers.
    writer_ready.set_value();
    started.wait();

    // One ordered mutation per key: any consistent snapshot is a prefix.
    for (uint64_t id = 1; id < FLAGS_num_keys && !failed; ++id) {
      const auto result = db.write(*engine, id, id % 2 ? 0 : 2);
      if (!result) {
        ADD_FAILURE() << result.message();
        failed = true;
        break;
      }
      completed = id + 1;
    }
  });
  std::thread reader([&] {
    DBHelper db;
    auto read = [&](uint64_t id) {
      make_key(db.key, id);
      const auto status = engine->Get(db.key, &db.output);
      make_value(db.value, id, 1);
      bool valid = status.IsOk() && db.output == db.value;
      if (id % 2) {
        valid = valid || status.IsNotFound();
      } else {
        make_value(db.value, id, 2);
        valid = valid || (status.IsOk() && db.output == db.value);
      }
      if (!valid) {
        ADD_FAILURE() << "seed=" << FLAGS_seed << " key_id=" << id
                      << ": concurrent Get returned an invalid value: "
                      << status.ToString();
        failed = true;
      }
    };

    read(0);
    reader_ready.set_value();
    started.wait();

    for (uint64_t id = 0; !stop && !failed; id = (id + 1) % FLAGS_num_keys) {
      read(id);
      std::this_thread::yield();
    }
  });

  // Prepare: both workers have completed their first operation.
  writer_ready.get_future().wait();
  reader_ready.get_future().wait();
  const uint64_t before = completed;

  // Run: release the workers and create the checkpoint concurrently.
  start.set_value();
  const auto status = engine->CreateCheckpoint((dir.path / "copy").string());
  // A write may have committed before its completion counter is published.
  const uint64_t after = std::min(FLAGS_num_keys, completed.load() + 1);
  // Finish: stop reads and wait for all writes before checking the result.
  stop = true;
  writer.join();
  reader.join();
  ASSERT_FALSE(failed);
  if (status.ToString() == "IOError: Checkpoint is not supported") {
    GTEST_SKIP() << "Checkpoint not implemented";
  }
  ASSERT_TRUE(status.IsOk()) << status.ToString();

  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.check(*engine, id, id % 2 ? 0 : 2));
  }
  engine.reset();
  std::filesystem::remove_all(dir.path / "original");
  ASSERT_TRUE(open_engine(dir.path / "copy", engine));

  uint64_t prefix = 0;
  bool saw_version_1 = false;
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    make_key(db.key, id);
    const auto result = engine->Get(db.key, &db.output);
    make_value(db.value, id, 2);
    bool data_updated = false;
    if (id % 2) {
      // the key maybe deleted or still at version 1, but not version 2
      data_updated = result.IsNotFound();
    } else {
      data_updated = result.IsOk() && db.output == db.value;
    }

    if (data_updated) {
      ASSERT_FALSE(saw_version_1) << "Non-prefix checkpoint at key_id=" << id;
      ++prefix;
    } else {
      make_value(db.value, id, 1);
      ASSERT_TRUE(result.IsOk())
          << "key_id=" << id << ": " << result.ToString();
      ASSERT_EQ(db.output, db.value) << "key_id=" << id;
      saw_version_1 = true;
    }
  }
  ASSERT_GE(prefix, before);
  ASSERT_LE(prefix, after);

  engine.reset();
  ASSERT_TRUE(open_engine(dir.path / "copy", engine));
  for (uint64_t id = 0; id < FLAGS_num_keys; ++id) {
    ASSERT_TRUE(db.check(*engine, id, id < prefix ? (id % 2 ? 0 : 2) : 1));
  }
}
