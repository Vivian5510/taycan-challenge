#include "kv_engine.h"
#include "faster_backend.h"

#include <filesystem>
#include <memory>

namespace {
namespace fs = std::filesystem;

class Adapter final : public KVEngine {
 public:
  explicit Adapter(std::unique_ptr<FasterBackend> backend) : backend_(std::move(backend)) {}
  Status Put(const std::string& key, const std::string& value) override {
    return Convert(backend_->Put(key, value));
  }
  Status Get(const std::string& key, std::string* value) override {
    return Convert(backend_->Get(key, value));
  }
  Status Delete(const std::string& key) override {
    return Convert(backend_->Delete(key));
  }
 private:
  static Status Convert(const F2Result& result) {
    if (result.code == F2ResultCode::ok) return Status::OK();
    if (result.code == F2ResultCode::not_found) return Status::NotFound();
    return Status::IOError(result.message);
  }
  std::unique_ptr<FasterBackend> backend_;
};
}

Status KVEngine::Open(const std::string& path, uint64_t budget, KVEngine** output) {
  if (!output) return Status::IOError("null engine output");
  *output = nullptr;
  if (path.empty() || budget < (16ULL << 20)) return Status::IOError("invalid directory or budget");
  try {
    if (fs::is_symlink(fs::symlink_status(path))) return Status::IOError("symlink database directory");
    fs::create_directories(path);
    *output = new Adapter(CreateFasterBackend(path, budget));
    return Status::OK();
  } catch (const std::exception& error) {
    return Status::IOError(error.what());
  }
}
