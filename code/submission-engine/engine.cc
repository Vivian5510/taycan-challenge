#include "kv_engine.h"
#include "faster_backend.h"

#include <filesystem>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <unistd.h>

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
  Status CreateCheckpoint(const std::string& directory) override {
    return Convert(backend_->CreateCheckpoint(directory));
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
    const auto root=fs::absolute(path).lexically_normal();
    const auto incomplete=root/"F2-CHECKPOINT-INCOMPLETE";
    if(fs::exists(incomplete)) {
      const bool committed=fs::is_regular_file(root/"F2-SYNC-CURRENT")||
                           fs::is_regular_file(root/"F2-SHARDS");
      if(!committed)return Status::IOError("checkpoint creation was interrupted");
      if(!fs::remove(incomplete))return Status::IOError("cannot finalize checkpoint marker");
      const int fd=::open(root.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
      if(fd<0)return Status::IOError("cannot open checkpoint directory");
      int result;
      do {result=::fsync(fd);} while(result<0&&errno==EINTR);
      ::close(fd);
      if(result<0)return Status::IOError("cannot sync checkpoint directory");
    }
    *output = new Adapter(CreateFasterBackend(path, budget));
    return Status::OK();
  } catch (const std::exception& error) {
    return Status::IOError(error.what());
  }
}
