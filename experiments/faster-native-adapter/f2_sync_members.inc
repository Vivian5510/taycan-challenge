 public:
  void SyncConfigure(const std::string& directory,const std::string& format,bool journal,uint64_t cutoff,
                     std::function<void()> touch,uint64_t reclaim_bytes,bool hybrid=false,
                     const std::string& base_token={}) {
    sync_state_=std::make_unique<experimental::SyncState>();
    auto& s=*sync_state_;s.root=directory;s.format=format;s.journal=journal;s.serial=s.cutoff=cutoff;
    s.touch=std::move(touch);s.reclaim_bytes=reclaim_bytes;
    s.hybrid=hybrid;s.base_token=base_token;
#ifdef F2_ROCKS_WAL
    s.rocks=std::make_unique<reference::Committer>(
#ifdef F2_DATA_PREFIX
      (s.root/"hot").string(),s.rocks_stats,
      [this](rocksdb::WriteBatch& batch,uint64_t first){return SyncApplyRocksBatch(batch,first);},cutoff,true,256<<10);
    s.rocks->UseDataLogCommit(true);
#else
      (s.root/"wal").string(),s.rocks_stats,
      [this](rocksdb::WriteBatch& batch,uint64_t first){return SyncApplyRocksBatch(batch,first);},cutoff,true);
#endif
    if(const char* mode=std::getenv("F2_WAL_APPLY_MODE"))
      if(std::string(mode)!="decoded")throw std::runtime_error("input borrowing prototype is archived");
    const char* collect=std::getenv("F2_WAL_GROUP_WAIT_US");
    s.rocks->SetCollectBudgetUs(collect?std::stoull(collect):50);
#endif
    if(journal) {
      s.log=std::make_unique<experimental::BatchLog>(s.root,0);
      experimental::SyncDirectory(s.root);
    }
  }
  template<class Apply>
  uint64_t SyncReplay(Apply apply) {
#ifdef F2_ROCKS_WAL
#ifdef F2_DATA_PREFIX
    (void)apply;
    return 0;  // Native data records were restored while recovering the index.
#else
    return SyncReplayRocks(apply);
#endif
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
#if defined(F2_ROCKS_WAL) && !defined(F2_DATA_PREFIX)
    const auto wal_path=(s.root/"wal"/(token.ToString()+".log")).string();
    auto next_wal=s.rocks->PrepareWal(wal_path);
#endif
    const char* min_fault=std::getenv("F2_SYNC_FAULT_MIN_SERIAL");
    const bool inject=min_fault && s.serial>=std::stoull(min_fault);
#if defined(F2_ROCKS_WAL) && !defined(F2_DATA_PREFIX)
    if(inject)experimental::Fault("after-new-wal");
#endif
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
#if defined(F2_ROCKS_WAL) && !defined(F2_DATA_PREFIX)
    s.rocks->InstallWal(wal_path,std::move(next_wal));
    s.base_token=token.ToString();
#endif
    s.cutoff=s.serial;
#ifdef F2_DATA_PREFIX
    s.prefix.base_tail=s.prefix.stable_tail;
#endif
    if(s.hybrid){s.base_token=token.ToString();s.base_tail=hot_store.hlog.GetTailAddress().control();}
    ++s.full_checkpoints;
    if(s.log) s.log->ResetAfterCheckpoint();
    if(inject) experimental::Fault("after-reset");
#if defined(F2_ROCKS_WAL) && !defined(F2_DATA_PREFIX)
    SyncPurgeOldRocksWal(token.ToString());
    if(inject)experimental::Fault("after-purge");
#endif
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
#ifdef F2_ROCKS_WAL
    (void)native;
    return SyncRocksMutate(op,key,value);
#else
    auto& s=*sync_state_;experimental::SyncRequest own{op,key,value,native};
    std::unique_lock<std::mutex> lock(s.mutex);
    if(s.poisoned) return Status::IOError;
    s.pending.push_back(&own);
    while(!own.done) {
      if(s.leading) { s.cv.wait(lock,[&]{return own.done||!s.leading;});continue; }
      s.leading=true;
      // Bounded gathering: a single synchronous caller can never fabricate a batch.
      s.cv.wait_for(lock,std::chrono::microseconds(100));
      std::vector<experimental::SyncRequest*> group(s.pending.begin(),s.pending.end());
      s.pending.clear();lock.unlock();
      bool session=false;bool failed=false;
      try {
        uint64_t next=s.serial;
        if(s.log) {
          for(auto* r:group) s.log->Stage(r->op,r->key,r->value,++next);
          s.log->Commit();
        }
        std::unique_lock<std::shared_mutex> gate(s.gate);
        StartSession();session=true;
        next=s.serial;
        for(auto* r:group) {
          r->status=r->apply(++next);
          if(r->status!=Status::Ok) throw std::runtime_error("native sync mutation failed");
        }
        s.serial=next;
#ifdef F2_HYBRID_EXPERIMENT
        if(s.hybrid)SyncCommitHybridPrefix();
        else
#endif
        if(!s.log) SyncCheckpointAndReclaim();
        else if(s.log->Bytes()>=s.reclaim_bytes) SyncCheckpointAndReclaim();
        CompletePending(true);StopSession();session=false;
        ++s.groups;s.operations+=group.size();
      } catch(...) {
        failed=true;
        if(session) { try { CompletePending(true);StopSession(); } catch(...){} }
      }
      lock.lock();
      if(failed) {
        s.poisoned=true;
        for(auto* r:s.pending) {r->status=Status::IOError;r->done=true;}
        s.pending.clear();
      }
      for(auto* r:group) { r->status=failed?Status::IOError:r->status;r->done=true; }
      s.leading=false;s.cv.notify_all();
    }
    return own.status;
#endif
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
#ifdef F2_PREFIX_PIPELINE
    PrefixStopPipeline();
#endif
#ifdef F2_ROCKS_WAL
    // All acknowledged requests are already recoverable from the active WAL.
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
