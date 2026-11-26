// Included INSIDE the experimental build's F2Kv class. No adapter-side journal.
 public:
  void ExperimentEnableWakeup(bool enabled) { experiment_wakeup_ = enabled; }
  void ExperimentOpenCommitLog(const std::string& directory, uint32_t lanes) {
    for (uint32_t lane=0; lane<lanes; ++lane)
      experiment_logs_.emplace_back(std::make_unique<experimental::BatchLog>(directory, lane));
    experimental::SyncDirectory(directory);
  }
  // apply() is the native operation plus Pending completion. This returns
  // staged completion only: the driver MUST NOT ACK until CommitBatch succeeds.
  template<class Apply>
  Status ExperimentStage(uint32_t lane, uint32_t op, const std::string& key,
                         const std::string& value, uint64_t serial, Apply apply) {
    const auto status = apply();
    if (status != Status::Ok) return status;
    experiment_logs_.at(lane)->Stage(op, key, value, serial);
    return Status::Ok;
  }
  uint64_t ExperimentCommitBatch(uint32_t lane) { return experiment_logs_.at(lane)->Commit(); }
  template<class Apply>
  uint64_t ExperimentRecoverCommitLog(Apply apply) {
    uint64_t count = 0;
    for (auto& log : experiment_logs_) count += log->Recover(apply);
    return count;
  }
  void ExperimentPrepareCheckpoint() { experimental::waiter = {}; }
  bool ExperimentBeginCheckpoint(Guid& token) { return Checkpoint(experimental::CheckpointDone, token, false); }
  uint64_t ExperimentWaitCheckpoint(uint64_t required) {
    auto next_diagnostic=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!experimental::waiter.done || checkpoint_.phase.load() != CheckpointPhase::REST) {
      CompletePending(false);
      if(std::getenv("F2_CP_TRACE") && std::chrono::steady_clock::now()>=next_diagnostic) {
        std::fprintf(stderr,"F2_CP_WAIT phase=%u hot_status=%u cold_status=%u callback=%u hot_sessions=%u cold_sessions=%u hot_phase=%u cold_phase=%u hot_flushed=%llu hot_tail=%llu cold_flushed=%llu cold_tail=%llu\n",
          unsigned(checkpoint_.phase.load()),unsigned(checkpoint_.hot_store_status.load()),unsigned(checkpoint_.cold_store_status.load()),
          unsigned(experimental::waiter.done),hot_store.NumActiveSessions(),cold_store.NumActiveSessions(),
          unsigned(hot_store.system_state_.phase()),unsigned(cold_store.system_state_.phase()),
          (unsigned long long)hot_store.hlog.flushed_until_address.control(),(unsigned long long)hot_store.hlog.GetTailAddress().control(),
          (unsigned long long)cold_store.hlog.flushed_until_address.control(),(unsigned long long)cold_store.hlog.GetTailAddress().control());
        std::fflush(stderr);next_diagnostic+=std::chrono::seconds(2);
      }
      std::this_thread::yield();
    }
    if (experimental::waiter.status != Status::Ok || experimental::waiter.serial < required)
      throw std::runtime_error("F2 checkpoint failed or did not cover required serial");
    return experimental::waiter.serial;
  }
 private:
  std::vector<std::unique_ptr<experimental::BatchLog>> experiment_logs_;
  std::condition_variable experiment_cv_;
  std::mutex experiment_mutex_;
  std::atomic<bool> experiment_wakeup_{true};
