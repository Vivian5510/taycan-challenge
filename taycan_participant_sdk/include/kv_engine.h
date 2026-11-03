#pragma once

#include <cstdint>
#include <string>

#include "status.h"

class KVEngine {
 public:
  // Implement this static factory in the submission (it is not virtual).
  // Create data_dir if absent; otherwise recover it. On failure set
  // *engine=nullptr. mem_limit_bytes: index memory budget. The Python runner
  // checks process RSS; see README.md for the allowance and full contract.
  static Status Open(const std::string& data_dir, uint64_t mem_limit_bytes,
                     KVEngine** engine);

  virtual Status Put(const std::string& key, const std::string& value) = 0;
  virtual Status Get(const std::string& key, std::string* value) = 0;
  virtual Status Delete(const std::string& key) = 0;

  virtual Status CreateCheckpoint(const std::string& checkpoint_dir) {
    (void)checkpoint_dir;
    return Status::IOError("Checkpoint is not supported");
  }

  virtual ~KVEngine() = default;
};
