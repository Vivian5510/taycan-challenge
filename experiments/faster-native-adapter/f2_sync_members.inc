 public:
  void SyncConfigure(const std::string& directory,const std::string& format,bool journal,uint64_t cutoff,
                     std::function<void()> touch,uint64_t reclaim_bytes,bool hybrid=false,
                     const std::string& base_token={}) {
    sync_state_=std::make_unique<experimental::SyncState>();
    auto& s=*sync_state_;s.root=directory;s.format=format;s.journal=journal;s.serial=s.cutoff=cutoff;
    s.touch=std::move(touch);s.reclaim_bytes=reclaim_bytes;
    s.hybrid=hybrid;s.base_token=base_token;
    if(journal) {
      s.log=std::make_unique<experimental::BatchLog>(s.root,0);
      experimental::SyncDirectory(s.root);
    }
  }
  template<class Apply>
  uint64_t SyncReplay(Apply apply) {
#ifdef F2_DATA_PREFIX
    (void)apply;
    return 0;  // Native data records were restored while recovering the index.
#else
    auto& s=*sync_state_;
    uint64_t count=s.log?s.log->Recover(apply,s.cutoff):0;
    if(s.log) s.serial=std::max(s.serial,s.log->last_serial());
    return count;
#endif
  }
  void SyncCheckpointAndReclaim() {
    auto& s=*sync_state_;
#ifdef F2_DATA_PREFIX
    PrefixSealBase();
#endif
    s.touch();
    Guid token;ExperimentPrepareCheckpoint();
    if(!ExperimentBeginCheckpoint(token)) throw std::runtime_error("sync checkpoint rejected");
    ExperimentWaitCheckpoint(s.serial?s.serial:1);
    experimental::FlushStore(s.root,token.ToString());
#ifdef F2_DURABLE_COMPACTION
    if(prefix_gc_publishing_)PrefixGcPhase("after-checkpoint-before-root");
#endif
    const char* min_fault=std::getenv("F2_SYNC_FAULT_MIN_SERIAL");
    const bool inject=min_fault && s.serial>=std::stoull(min_fault);
    if(inject) experimental::Fault("before-root");
    experimental::PublishRoot(s.root,token.ToString(),s.serial,
#ifdef F2_DATA_PREFIX
      s.format+"\n"+std::to_string(s.prefix.stable_tail));
#else
      s.hybrid?s.format+"\n"+token.ToString():s.format);
#endif
    if(inject) experimental::Fault("after-root");
#ifdef F2_DURABLE_COMPACTION
    if(prefix_gc_publishing_)PrefixGcPhase("after-root-before-truncate");
#endif
    s.cutoff=s.serial;
#ifdef F2_DATA_PREFIX
    s.prefix.base_tail=s.prefix.stable_tail;
#endif
    if(s.hybrid){s.base_token=token.ToString();s.base_tail=hot_store.hlog.GetTailAddress().control();}
    ++s.full_checkpoints;
    if(s.log) s.log->ResetAfterCheckpoint();
    if(inject) experimental::Fault("after-reset");
    experimental::PurgeCheckpoints(s.root,token.ToString());
  }
#ifdef F2_HYBRID_EXPERIMENT
  void SyncCommitHybridPrefix() {
    auto& s=*sync_state_;
    Guid token=HybridLogCommit(s.serial);
    const char* fault_serial=std::getenv("F2_SYNC_FAULT_MIN_SERIAL");
    bool inject=fault_serial && s.serial>=std::stoull(fault_serial);
    if(inject)experimental::Fault("before-sync");
    // These are I/O-completion frontiers until dependencies are synced below.
    experimental::FlushStore(s.root,token.ToString());
    if(inject)experimental::Fault("after-sync");
    if(inject)experimental::Fault("before-root");
    experimental::PublishRoot(s.root,token.ToString(),s.serial,s.format+"\n"+s.base_token);
    if(inject)experimental::Fault("after-root");
    s.cutoff=s.serial;++s.log_commits;
    // Retain the full base and newest log metadata; previous descriptors no
    // longer have a published root. Data log segments are never deleted here.
    experimental::PurgeHybridMetadata(s.root,s.base_token,token.ToString());
    if(hot_store.hlog.GetTailAddress().control()-s.base_tail>=s.reclaim_bytes)
      SyncCheckpointAndReclaim();
  }
