#pragma once
#include <cstdint>
#include <memory>
#include <string>

enum class F2ResultCode { ok, not_found, error };
struct F2Result {
  F2ResultCode code;
  std::string message;
};

class FasterBackend {
 public:
  virtual ~FasterBackend() = default;
  virtual F2Result Put(const std::string&, const std::string&) = 0;
  virtual F2Result Get(const std::string&, std::string*) = 0;
  virtual F2Result Delete(const std::string&) = 0;
  virtual F2Result CreateCheckpoint(const std::string&) {
    return {F2ResultCode::error, "Checkpoint is not supported"};
  }
};

std::unique_ptr<FasterBackend> CreateFasterBackend(const std::string&, uint64_t);
