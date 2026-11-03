// F2-native C++ adaptation of Tsavorite's outstanding commit requests,
// covered-commit completion and MaxParallel admission. See SOURCE_REUSE.md.
// It does not copy C# runtime/allocator code or weaken stable-storage sync.
 private:
  struct PrefixPipelineRequest {
    uint32_t operation;
    const std::string *key,*value;
    bool done=false;
    Status result=Status::IOError;
#if F2_PIPELINE_LOCAL_WAIT
    reference::CommitWait::Writer waiter;
#elif F2_PIPELINE_TARGETED_WAKE
    std::condition_variable completion;
#endif
  };
  struct PrefixPipelineCommit {
    uint64_t first=0,last=0,end=0;
    bool synced=false,failed=false;
    std::vector<PrefixPipelineRequest*> requests;
    typename hot_faster_store_t::PrefixPreparedIndex index;
    std::map<std::string,std::shared_ptr<experimental::PrefixSyncDescriptor>> dependencies;
  };
  using prefix_commit_ptr=std::shared_ptr<PrefixPipelineCommit>;
  std::mutex prefix_pipeline_mutex_;
  std::condition_variable prefix_pipeline_cv_;
  std::deque<PrefixPipelineRequest*> prefix_pipeline_pending_;
  std::deque<prefix_commit_ptr> prefix_pipeline_outstanding_,prefix_pipeline_flush_queue_;
  std::thread prefix_pipeline_preparer_,prefix_pipeline_flusher_;
  bool prefix_pipeline_closing_=false,prefix_pipeline_prepare_done_=false;
  bool prefix_pipeline_preparing_=false,prefix_pipeline_publishing_=false,prefix_pipeline_checkpoint_fence_=false;
  std::atomic<bool> prefix_pipeline_sync_active_{false};
  uint64_t prefix_pipeline_peak_=0,prefix_pipeline_prepared_=0,prefix_pipeline_overlap_=0;
  uint64_t prefix_pipeline_sync_batches_=0,prefix_pipeline_covered_=0;
  std::atomic<size_t> prefix_pipeline_active_callers_{0};
#if F2_PIPELINE_LOCAL_WAIT
  reference::CommitWait prefix_request_wait_;
