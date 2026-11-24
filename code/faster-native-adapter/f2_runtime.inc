using Disk = FASTER::device::FileSystemDisk<FASTER::environment::QueueIoHandler, 1ULL << 30>;
using ColdIndex = fi::ColdIndex<Disk, fi::ColdLogHashIndexDefinition<8>>;
using Store = fc::F2Kv<Key, Value, Disk, fi::MemHashIndex<Disk>, ColdIndex>;

thread_local Completion* current_checkpoint = nullptr;
void CheckpointDone(FStatus status, uint64_t serial) {
  Trace("checkpoint.callback", serial);
  current_checkpoint->status.store(status, std::memory_order_relaxed);
  current_checkpoint->done.store(true, std::memory_order_release);
}
template<class Context>
void CompleteOperation(fc::IAsyncContext* pointer, FStatus status) {
  fc::CallbackContext<Context> context{pointer};
  context->result->status.store(status, std::memory_order_relaxed);
  context->result->done.store(true, std::memory_order_release);
}
F2Result Convert(FStatus status) {
  if (status == FStatus::Ok) return {F2ResultCode::ok, {}};
  if (status == FStatus::NotFound) return {F2ResultCode::not_found, {}};
  return {F2ResultCode::error, std::string("FASTER/F2: ") + fc::StatusStr(status)};
}
struct LockFile {
  int fd = -1;
  explicit LockFile(const std::string& path) {
    fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("cannot open LOCK");
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      ::close(fd);
      fd = -1;
      throw std::runtime_error("database directory is locked");
    }
  }
  ~LockFile() { if (fd >= 0) ::close(fd); }
};

class FasterF2Backend final : public FasterBackend {
 public:
  FasterF2Backend(const std::string& directory, uint64_t budget)
      : directory_(directory), lock_(directory + "/LOCK"), budget_(budget) {
    const auto current = directory_ + "/FASTER-CURRENT";
    const bool recover = fs::exists(current);
    std::string token;
    const std::string configuration = "f2-adapter-v3 " + std::to_string(F2_LOG_PAGE_BITS)
        + " " + std::to_string(F2_OVERFLOW_PAGE_BITS);
    if (recover) {
      std::ifstream input(current);
      std::getline(input, token);
      if (token.size() != 36) throw std::runtime_error("invalid checkpoint token");
      std::ifstream config_input(directory_ + "/F2-ADAPTER-CONFIG");
      std::string existing;
      std::getline(config_input, existing);
      if (existing != configuration)
        throw std::runtime_error("incompatible F2 adapter format or page geometry");
    } else {
      for (const auto& item : fs::directory_iterator(directory_))
        if (item.path().filename() != "LOCK")
          throw std::runtime_error("checkpoint token absent from non-empty database");
      PersistText("F2-ADAPTER-CONFIG", configuration + "\n");
    }
    uint64_t entries = 4096;
    while (entries < budget / 4096) entries *= 2;
    const uint64_t page_bytes = uint64_t{1} << F2_LOG_PAGE_BITS;
    const uint64_t log_memory = std::max<uint64_t>(
        6 * page_bytes, (budget / 8 / page_bytes) * page_bytes);
    Store::HotIndexConfig hot{entries};
    Store::ColdIndexConfig cold{entries, log_memory, 1.0/3.0};
    fc::ReadCacheConfig read_cache{};
    read_cache.enabled = false;
    auto compaction = fc::DEFAULT_F2_COMPACTION_CONFIG;
    // Correctness baseline: data/index compaction policy is evaluated separately.
    compaction.hot_store.enabled = false;
    compaction.cold_store.enabled = false;
    store_ = std::make_unique<Store>(hot, log_memory, directory_ + "/hot/",
        cold, log_memory, directory_ + "/cold/", 1.0/3.0, 0, read_cache, compaction);
    Trace("store.constructed");
    if (recover) {
      uint32_t version = 0;
      Trace("recover.begin");
      if (store_->Recover(fc::Guid::Parse(token), version, recovered_sessions_) != FStatus::Ok)
        throw std::runtime_error("F2 checkpoint recovery failed");
      Trace("recover.done", recovered_sessions_.size());
    } else {
      Checkpoint();
    }
    recovered_remaining_.store(recovered_sessions_.size(), std::memory_order_release);
  }

  ~FasterF2Backend() override {
    // KVEngine callers must finish before destruction. No frontend worker,
    // request queue, or store-wide operation lock is involved.
    if (!store_) return;
    Trace("destructor.begin");
    try { Checkpoint(); } catch (const std::exception& error) {
      std::fprintf(stderr, "F2 close checkpoint failed: %s\n", error.what());
    }
    store_.reset();
    Trace("destructor.done");
  }

  F2Result Put(const std::string& key, const std::string& value) override {
    if (key.empty()) return {F2ResultCode::error, "empty key"};
    if (uint64_t(key.size()) + value.size() + 128 > (1ULL << F2_LOG_PAGE_BITS) ||
        uint64_t(key.size()) + value.size() > budget_ / 4)
      return {F2ResultCode::error, "record exceeds prototype record limit"};
    auto result = std::make_shared<Completion>();
    UpsertContext context{key, value, result};
    return InSession([&](uint64_t serial) {
      return store_->Upsert(context, CompleteOperation<UpsertContext>, serial);
    }, result);
  }

