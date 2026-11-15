 public:
  void SyncConfigure(const std::string& directory,const std::string& format,bool journal,uint64_t cutoff,
                     std::function<void()> touch,uint64_t reclaim_bytes) {
    sync_state_=std::make_unique<experimental::SyncState>();
    auto& s=*sync_state_;s.root=directory;s.format=format;s.journal=journal;s.serial=s.cutoff=cutoff;
    s.touch=std::move(touch);s.reclaim_bytes=reclaim_bytes;
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
      s.format);
#endif
    if(inject) experimental::Fault("after-root");
#ifdef F2_DURABLE_COMPACTION
    if(prefix_gc_publishing_)PrefixGcPhase("after-root-before-truncate");
#endif
    s.cutoff=s.serial;
#ifdef F2_DATA_PREFIX
    s.prefix.base_tail=s.prefix.stable_tail;
#endif
    ++s.full_checkpoints;
    if(s.log) s.log->ResetAfterCheckpoint();
    if(inject) experimental::Fault("after-reset");
    experimental::PurgeCheckpoints(s.root,token.ToString());
  }
#ifdef F2_DATA_PREFIX
  template<class Native>
  Status SyncMutate(uint32_t op,const std::string& key,const std::string& value,Native native) {
    auto& s=*sync_state_;
    std::unique_lock<std::mutex> request(s.mutex);
    if(s.poisoned)return Status::IOError;
    const uint64_t serial=s.serial+1;
    bool session=false;
    std::shared_lock<std::shared_mutex> gate(s.gate,std::defer_lock);
    std::unique_lock<std::shared_mutex> checkpoint_gate(s.gate,std::defer_lock);
    const char* minimum=std::getenv("F2_SYNC_FAULT_MIN_SERIAL");
    const bool inject=minimum && serial>=std::stoull(minimum);
    try {
      if(s.log){s.log->Stage(op,key,value,serial);s.log->Commit();}
      gate.lock();
      StartSession();session=true;
      if(std::getenv("F2_SYNC_FAIL_APPLY"))throw std::runtime_error("injected F2 apply failure");
      hot_store.PrefixBeginIndex();
      const auto status=native(serial);
      if(status!=Status::Ok)throw std::runtime_error("F2 single-request apply failed");
      s.serial=serial;
      PrefixCommit(serial,serial,1);
      if(inject)experimental::Fault("after-wal");
      hot_store.PrefixPublishIndex();
#ifdef F2_DURABLE_COMPACTION
      if(prefix_gc_copying_.load())++prefix_gc_writes_;
#endif
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
      if(session){CompletePending(true);StopSession();session=false;}
      if(inject)experimental::Fault("before-ack");
      return Status::Ok;
    } catch(...) {
      s.poisoned=true;
      hot_store.PrefixAbortIndex();
      if(session){try{CompletePending(true);StopSession();}catch(...){}}
      return Status::IOError;
    }
  }
#else
  // Group-commit path for non-prefix strategies (hybridlog, commit-log, checkpoint).
  // All concurrent Put/Delete callers queue here. The leader gathers within a
  // bounded window, writes one WAL frame, applies the batch to F2, then triggers
  // a checkpoint only when the WAL has grown past reclaim_bytes.
  template<class Native>
  Status SyncMutate(uint32_t op,const std::string& key,const std::string& value,Native native) {
    auto& s=*sync_state_;
    experimental::SyncRequest own{op,key,value,native};
    std::unique_lock<std::mutex> lock(s.mutex);
    if(s.poisoned) return Status::IOError;
    s.pending.push_back(&own);
    while(!own.done) {
      if(s.leading) { s.cv.wait(lock,[&]{return own.done||!s.leading;}); continue; }
      s.leading=true;
      // Bounded gathering window — lets concurrent callers join the batch without
      // allowing a single-threaded caller to spin forever.
      const char* wait_us_env=std::getenv("F2_WAL_GROUP_WAIT_US");
      const auto wait=std::chrono::microseconds(wait_us_env?std::stoull(wait_us_env):100);
      s.cv.wait_for(lock,wait);
      std::vector<experimental::SyncRequest*> group(s.pending.begin(),s.pending.end());
      s.pending.clear(); lock.unlock();
      bool session=false; bool failed=false;
      try {
        uint64_t next=s.serial;
        // One WAL fdatasync covers the entire batch.
        if(s.log) {
          for(auto* r:group) s.log->Stage(r->op,r->key,r->value,++next);
          s.log->Commit();
        }
        const char* minimum=std::getenv("F2_SYNC_FAULT_MIN_SERIAL");
        const bool inject=minimum && s.serial+group.size()>=std::stoull(minimum);
        if(inject) experimental::Fault("after-wal");
        std::unique_lock<std::shared_mutex> gate(s.gate);
        StartSession(); session=true;
        if(std::getenv("F2_SYNC_FAIL_APPLY"))throw std::runtime_error("injected F2 apply failure");
        next=s.serial;
        for(auto* r:group) {
          r->status=r->apply(++next);
          if(r->status!=Status::Ok) throw std::runtime_error("F2 group apply failed");
        }
        s.serial=next;
        if(!s.log || s.log->Bytes()>=s.reclaim_bytes) SyncCheckpointAndReclaim();
        if(inject) experimental::Fault("before-ack");
        CompletePending(true); StopSession(); session=false;
        ++s.groups; s.operations+=group.size();
      } catch(...) {
        failed=true;
        if(session){try{CompletePending(true);StopSession();}catch(...){}}
      }
      lock.lock();
      if(failed) {
        s.poisoned=true;
        for(auto* r:s.pending){r->status=Status::IOError;r->done=true;}
        s.pending.clear();
      }
      for(auto* r:group){r->status=failed?Status::IOError:r->status;r->done=true;}
      s.leading=false; s.cv.notify_all();
    }
    return own.status;
  }
#endif
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
  uint64_t SyncLogCommits() const {return sync_state_->groups;}
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