#endif
  bool prefix_pipeline_test_hold_used_=false;
  std::atomic<uint64_t> prefix_profile_prepare_ns_{0},prefix_profile_sync_ns_{0},prefix_profile_publish_ns_{0};
  uint64_t prefix_profile_predicate_checks_=0,prefix_profile_callers_=0;

  static bool PrefixPipelineFaultEnabled(uint64_t serial) {
    const char* minimum=std::getenv("F2_SYNC_FAULT_MIN_SERIAL");
    return minimum && serial>=std::stoull(minimum);
  }
  void PrefixPipelineTakeDependencies(PrefixPipelineCommit& commit) {
    const std::string prefix=(sync_state_->root/"hot"/"log.log").string();
    std::lock_guard<std::mutex> lock(experimental::dirty_mutex);
    for(const auto& path:experimental::failed_paths)
      if(path.compare(0,prefix.size(),prefix)==0)throw std::runtime_error("pipeline data I/O failed");
    for(auto it=experimental::dirty_paths.begin();it!=experimental::dirty_paths.end();) {
      if(it->compare(0,prefix.size(),prefix)!=0){++it;continue;}
      auto descriptor=experimental::dirty_data_descriptors.find(*it);
      if(descriptor==experimental::dirty_data_descriptors.end())throw std::runtime_error("pipeline lost write descriptor");
      commit.dependencies.emplace(*it,std::move(descriptor->second));
      experimental::dirty_data_descriptors.erase(descriptor);it=experimental::dirty_paths.erase(it);
    }
    if(commit.dependencies.empty())throw std::runtime_error("pipeline commit has no completed writes");
  }
  void PrefixPipelinePrepare(PrefixPipelineCommit& commit) {
    auto& s=*sync_state_;bool session=false;
    std::shared_lock<std::shared_mutex> gate(s.gate);
    try {
      const auto profile_begin=reference::ProfileNow();
      StartSession();session=true;hot_store.PrefixBeginIndex();commit.first=s.serial+1;
      for(auto* request:commit.requests) {
        if(std::getenv("F2_SYNC_FAIL_APPLY"))throw std::runtime_error("injected pipeline apply failure");
        if(s.rocks_apply(request->operation,*request->key,*request->value,++s.serial)!=Status::Ok)
          throw std::runtime_error("pipeline F2 apply failed");
      }
      commit.last=s.serial;CompletePending(true);
      PrefixCommit(commit.first,commit.last,commit.requests.size()); // seal + OS write, not sync
      commit.end=s.prefix.prepared_tail;commit.index=hot_store.PrefixDetachIndex();
      PrefixPipelineTakeDependencies(commit);
      StopSession();session=false;++prefix_pipeline_prepared_;
      if constexpr(reference::kProfileWal)prefix_profile_prepare_ns_+=reference::ElapsedNs(profile_begin);
    }catch(...) {
      hot_store.PrefixAbortIndex();
      if(session){try{CompletePending(true);StopSession();}catch(...){}}
      throw;
    }
  }
  void PrefixPipelineSync(std::vector<prefix_commit_ptr>& commits) {
    const auto profile_begin=reference::ProfileNow();
    auto& s=*sync_state_;
    std::map<std::string,std::shared_ptr<experimental::PrefixSyncDescriptor>> dependencies;
    for(const auto& commit:commits)
      for(const auto& entry:commit->dependencies)dependencies.insert_or_assign(entry.first,entry.second);
    const auto last=commits.back()->last;
    // Explicit test-only handshake: hold before the real durable sync so a
    // probe can assert old committed reads while later groups are prepared.
    if(!prefix_pipeline_test_hold_used_) {
      const char* release=std::getenv("F2_PIPELINE_TEST_RELEASE_FD");
      const char* notice=std::getenv("F2_PIPELINE_TEST_NOTICE_FD");
      if(release && notice) {
        prefix_pipeline_test_hold_used_=true;
        const int release_fd=std::stoi(release),notice_fd=std::stoi(notice);
        const char ready='R';char go=0;
        if(::write(notice_fd,&ready,1)!=1 || ::read(release_fd,&go,1)!=1 || go!='G')
          throw std::runtime_error("pipeline test sync handshake failed");
        std::lock_guard<std::mutex> lock(prefix_pipeline_mutex_);
        std::fprintf(stderr,"F2_PIPELINE_HELD outstanding=%zu\n",prefix_pipeline_outstanding_.size());
      }
    }
    if(PrefixPipelineFaultEnabled(last))experimental::Fault("before-data-sync");
    for(const auto& entry:dependencies) {
      experimental::Sync(entry.second->fd);++s.prefix.data_syncs;
      if(s.prefix.synced_files.insert(entry.first).second) {
        experimental::SyncDirectory(std::filesystem::path(entry.first).parent_path());++s.prefix.directory_syncs;
      }
    }
    if(PrefixPipelineFaultEnabled(last))experimental::Fault("after-data-sync");
    ++prefix_pipeline_sync_batches_;prefix_pipeline_covered_+=commits.size();
    if constexpr(reference::kProfileWal)prefix_profile_sync_ns_+=reference::ElapsedNs(profile_begin);
  }
  void PrefixPipelineFlushLoop() {
    for(;;) {
      std::vector<prefix_commit_ptr> commits;
      {
        std::unique_lock<std::mutex> lock(prefix_pipeline_mutex_);
        prefix_pipeline_cv_.wait(lock,[&]{return !prefix_pipeline_flush_queue_.empty() || prefix_pipeline_prepare_done_;});
        if(prefix_pipeline_flush_queue_.empty())return;
#if F2_PIPELINE_SYNC_COLLECT_US > 0
        if(F2_PIPELINE_DEPTH>1) {
          // Finishing a durable prefix releases a wave of synchronous callers.
          // Give its callbacks a bounded chance to prepare the next wave, then
          // cover every already queued preparation before the next barrier.
          // Starting a barrier immediately on one fragment splits the batch.
          if(prefix_pipeline_active_callers_>1)
            prefix_pipeline_cv_.wait_for(lock,std::chrono::microseconds(F2_PIPELINE_SYNC_COLLECT_US),[&]{
              return prefix_pipeline_closing_ || sync_state_->poisoned || prefix_pipeline_outstanding_.size()>=F2_PIPELINE_DEPTH;
            });
          prefix_pipeline_cv_.wait(lock,[&]{
            return prefix_pipeline_prepare_done_ || sync_state_->poisoned || prefix_pipeline_checkpoint_fence_ ||
              prefix_pipeline_outstanding_.size()>=F2_PIPELINE_DEPTH ||
              (!prefix_pipeline_preparing_ && !prefix_pipeline_publishing_ && prefix_pipeline_pending_.empty() &&
               (prefix_pipeline_outstanding_.empty() || !prefix_pipeline_outstanding_.front()->synced));
          });
        }
#endif
        if(sync_state_->poisoned){prefix_pipeline_flush_queue_.clear();continue;}
        while(!prefix_pipeline_flush_queue_.empty()) {
          commits.push_back(std::move(prefix_pipeline_flush_queue_.front()));prefix_pipeline_flush_queue_.pop_front();
        }
        prefix_pipeline_sync_active_=true;
      }
      bool failed=false;
      try{PrefixPipelineSync(commits);}catch(const std::exception& error){
        std::fprintf(stderr,"F2_PIPELINE_SYNC_ERROR %s\n",error.what());failed=true;sync_state_->poisoned=true;
      }
      {
        std::lock_guard<std::mutex> lock(prefix_pipeline_mutex_);
        for(auto& commit:commits){commit->failed=failed;commit->synced=true;commit->dependencies.clear();}
        prefix_pipeline_sync_active_=false;
      }
      prefix_pipeline_cv_.notify_all();
    }
  }
  void PrefixPipelinePublish(PrefixPipelineCommit& commit,bool last_outstanding) {
    const auto profile_begin=reference::ProfileNow();
    auto& s=*sync_state_;
    if(commit.failed || s.poisoned)throw std::runtime_error("pipeline sync failed");
    if(PrefixPipelineFaultEnabled(commit.last))experimental::Fault("after-wal");
    hot_store.PrefixPublishPreparedIndex(commit.index);
    s.prefix.stable_tail=commit.end;++s.prefix.commits;
    ++s.groups;++s.log_commits;s.operations+=commit.requests.size();
    if(last_outstanding && s.prefix.prepared_tail-s.prefix.base_tail>=s.reclaim_bytes) {
      std::unique_lock<std::shared_mutex> gate(s.gate);bool session=false;
      try{StartSession();session=true;SyncCheckpointAndReclaim();StopSession();session=false;}
      catch(...){if(session){try{CompletePending(true);StopSession();}catch(...){}}throw;}
    }
    if(PrefixPipelineFaultEnabled(commit.last))experimental::Fault("before-ack");
#if !F2_PIPELINE_LOCAL_WAIT
    std::lock_guard<std::mutex> lock(prefix_pipeline_mutex_);
#endif
    for(auto* request:commit.requests){request->result=Status::Ok;
#if F2_PIPELINE_LOCAL_WAIT
      prefix_request_wait_.Complete(&request->waiter); // Last access: caller can now destroy it.
#else
      request->done=true;
#if F2_PIPELINE_TARGETED_WAKE
      request->completion.notify_one();
#endif
#endif
    }
    prefix_pipeline_cv_.notify_all();
    if constexpr(reference::kProfileWal)prefix_profile_publish_ns_+=reference::ElapsedNs(profile_begin);
  }
  void PrefixPipelinePrepareLoop() {
    prefix_commit_ptr current;
    try {
      for(;;) {
        std::unique_lock<std::mutex> lock(prefix_pipeline_mutex_);
        if(sync_state_->poisoned)throw std::runtime_error("pipeline instance failed");
        if(!prefix_pipeline_outstanding_.empty() && prefix_pipeline_outstanding_.front()->synced) {
          current=std::move(prefix_pipeline_outstanding_.front());prefix_pipeline_outstanding_.pop_front();
          prefix_pipeline_publishing_=true;
          const bool empty=prefix_pipeline_outstanding_.empty();lock.unlock();
          PrefixPipelinePublish(*current,empty);current.reset();lock.lock();
          prefix_pipeline_publishing_=false;
          prefix_pipeline_checkpoint_fence_=sync_state_->prefix.prepared_tail-sync_state_->prefix.base_tail>=sync_state_->reclaim_bytes;
          lock.unlock();prefix_pipeline_cv_.notify_all();continue;
        }
        if(prefix_pipeline_closing_ && prefix_pipeline_pending_.empty() && prefix_pipeline_outstanding_.empty()) {
          prefix_pipeline_prepare_done_=true;lock.unlock();prefix_pipeline_cv_.notify_all();return;
        }
        const bool checkpoint_due=sync_state_->prefix.prepared_tail-sync_state_->prefix.base_tail>=sync_state_->reclaim_bytes;
        if(!prefix_pipeline_pending_.empty() && prefix_pipeline_outstanding_.size()<F2_PIPELINE_DEPTH && !checkpoint_due) {
          // A fixed collection delay can dominate a fast sync. Keep it bounded
          // and independently disableable; never wait to fill a required batch.
#if F2_PIPELINE_PREPARE_COLLECT_US > 0
          if(prefix_pipeline_pending_.size()>1 && prefix_pipeline_pending_.size()<F2_PIPELINE_GROUP_OPS)
            prefix_pipeline_cv_.wait_for(lock,std::chrono::microseconds(F2_PIPELINE_PREPARE_COLLECT_US),[&]{
              return prefix_pipeline_pending_.size()>=F2_PIPELINE_GROUP_OPS || prefix_pipeline_closing_;});
#endif
          current=std::make_shared<PrefixPipelineCommit>();size_t bytes=0;
          while(!prefix_pipeline_pending_.empty() && current->requests.size()<F2_PIPELINE_GROUP_OPS) {
            auto* request=prefix_pipeline_pending_.front();
            const size_t size=request->key->size()+request->value->size()+128;
            if(!current->requests.empty() && bytes+size>(256ULL<<10))break;
            bytes+=size;current->requests.push_back(request);prefix_pipeline_pending_.pop_front();
          }
          const bool overlapping=prefix_pipeline_sync_active_.load();
          prefix_pipeline_preparing_=true;
          lock.unlock();PrefixPipelinePrepare(*current);lock.lock();
          prefix_pipeline_preparing_=false;
          prefix_pipeline_checkpoint_fence_=sync_state_->prefix.prepared_tail-sync_state_->prefix.base_tail>=sync_state_->reclaim_bytes;
          if(overlapping)++prefix_pipeline_overlap_;
          prefix_pipeline_outstanding_.push_back(current);prefix_pipeline_flush_queue_.push_back(current);
          prefix_pipeline_peak_=std::max<uint64_t>(prefix_pipeline_peak_,prefix_pipeline_outstanding_.size());
          if(overlapping && PrefixPipelineFaultEnabled(current->last))experimental::Fault("after-overlapping-prepare");
          current.reset();lock.unlock();prefix_pipeline_cv_.notify_all();continue;
        }
        prefix_pipeline_cv_.wait(lock);
      }
    }catch(const std::exception& error) {
      std::fprintf(stderr,"F2_PIPELINE_PREPARE_ERROR %s\n",error.what());sync_state_->poisoned=true;
      hot_store.PrefixAbortIndex();
      std::lock_guard<std::mutex> lock(prefix_pipeline_mutex_);
      auto fail=[this](PrefixPipelineRequest* r){r->result=Status::IOError;
#if F2_PIPELINE_LOCAL_WAIT
        prefix_request_wait_.Complete(&r->waiter);
#else
        r->done=true;
#if F2_PIPELINE_TARGETED_WAKE
        r->completion.notify_one();
#endif
#endif
      };
      if(current)for(auto* request:current->requests)fail(request);
      for(auto* request:prefix_pipeline_pending_)fail(request);
      for(auto& commit:prefix_pipeline_outstanding_)for(auto* request:commit->requests)fail(request);
      prefix_pipeline_pending_.clear();prefix_pipeline_outstanding_.clear();prefix_pipeline_flush_queue_.clear();
      prefix_pipeline_prepare_done_=true;prefix_pipeline_closing_=true;prefix_pipeline_cv_.notify_all();
    }
  }
 public:
  void PrefixStartPipeline() {
    try {
      prefix_pipeline_flusher_=std::thread([this]{PrefixPipelineFlushLoop();});
      prefix_pipeline_preparer_=std::thread([this]{PrefixPipelinePrepareLoop();});
    }catch(...) {
      {std::lock_guard<std::mutex> lock(prefix_pipeline_mutex_);prefix_pipeline_prepare_done_=true;}
      prefix_pipeline_cv_.notify_all();if(prefix_pipeline_flusher_.joinable())prefix_pipeline_flusher_.join();throw;
    }
  }
  void PrefixStopPipeline() {
    {std::lock_guard<std::mutex> lock(prefix_pipeline_mutex_);prefix_pipeline_closing_=true;}
    prefix_pipeline_cv_.notify_all();
    if(prefix_pipeline_preparer_.joinable())prefix_pipeline_preparer_.join();
    if(prefix_pipeline_flusher_.joinable())prefix_pipeline_flusher_.join();
  }
  Status PrefixPipelineMutate(uint32_t op,const std::string& key,const std::string& value) {
    PrefixPipelineRequest request{op,&key,&value};
    std::unique_lock<std::mutex> lock(prefix_pipeline_mutex_);
    if(sync_state_->poisoned || prefix_pipeline_closing_)return Status::IOError;
    ++prefix_pipeline_active_callers_;
    prefix_pipeline_pending_.push_back(&request);prefix_pipeline_cv_.notify_all();
    uint64_t checks=0;
    auto finished=[&]{if constexpr(reference::kProfileWal)++checks;return request.done;};
#if F2_PIPELINE_LOCAL_WAIT
    lock.unlock();
    prefix_request_wait_.Wait(&request.waiter);
    if constexpr(reference::kProfileWal)lock.lock();
#elif F2_PIPELINE_TARGETED_WAKE
    request.completion.wait(lock,finished);
#else
    prefix_pipeline_cv_.wait(lock,finished);
#endif
    if constexpr(reference::kProfileWal){prefix_profile_predicate_checks_+=checks;++prefix_profile_callers_;}
    --prefix_pipeline_active_callers_;return request.result;
  }
  void PrefixPrintPipelineCounters() const {
    std::fprintf(stderr,"F2_PREFIX_PIPELINE depth=%u max_ops=%u sync_collect_us=%u prepare_collect_us=%u prepared=%llu peak_outstanding=%llu prepared_while_sync=%llu sync_batches=%llu covered=%llu\n",
      F2_PIPELINE_DEPTH,F2_PIPELINE_GROUP_OPS,F2_PIPELINE_SYNC_COLLECT_US,F2_PIPELINE_PREPARE_COLLECT_US,(unsigned long long)prefix_pipeline_prepared_,
      (unsigned long long)prefix_pipeline_peak_,(unsigned long long)prefix_pipeline_overlap_,
      (unsigned long long)prefix_pipeline_sync_batches_,(unsigned long long)prefix_pipeline_covered_);
    if constexpr(reference::kProfileWal)
      std::fprintf(stderr,"F2_PIPELINE_PROFILE prepare_ns=%llu sync_ns=%llu publish_ns=%llu predicate_checks=%llu callers=%llu targeted_wake=%u\n",
        (unsigned long long)prefix_profile_prepare_ns_.load(),(unsigned long long)prefix_profile_sync_ns_.load(),
        (unsigned long long)prefix_profile_publish_ns_.load(),(unsigned long long)prefix_profile_predicate_checks_,
        (unsigned long long)prefix_profile_callers_,F2_PIPELINE_TARGETED_WAKE);
  }