  F2Result Get(const std::string& key, std::string* value) override {
    if (key.empty() || !value) return {F2ResultCode::error, "empty key or null output"};
    auto result = std::make_shared<Completion>();
    // Copy Key before clearing output: the interface permits &key == value.
    ReadContext context{key, result};
    value->clear();
    const auto status = InSession([&](uint64_t serial) {
      return store_->Read(context, CompleteOperation<ReadContext>, serial);
    }, result);
    if (status.code == F2ResultCode::ok) *value = std::move(result->value);
    return status;
  }

  F2Result Delete(const std::string& key) override {
    if (key.empty()) return {F2ResultCode::error, "empty key"};
    auto result = std::make_shared<Completion>();
    DeleteContext context{key, result};
    auto status = InSession([&](uint64_t serial) {
      return store_->Delete(context, CompleteOperation<DeleteContext>, serial);
    }, result);
    if (status.code == F2ResultCode::not_found) return {F2ResultCode::ok, {}};
    return status;
  }

 private:
  template<class F>
  F2Result InSession(F operation, const std::shared_ptr<Completion>& result) {
    uint64_t serial = 0;
    fc::Guid recovered;
    if (TakeRecoveredSession(&recovered)) serial = store_->ContinueSession(recovered);
    else store_->StartSession();
    FStatus status;
    try {
      status = operation(serial + 1);
      while (status == FStatus::Pending && !result->done.load(std::memory_order_acquire)) {
        store_->CompletePending(false);
        std::this_thread::yield();
      }
      if (status == FStatus::Pending) status = result->status.load(std::memory_order_acquire);
      store_->CompletePending(true);
      store_->StopSession();
    } catch (...) {
      store_->CompletePending(true);
      store_->StopSession();
      throw;
    }
    return Convert(status);
  }

  bool TakeRecoveredSession(fc::Guid* id) {
    // Recovery tokens are claimed once; no lock is used after they are taken.
    size_t remaining = recovered_remaining_.load(std::memory_order_relaxed);
    while (remaining != 0) {
      if (recovered_remaining_.compare_exchange_weak(
              remaining, remaining - 1, std::memory_order_acq_rel)) {
        *id = recovered_sessions_[remaining - 1];
        return true;
      }
    }
    return false;
  }

  void Checkpoint() {
    Trace("checkpoint.begin");
    store_->StartSession();
    auto progress = std::make_shared<Completion>();
    ReadContext context{"f2-adapter-checkpoint-progress", progress};
    const auto status = store_->Read(context, CompleteOperation<ReadContext>, 1);
    if (status == FStatus::Pending)
      while (!progress->done.load(std::memory_order_acquire)) store_->CompletePending(false);
    store_->CompletePending(true);
    Completion result;
    current_checkpoint = &result;
    fc::Guid token;
    if (!store_->Checkpoint(CheckpointDone, token, false)) {
      current_checkpoint = nullptr;
      store_->StopSession();
      throw std::runtime_error("could not start F2 checkpoint");
    }
    while (!result.done.load(std::memory_order_acquire)) {
      store_->CompletePending(false);
      std::this_thread::yield();
    }
    store_->CompletePending(true);
    store_->StopSession();
    current_checkpoint = nullptr;
    if (result.status.load(std::memory_order_acquire) != FStatus::Ok)
      throw std::runtime_error("F2 checkpoint failed");
    PersistText("FASTER-CURRENT", token.ToString() + "\n");
    Trace("checkpoint.done");
  }

  void PersistText(const std::string& name, const std::string& text) {
    const auto temporary = directory_ + "/" + name + ".tmp";
    const int fd = ::open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("create metadata failed");
    size_t offset = 0;
    while (offset < text.size()) {
      const ssize_t n = ::write(fd, text.data() + offset, text.size() - offset);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) { ::close(fd); throw std::runtime_error("write metadata failed"); }
      offset += size_t(n);
    }
#ifdef __APPLE__
    const int synced = ::fcntl(fd, F_FULLFSYNC);
#else
    const int synced = ::fsync(fd);
#endif
    ::close(fd);
    if (synced != 0) throw std::runtime_error("sync metadata failed");
    if (::rename(temporary.c_str(), (directory_ + "/" + name).c_str()) != 0)
      throw std::runtime_error("publish metadata failed");
    const int dirfd = ::open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd < 0) throw std::runtime_error("open data directory failed");
    const int dir_synced = ::fsync(dirfd);
    ::close(dirfd);
    if (dir_synced != 0) throw std::runtime_error("directory sync failed");
  }

  std::string directory_;
  LockFile lock_;
  uint64_t budget_;
  std::unique_ptr<Store> store_;
  std::vector<fc::Guid> recovered_sessions_;
  std::atomic<size_t> recovered_remaining_{0};
};
}

std::unique_ptr<FasterBackend> CreateFasterBackend(const std::string& directory, uint64_t budget) {
  return std::make_unique<FasterF2Backend>(directory, budget);
}