#endif
  template<class Native>
  Status SyncMutate(uint32_t op,const std::string& key,const std::string& value,Native native) {
    auto& s=*sync_state_;
    std::unique_lock<std::mutex> request(s.mutex);
    if(s.poisoned)return Status::IOError;
    const uint64_t serial=s.serial+1;
    bool session=false;
#ifdef F2_DATA_PREFIX
    std::shared_lock<std::shared_mutex> gate(s.gate,std::defer_lock);
    std::unique_lock<std::shared_mutex> checkpoint_gate(s.gate,std::defer_lock);
#else
    std::unique_lock<std::shared_mutex> gate(s.gate,std::defer_lock);
#endif
    const char* minimum=std::getenv("F2_SYNC_FAULT_MIN_SERIAL");
    const bool inject=minimum && serial>=std::stoull(minimum);
    try {
      if(s.log){s.log->Stage(op,key,value,serial);s.log->Commit();}
      gate.lock();
      StartSession();session=true;
      if(std::getenv("F2_SYNC_FAIL_APPLY"))throw std::runtime_error("injected F2 apply failure");
#ifdef F2_DATA_PREFIX
      hot_store.PrefixBeginIndex();
#endif
      const auto status=native(serial);
      if(status!=Status::Ok)throw std::runtime_error("F2 single-request apply failed");
      s.serial=serial;
#ifdef F2_DATA_PREFIX
      PrefixCommit(serial,serial,1);
      if(inject)experimental::Fault("after-wal");
      hot_store.PrefixPublishIndex();
#ifdef F2_DURABLE_COMPACTION
      if(prefix_gc_copying_.load())++prefix_gc_writes_;
#endif
      ++s.log_commits;
      s.prefix.last_mutation_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
      StopSession();session=false;
      const bool needs_checkpoint=s.prefix.stable_tail-s.prefix.base_tail>=s.reclaim_bytes
#ifdef F2_DURABLE_COMPACTION
        && !prefix_gc_active_.load()
#endif
        ;
      gate.unlock();
      if(needs_checkpoint) {
        checkpoint_gate.lock();
        if(s.prefix.stable_tail-s.prefix.base_tail>=s.reclaim_bytes) {
          StartSession();session=true;SyncCheckpointAndReclaim();StopSession();session=false;
        }
      }
#elif defined(F2_HYBRID_EXPERIMENT)
      if(s.hybrid)SyncCommitHybridPrefix();
      else if(!s.log || s.log->Bytes()>=s.reclaim_bytes)SyncCheckpointAndReclaim();
#else
      if(!s.log || s.log->Bytes()>=s.reclaim_bytes)SyncCheckpointAndReclaim();
#endif
      if(session){CompletePending(true);StopSession();session=false;}
      if(inject)experimental::Fault("before-ack");
      return Status::Ok;
    } catch(...) {
      // Keep the maintenance gate held until session/index cleanup finishes.
      s.poisoned=true;
#ifdef F2_DATA_PREFIX
      hot_store.PrefixAbortIndex();
#endif
      if(session){try{CompletePending(true);StopSession();}catch(...){}}
      return Status::IOError;
    }
  }
  template<class Native>
  Status SyncRead(Native native) {
#if defined(__APPLE__) && defined(F2_NATIVE_IO_PROFILE)
    FASTER::environment::NativeIoReadScope io_read_scope;
#endif
    auto& s=*sync_state_;std::shared_lock<std::shared_mutex> gate(s.gate);
#ifdef F2_DURABLE_COMPACTION
    if(prefix_gc_copying_.load())++prefix_gc_reads_;
#endif
    if(s.poisoned) return Status::IOError;
    StartSession();
    try {auto result=native();CompletePending(true);StopSession();return result;}
    catch(...) {CompletePending(true);StopSession();throw;}
  }
  uint64_t SyncSerial() const {return sync_state_->serial;}
  uint64_t SyncLogCommits() const {return sync_state_->log_commits;}
  uint64_t SyncFullCheckpoints() const {return sync_state_->full_checkpoints;}
  void SyncClose() {
#ifdef F2_DURABLE_COMPACTION
    PrefixGcStop();
#endif
#ifdef F2_DATA_PREFIX
    // Each acknowledged data-log operation is already recoverable. Closing
    // must not turn single-request commits into per-close checkpoints.
    return;
#else
    auto& s=*sync_state_;std::unique_lock<std::shared_mutex> gate(s.gate);
    if(!s.poisoned && s.serial!=s.cutoff) {
      StartSession();try {SyncCheckpointAndReclaim();StopSession();}
      catch(...) {StopSession();throw;}
    }
#endif
  }
 private:
  std::unique_ptr<experimental::SyncState> sync_state_;
