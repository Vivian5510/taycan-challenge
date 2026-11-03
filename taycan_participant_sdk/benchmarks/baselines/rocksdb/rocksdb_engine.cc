// Local baseline adapter; excluded from contestant submissions.
#include "kv_engine.h"

#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/table.h>
#include <rocksdb/utilities/checkpoint.h>
#include <rocksdb/write_buffer_manager.h>

#include <algorithm>
#include <filesystem>
#include <memory>

namespace {
Status Convert(const rocksdb::Status& status) {
  if (status.ok()) return Status::OK();
  if (status.IsNotFound()) return Status::NotFound();
  return Status::IOError(status.ToString());
}

class RocksEngine final : public KVEngine {
 public:
  explicit RocksEngine(rocksdb::DB* db) : db_(db) {
    write_.sync = true;
    write_.disableWAL = false;
  }
  Status Put(const std::string& key, const std::string& value) override {
    if (key.empty()) return Status::IOError("empty key");
    return Convert(db_->Put(write_, key, value));
  }
  Status Delete(const std::string& key) override {
    if (key.empty()) return Status::IOError("empty key");
    return Convert(db_->Delete(write_, key));
  }
  Status Get(const std::string& key, std::string* value) override {
    if (key.empty() || !value) return Status::IOError("empty key or null output");
    // The public interface permits the same std::string as key and output.
    if (&key == value) {
      const auto copied_key = key;
      return Convert(db_->Get(rocksdb::ReadOptions{}, copied_key, value));
    }
    return Convert(db_->Get(rocksdb::ReadOptions{}, key, value));
  }
  Status CreateCheckpoint(const std::string& directory) override {
    rocksdb::Checkpoint* raw = nullptr;
    auto status = rocksdb::Checkpoint::Create(db_.get(), &raw);
    std::unique_ptr<rocksdb::Checkpoint> checkpoint(raw);
    if (!status.ok()) return Convert(status);
    // Keep the baseline's checkpoint an independent copy of a committed prefix.
    return Convert(checkpoint->CreateCheckpoint(directory));
  }

 private:
  std::unique_ptr<rocksdb::DB> db_;
  rocksdb::WriteOptions write_;
};
}

Status KVEngine::Open(const std::string& directory, uint64_t budget, KVEngine** output) {
  if (!output) return Status::IOError("null output");
  *output = nullptr;
  if (directory.empty() || budget < (64ULL << 20))
    return Status::IOError("RocksDB baseline requires a directory and at least 64 MiB budget");
  try {
    namespace fs = std::filesystem;
    if (fs::is_symlink(fs::symlink_status(directory))) return Status::IOError("symlink database directory");
    // Do not silently recreate an empty DB when its metadata was removed.
    if (fs::exists(directory) && !fs::is_empty(directory) && !fs::exists(fs::path(directory)/"CURRENT"))
      return Status::IOError("CURRENT missing in a non-empty directory");
    fs::create_directories(directory);
    rocksdb::Options options;
    options.create_if_missing = true;
    options.compression = rocksdb::kNoCompression;
    options.bottommost_compression = rocksdb::kNoCompression;
    options.allow_mmap_reads = false;
    options.allow_mmap_writes = false;
    options.max_open_files = 128;
    options.max_background_jobs = 2;
    options.write_buffer_size = size_t(budget/16);
    options.max_write_buffer_number = 4;
    options.min_write_buffer_number_to_merge = 1;
    options.max_total_wal_size = budget/2;
    options.target_file_size_base = std::max<uint64_t>(4ULL << 20, budget/8);
    options.max_bytes_for_level_base = budget;
    options.level_compaction_dynamic_level_bytes = true;
    // Charge memtables to the same cache budget, leaving room for the runtime,
    // table readers, stacks and background allocations. Actual RSS is checked
    // by the identical external monitor; these knobs alone do not bound RSS.
    const uint64_t cache_budget = budget*5/8;
    auto cache = rocksdb::NewLRUCache(size_t(cache_budget), -1, true);
    options.write_buffer_manager = std::make_shared<rocksdb::WriteBufferManager>(
        size_t(budget/4), cache, true);
    rocksdb::BlockBasedTableOptions table;
    table.block_cache = cache;
    table.cache_index_and_filter_blocks = true;
    table.cache_index_and_filter_blocks_with_high_priority = true;
    table.pin_l0_filter_and_index_blocks_in_cache = false;
    table.pin_top_level_index_and_filter = false;
    table.index_type = rocksdb::BlockBasedTableOptions::kTwoLevelIndexSearch;
    table.partition_filters = true;
    table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
    rocksdb::DB* db = nullptr;
    const auto status = rocksdb::DB::Open(options, directory, &db);
    if (!status.ok()) return Convert(status);
    *output = new RocksEngine(db);
    return Status::OK();
  } catch (const std::exception& error) {
    return Status::IOError(error.what());
  }
}
