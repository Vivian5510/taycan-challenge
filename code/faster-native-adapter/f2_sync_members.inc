 public:
#ifdef F2_LINUX_DATA_PREFIX
  void PrefixDurableLoop() {
    auto& s=*sync_state_;
    for (;;) {
      std::vector<std::shared_ptr<experimental::PrefixState::DurabilityJob>> jobs;
      {
        std::unique_lock<std::mutex> lock(s.durable_mutex);
        s.durable_cv.wait(lock,[&]{return s.durable_stop||!s.durable_jobs.empty();});
        if(!s.durable_stop && s.async_wait_us) {
          s.durable_cv.wait_for(lock,std::chrono::microseconds(s.async_wait_us));
        }
        jobs.assign(s.durable_jobs.begin(),s.durable_jobs.end());
        s.durable_jobs.clear();
        if (jobs.empty() && s.durable_stop) return;
      }
      Status status=Status::Ok;
      std::map<std::string,uint64_t> file_frontier;
      uint64_t max_serial=0;
      for(const auto& job:jobs)
      {
        max_serial=std::max(max_serial,job->serial);
        for(const auto& [path,generation]:job->files)
          file_frontier[path]=std::max(file_frontier[path],generation);
      }
      try {
        const char* minimum=std::getenv("F2_SYNC_FAULT_MIN_SERIAL");
        const bool inject=minimum && max_serial>=std::stoull(minimum);
        if(inject)experimental::Fault("before-data-sync");
        PrefixSyncDataFiles(file_frontier);
        if(inject)experimental::Fault("after-data-sync");
      } catch (const std::exception& error) {
        std::fprintf(stderr,"F2_ASYNC_WRITER_FAIL pending_jobs=%zu reason=%s\n",jobs.size(),error.what());
        status=Status::IOError;
      } catch (...) {
        std::fprintf(stderr,"F2_ASYNC_WRITER_FAIL pending_jobs=%zu reason=unknown\n",jobs.size());
        status=Status::IOError;
      }
      if(status!=Status::Ok) {
        std::fflush(stderr);
        s.durable_error=true;
        s.poisoned.store(true,std::memory_order_release);
      } else {
        uint64_t durable_end=s.prefix.stable_tail;
        uint64_t durable_serial=s.durable_serial.load(std::memory_order_relaxed);
        for(const auto& job:jobs) {
          durable_end=std::max(durable_end,job->end);
          durable_serial=std::max(durable_serial,job->serial);
        }
        s.prefix.stable_tail=durable_end;
        s.durable_serial.store(durable_serial,std::memory_order_release);
      }
      for(const auto& job:jobs) {
        std::lock_guard<std::mutex> job_lock(job->mutex);
        job->status=status;job->done=true;job->cv.notify_all();
      }
      if(status!=Status::Ok) {
        std::unique_lock<std::mutex> lock(s.durable_mutex);
        for(const auto& pending:s.durable_jobs) {
          std::lock_guard<std::mutex> job_lock(pending->mutex);
          pending->status=status;pending->done=true;pending->cv.notify_all();
        }
        s.durable_jobs.clear();
        return;
      }
    }
  }
  std::shared_ptr<experimental::PrefixState::DurabilityJob> PrefixEnqueueDurable(
      uint64_t end,uint64_t serial,std::map<std::string,uint64_t> files) {
    auto& s=*sync_state_;
    auto job=std::make_shared<experimental::PrefixState::DurabilityJob>();
    job->end=end;job->serial=serial;
    job->files=std::move(files);
    {
      std::lock_guard<std::mutex> lock(s.durable_mutex);
      if(s.durable_error||s.poisoned.load(std::memory_order_acquire))
        throw std::runtime_error("F2 durable writer is poisoned");
      s.durable_jobs.push_back(job);
    }
    s.durable_cv.notify_one();
    return job;
  }
  void PrefixWaitDurable(const std::shared_ptr<experimental::PrefixState::DurabilityJob>& job) {
    std::unique_lock<std::mutex> lock(job->mutex);
    job->cv.wait(lock,[&]{return job->done;});
    if(job->status!=Status::Ok) throw std::runtime_error("F2 durable writer failed");
  }
  void PrefixStartDurableWriter() {
    auto& s=*sync_state_;
    s.durable_stop=false;s.durable_error=false;
    s.durable_thread=std::thread([this]{PrefixDurableLoop();});
  }
  void PrefixStopDurableWriter() {
    auto& s=*sync_state_;
    if(!s.durable_thread.joinable()) return;
    {
      std::lock_guard<std::mutex> lock(s.durable_mutex);
      s.durable_stop=true;
    }
    s.durable_cv.notify_one();
    s.durable_thread.join();
  }
#endif
  void SyncConfigure(const std::string& directory,const std::string& format,bool journal,bool prewritten,uint64_t cutoff,
                     std::function<void()> touch,uint64_t reclaim_bytes) {
    sync_state_=std::make_unique<experimental::SyncState>();
    auto& s=*sync_state_;s.root=directory;s.format=format;s.journal=journal;s.serial=s.reserved_serial=s.cutoff=cutoff;
    s.applied_serial.store(cutoff,std::memory_order_relaxed);
    s.durable_serial.store(cutoff,std::memory_order_relaxed);
    s.touch=std::move(touch);s.reclaim_bytes=reclaim_bytes;
    if(journal) {
      s.log=std::make_unique<experimental::BatchLog>(s.root,0,prewritten);
      experimental::SyncDirectory(s.root);
    }
    // Architecture-experiment knobs — read once at open time so hot paths avoid getenv.
    auto envbool=[](const char* n){const char* v=std::getenv(n);return v&&v[0]=='1'&&!v[1];};
    s.opt_adaptive=envbool("F2_GC_ADAPTIVE");
    s.opt_pipeline=envbool("F2_GC_PIPELINE");
    s.opt_lockfree=envbool("F2_GC_LOCKFREE");
#ifdef F2_LINUX_DATA_PREFIX
    s.opt_batch_notify=envbool("F2_GC_BATCH_NOTIFY");
    s.opt_batch_takeover=envbool("F2_GC_BATCH_TAKEOVER");
    s.opt_batch_profile=envbool("F2_GC_BATCH_PROFILE");
    s.opt_async_writer=envbool("F2_PREFIX_ASYNC_WRITER");
    if(s.opt_batch_notify && (!s.opt_adaptive || s.opt_async_writer || s.opt_pipeline || s.opt_lockfree))
      throw std::runtime_error("batch completion requires synchronous adaptive queue collection");
    if(s.opt_batch_takeover && !s.opt_batch_notify)
      throw std::runtime_error("direct batch takeover requires batch completion");
    if (s.opt_lockfree || (s.opt_pipeline && !s.opt_async_writer))
      throw std::runtime_error("Linux data-log pipeline requires the durable writer");
    if(const char* async_wait=std::getenv("F2_PREFIX_ASYNC_WAIT_US"))
      s.async_wait_us=std::stoull(async_wait);
    s.prefix.prewrite = envbool("F2_PREFIX_PREWRITE");
    using Readers = experimental::PrefixState::Readers;
    if (const char* overlap = std::getenv("F2_PREFIX_READ_OVERLAP")) {
      if (std::strcmp(overlap, "1") == 0) s.prefix.readers = Readers::DuringSync;
      if (std::strcmp(overlap, "2") == 0) s.prefix.readers = Readers::UntilPublish;
    }
#endif
    const char* atomic_readers=std::getenv("F2_GATE_ATOMIC_READERS");
    s.opt_atomic_readers=!(atomic_readers && atomic_readers[0]=='0' && !atomic_readers[1]);
    s.gate.set_atomic_readers(s.opt_atomic_readers);
    s.profile_reads=envbool("F2_SYNC_READ_PROFILE");
    s.reuse_read_guid=envbool("F2_READ_REUSE_GUID");
    s.reuse_write_guid=envbool("F2_WRITE_REUSE_GUID");
    if(const char* wait_us=std::getenv("F2_WAL_GROUP_WAIT_US"))
      s.group_wait_us=std::stoull(wait_us);
    if(const char* b=std::getenv("F2_GC_MIN_BATCH")) s.opt_min_batch=std::stoull(b);
    else s.opt_min_batch=4;
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
    s.reserved_serial=s.serial;
    s.applied_serial.store(s.serial,std::memory_order_release);
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
#if defined(F2_DATA_PREFIX) && !defined(F2_LINUX_DATA_PREFIX)
  template<class Native>
  Status SyncMutate(uint32_t op,const std::string& key,const std::string& value,Native native) {
    auto& s=*sync_state_;
    std::unique_lock<std::mutex> request(s.mutex);
    if(s.poisoned)return Status::IOError;
    const uint64_t serial=s.serial+1;
    bool session=false;
    std::shared_lock<experimental::ReadWriteGate> gate(s.gate,std::defer_lock);
    std::unique_lock<experimental::ReadWriteGate> checkpoint_gate(s.gate,std::defer_lock);
    const char* minimum=std::getenv("F2_SYNC_FAULT_MIN_SERIAL");
    const bool inject=minimum && serial>=std::stoull(minimum);
    try {
      if(s.log){s.log->Stage(op,key,value,serial);s.log->Commit();}
      gate.lock();
      StartWriteSession();session=true;
      if(std::getenv("F2_SYNC_FAIL_APPLY"))throw std::runtime_error("injected F2 apply failure");
      hot_store.PrefixBeginIndex();
      const auto status=native(serial);
      if(status!=Status::Ok)throw std::runtime_error("F2 single-request apply failed");
      s.serial=serial;
      s.reserved_serial=serial;
      s.applied_serial.store(serial,std::memory_order_release);
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
#ifdef F2_LINUX_DATA_PREFIX
  bool SyncCollectBatch(experimental::SyncRequest& own,
                        std::vector<experimental::SyncRequest*>& group,
                        std::shared_ptr<experimental::BatchCompletion>& batch,
                        std::unique_lock<std::mutex>& lock) {
    auto& s=*sync_state_;
    const bool first=s.pending.empty();
    if(first) {
      s.collecting_batch=std::make_shared<experimental::BatchCompletion>();
      if(s.opt_batch_profile) {
        s.batch_first_at=std::chrono::steady_clock::now();
        s.batch_full_at={};
        if(s.batch_completed_at.time_since_epoch().count()!=0 && s.batch_first_at>s.batch_completed_at)
          s.batch_gap_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(s.batch_first_at-s.batch_completed_at).count();
      }
    }
    batch=s.collecting_batch;
    s.pending.push_back(&own);
    if(s.opt_batch_profile && s.pending.size()==s.opt_min_batch)
      s.batch_full_at=std::chrono::steady_clock::now();

    // Collection starts only after the preceding batch has finished publishing.
    // Under the same queue lock, either the timeout collector or the last
    // arriving caller claims the batch, so only one of them executes it.
    if(!first) {
      if(s.pending.size()==s.opt_min_batch)s.collect_cv.notify_one();
      if(!s.opt_batch_takeover || !s.collecting || s.pending.size()<s.opt_min_batch)
        return false;
      ++s.batch_direct_owners;
    } else {
      if(auto previous=s.active_batch) {
        lock.unlock();
        const Status status=previous->Wait();
        lock.lock();
        if(status!=Status::Ok)return false;
      }
      s.collecting=true;
      const auto deadline=std::chrono::steady_clock::now()+std::chrono::microseconds(s.group_wait_us);
      while(s.collecting_batch==batch && s.pending.size()<s.opt_min_batch) {
        if(s.collect_cv.wait_until(lock,deadline)==std::cv_status::timeout)break;
      }
      if(s.collecting_batch!=batch)return false;
      ++s.batch_fallback_owners;
    }

    if(s.opt_batch_profile) {
      const auto drain=std::chrono::steady_clock::now();
      s.batch_collect_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(drain-s.batch_first_at).count();
      if(s.batch_full_at.time_since_epoch().count()!=0)
        s.batch_ready_delay_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(drain-s.batch_full_at).count();
    }
    group.assign(s.pending.begin(),s.pending.end());
    s.pending.clear();
    s.active_batch=batch;
    s.collecting_batch.reset();
    s.collecting=false;
    s.collect_cv.notify_one();
    return true;
  }
#endif
  template<class Native>
  Status SyncMutate(uint32_t op,const std::string& key,const std::string& value,Native native) {
    auto& s=*sync_state_;
    experimental::SyncRequest own{op,key,value,native};
    std::vector<experimental::SyncRequest*> group;
    bool batch_owner=false;
#ifdef F2_LINUX_DATA_PREFIX
    std::shared_ptr<experimental::BatchCompletion> batch;
#endif
    if(s.opt_lockfree) {
      own.lf_next=s.lf_stack.load(std::memory_order_relaxed);
      while(!s.lf_stack.compare_exchange_weak(own.lf_next,&own,
              std::memory_order_release,std::memory_order_relaxed)) {}
      s.lf_count.fetch_add(1,std::memory_order_release);
    }

    std::unique_lock<std::mutex> lock(s.mutex);
    if(s.poisoned.load(std::memory_order_relaxed))return Status::IOError;
#ifdef F2_LINUX_DATA_PREFIX
    if(s.opt_batch_notify) {
      batch_owner=SyncCollectBatch(own,group,batch,lock);
      if(!batch_owner) {
        lock.unlock();
        return batch->Wait();
      }
    } else
#endif
    if(!s.opt_lockfree) {
      s.pending.push_back(&own);
      if(s.opt_adaptive && s.pending.size()==s.opt_min_batch)s.collect_cv.notify_one();
    } else if(s.opt_adaptive && s.lf_count.load(std::memory_order_acquire)==s.opt_min_batch) {
      s.collect_cv.notify_one();
    }

    while(!own.done) {
      if(!batch_owner) {
        if(own.in_group) {
          s.cv.wait(lock,[&]{return own.done;});
          continue;
        }
        if(s.leading.load(std::memory_order_relaxed)) {
          s.cv.wait(lock,[&]{return own.done||!s.leading.load(std::memory_order_relaxed);});
          continue;
        }
        s.leading.store(true,std::memory_order_relaxed);
        const auto wait=std::chrono::microseconds(s.group_wait_us);
        if(s.opt_adaptive) {
          const auto deadline=std::chrono::steady_clock::now()+wait;
          while(true) {
            const size_t depth=s.opt_lockfree?s.lf_count.load(std::memory_order_acquire):s.pending.size();
            if(depth>=s.opt_min_batch)break;
            if(s.collect_cv.wait_until(lock,deadline)==std::cv_status::timeout)break;
          }
        } else {
          s.cv.wait_for(lock,wait);
        }
        group.clear();
        if(s.opt_lockfree) {
          experimental::SyncRequest* head=s.lf_stack.exchange(nullptr,std::memory_order_acquire);
          s.lf_count.store(0,std::memory_order_relaxed);
          while(head){group.push_back(head);head=head->lf_next;}
          std::reverse(group.begin(),group.end());
        } else {
          group.assign(s.pending.begin(),s.pending.end());
          s.pending.clear();
        }
        if(group.empty()) {
          s.leading.store(false,std::memory_order_relaxed);
          s.cv.notify_all();
          continue;
        }
        for(auto* request:group)request->in_group=true;
      }
      batch_owner=false;

#ifdef F2_LINUX_DATA_PREFIX
      if(s.opt_pipeline && s.opt_async_writer) {
        // Serialize F2's mutable HybridLog and deferred-index preparation up to
        // enqueue. The next group may apply while this group's file sync runs.
        s.leading.store(false,std::memory_order_relaxed);
        s.cv.notify_all();
        lock.unlock();

        bool session=false;
        bool failed=false;
        std::unique_lock<std::mutex> stage(s.stage_mutex);
        std::shared_ptr<experimental::PrefixState::DurabilityJob> job;
        try {
          if(s.poisoned.load(std::memory_order_acquire))
            throw std::runtime_error("F2 pipeline commit poisoned");
          const uint64_t first=s.serial+1;
          uint64_t next=s.serial;
          const char* minimum=std::getenv("F2_SYNC_FAULT_MIN_SERIAL");
          const bool inject=minimum && s.serial+group.size()>=std::stoull(minimum);
          std::unique_lock<experimental::ReadWriteGate> gate(s.gate);
          StartWriteSession();session=true;
          if(std::getenv("F2_SYNC_FAIL_APPLY"))
            throw std::runtime_error("injected F2 apply failure");
          hot_store.PrefixBeginIndex();
          for(auto* r:group) {
            r->status=r->apply(++next);
            if(r->status!=Status::Ok)
              throw std::runtime_error("F2 group apply failed");
          }
          s.serial=next;
          s.reserved_serial=next;
          s.applied_serial.store(next,std::memory_order_release);
          auto files=PrefixCommit(first,next,group.size(),&gate,false,true);
          if(inject) experimental::Fault("after-wal");
          const uint64_t end=s.prefix.prepared_tail;
          StopSession();session=false;
          job=PrefixEnqueueDurable(end,next,std::move(files));
          s.prefix.last_mutation_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
          stage.unlock();

          PrefixWaitDurable(job);
          {
            std::unique_lock<std::mutex> checkpoint_stage(s.stage_mutex);
            if(s.durable_serial.load(std::memory_order_acquire)==s.serial &&
               s.prefix.stable_tail-s.prefix.base_tail>=s.reclaim_bytes) {
              std::unique_lock<experimental::ReadWriteGate> checkpoint_gate(s.gate);
              StartSession();session=true;
              SyncCheckpointAndReclaim();
              StopSession();session=false;
            }
          }
          if(inject) experimental::Fault("before-ack");
          ++s.groups;s.operations+=group.size();
        } catch(...) {
          failed=true;
          s.poisoned.store(true,std::memory_order_release);
          if(stage.owns_lock())stage.unlock();
          hot_store.PrefixAbortIndex();
          if(session){try{CompletePending(true);StopSession();}catch(...){}}
        }
        lock.lock();
        if(failed) {
          s.poisoned.store(true,std::memory_order_release);
          for(auto* r:s.pending){r->status=Status::IOError;r->done=true;}
          s.pending.clear();
        }
        for(auto* r:group){r->status=failed?Status::IOError:r->status;r->done=true;}
        s.cv.notify_all();
        continue;
      }
      if(s.opt_async_writer) {
        s.leading.store(false,std::memory_order_relaxed);
        s.cv.notify_all();
        lock.unlock();
        bool session=false; bool failed=false;
        try {
          std::shared_ptr<experimental::PrefixState::DurabilityJob> job;
          uint64_t next=s.serial;
          const char* minimum=std::getenv("F2_SYNC_FAULT_MIN_SERIAL");
          const bool inject=minimum && s.serial+group.size()>=std::stoull(minimum);
          std::unique_lock<experimental::ReadWriteGate> gate(s.gate);
          const uint64_t first=s.serial+1;
          next=s.serial;
          StartWriteSession();session=true;
          if(std::getenv("F2_SYNC_FAIL_APPLY"))throw std::runtime_error("injected F2 apply failure");
          hot_store.PrefixBeginIndex();
          for(auto* r:group) {
            r->status=r->apply(++next);
            if(r->status!=Status::Ok)throw std::runtime_error("F2 group apply failed");
          }
          s.serial=next;s.reserved_serial=next;
          s.applied_serial.store(next,std::memory_order_release);
          auto file_frontier=PrefixCommit(first,next,group.size(),&gate,false,true);
          if(inject) experimental::Fault("after-wal");
          const uint64_t end=s.prefix.prepared_tail;
          StopSession();session=false;
          job=PrefixEnqueueDurable(end,next,std::move(file_frontier));
          PrefixWaitDurable(job);
          if(s.prefix.stable_tail-s.prefix.base_tail>=s.reclaim_bytes) {
            std::unique_lock<experimental::ReadWriteGate> checkpoint_gate(s.gate);
            StartSession();
            try { SyncCheckpointAndReclaim(); StopSession(); }
            catch(...) { StopSession(); throw; }
          }
          if(inject) experimental::Fault("before-ack");
          ++s.groups;s.operations+=group.size();
        } catch(...) {
          failed=true;
          try { throw; } catch(const std::exception& e) {
            std::fprintf(stderr,"F2_ASYNC_REQUEST_FAIL %s\n",e.what());
          } catch(...) { std::fprintf(stderr,"F2_ASYNC_REQUEST_FAIL unknown\n"); }
          hot_store.PrefixAbortIndex();
          if(session){try{CompletePending(true);StopSession();}catch(...){}}
        }
        lock.lock();
        if(failed) {
          s.poisoned.store(true,std::memory_order_relaxed);
          for(auto* r:s.pending){r->status=Status::IOError;r->done=true;}
          s.pending.clear();
        }
        for(auto* r:group){r->status=failed?Status::IOError:r->status;r->done=true;}
        s.cv.notify_all();
        continue;
      }
#endif
      if(s.opt_pipeline) {
        // Release leadership immediately so the next group starts collecting
        // while this one does WAL (stage+sync) and apply.
        // stage_mutex serialises the entire WAL+serial+apply sequence across
        // concurrent leaders so serial order is preserved and s.serial is only
        // advanced once per group under exclusive ownership.
        s.leading.store(false,std::memory_order_relaxed);
        s.cv.notify_all();
        lock.unlock();

        bool session=false; bool failed=false;
        try {
          uint64_t first=0, last=0;
          {
            std::unique_lock<std::mutex> stage(s.stage_mutex);
            first=s.reserved_serial+1;
            last=s.reserved_serial;
            if(s.log) {
              for(auto* r:group) s.log->Stage(r->op,r->key,r->value,++last);
              s.log->Commit();
            } else {
              last+=group.size();
            }
            s.reserved_serial=last;
          }
          const char* minimum=std::getenv("F2_SYNC_FAULT_MIN_SERIAL");
          const bool inject=minimum&&last>=std::stoull(minimum);
          if(inject) experimental::Fault("after-wal");
          {
            std::unique_lock<std::mutex> order(s.mutex);
            s.cv.wait(order,[&]{return s.poisoned.load(std::memory_order_acquire) ||
                                      s.applied_serial.load(std::memory_order_acquire)==first-1;});
          }
          if(s.poisoned.load(std::memory_order_acquire))throw std::runtime_error("F2 pipeline commit poisoned");
          std::unique_lock<experimental::ReadWriteGate> gate(s.gate);
          StartSession(); session=true;
          if(std::getenv("F2_SYNC_FAIL_APPLY"))throw std::runtime_error("injected F2 apply failure");
          uint64_t next=s.serial;
          if(next+1!=first)throw std::runtime_error("F2 pipeline serial reservation mismatch");
          for(auto* r:group){
            r->status=r->apply(++next);
            if(r->status!=Status::Ok)throw std::runtime_error("F2 group apply failed");
          }
          s.serial=next;
          s.applied_serial.store(next,std::memory_order_release);
          if(!s.log||s.log->Bytes()>=s.reclaim_bytes) {
            std::unique_lock<std::mutex> checkpoint_stage(s.stage_mutex);
            if(s.reserved_serial==s.serial && (!s.log||s.log->Bytes()>=s.reclaim_bytes))
              SyncCheckpointAndReclaim();
          }
          if(inject) experimental::Fault("before-ack");
          CompletePending(true); StopSession(); session=false;
          ++s.groups; s.operations+=group.size();
        } catch(...) {
          failed=true;
          if(session){try{CompletePending(true);StopSession();}catch(...){}}
        }
        lock.lock();
        if(failed) {
          s.poisoned.store(true,std::memory_order_relaxed);
          for(auto* r:s.pending){r->status=Status::IOError;r->done=true;}
          s.pending.clear();
        }
        for(auto* r:group){r->status=failed?Status::IOError:r->status;r->done=true;}
        s.cv.notify_all();
        continue; // re-check own.done
      }

      // Baseline / adaptive: hold leading across WAL+apply.
      lock.unlock();
      bool session=false; bool failed=false;
      try {
        uint64_t next=s.serial;
#ifndef F2_LINUX_DATA_PREFIX
        if(s.log) {
          for(auto* r:group) s.log->Stage(r->op,r->key,r->value,++next);
          s.log->Commit();
        }
#else
        const uint64_t first = s.serial + 1;
#endif
        const char* minimum = std::getenv("F2_SYNC_FAULT_MIN_SERIAL");
        const bool inject = minimum && s.serial + group.size() >= std::stoull(minimum);
#ifndef F2_LINUX_DATA_PREFIX
        if(inject) experimental::Fault("after-wal");
#endif
#ifdef F2_LINUX_DATA_PREFIX
        using Readers = experimental::PrefixState::Readers;
        std::unique_lock<experimental::ReadWriteGate> gate(s.gate,std::defer_lock);
        std::shared_lock<experimental::ReadWriteGate> staging(s.gate,std::defer_lock);
        if (s.prefix.readers == Readers::UntilPublish) staging.lock();
        else gate.lock();
        const auto apply_start = std::chrono::steady_clock::now();
#else
        std::unique_lock<experimental::ReadWriteGate> gate(s.gate);
#endif
        const auto session_start = std::chrono::steady_clock::now();
        StartWriteSession(); session=true;
#ifdef F2_LINUX_DATA_PREFIX
        s.prefix.session_start_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-session_start).count();
#endif
        if(std::getenv("F2_SYNC_FAIL_APPLY"))throw std::runtime_error("injected F2 apply failure");
        next=s.serial;
#ifdef F2_LINUX_DATA_PREFIX
        hot_store.PrefixBeginIndex();
        const auto index_apply_start=std::chrono::steady_clock::now();
#endif
        for(auto* r:group){
          r->status=r->apply(++next);
          if(r->status!=Status::Ok)throw std::runtime_error("F2 group apply failed");
        }
        s.serial=next;
        s.reserved_serial=next;
        s.applied_serial.store(next,std::memory_order_release);
#ifdef F2_LINUX_DATA_PREFIX
        s.prefix.index_apply_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-index_apply_start).count();
        s.prefix.apply_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - apply_start).count();
        PrefixCommit(first, next, group.size(), s.prefix.readers == Readers::DuringSync ? &gate : nullptr);
        if(inject) experimental::Fault("after-wal");
        if (staging.owns_lock()) {
          staging.unlock();
          const auto publish_start = std::chrono::steady_clock::now();
          gate.lock();
          s.prefix.publish_wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - publish_start).count();
        }
        const auto publication_start=std::chrono::steady_clock::now();
        hot_store.PrefixPublishIndex();
        if(s.opt_batch_profile)s.batch_publication_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now()-publication_start).count();
        s.prefix.last_mutation_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
        if(s.prefix.stable_tail - s.prefix.base_tail >= s.reclaim_bytes) {
          const auto checkpoint_start=std::chrono::steady_clock::now();
          SyncCheckpointAndReclaim();
          if(s.opt_batch_profile)s.batch_checkpoint_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-checkpoint_start).count();
        }
#else
        if(!s.log||s.log->Bytes()>=s.reclaim_bytes) SyncCheckpointAndReclaim();
#endif
        if(inject) experimental::Fault("before-ack");
        const auto session_complete_start=std::chrono::steady_clock::now();
        CompletePending(true);
#ifdef F2_LINUX_DATA_PREFIX
        s.prefix.session_complete_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-session_complete_start).count();
#endif
        const auto session_stop_start=std::chrono::steady_clock::now();
        StopSession(); session=false;
#ifdef F2_LINUX_DATA_PREFIX
        s.prefix.session_stop_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-session_stop_start).count();
#endif
        ++s.groups; s.operations+=group.size();
      } catch(...) {
        failed=true;
#ifdef F2_LINUX_DATA_PREFIX
        hot_store.PrefixAbortIndex();
#endif
        if(session){try{CompletePending(true);StopSession();}catch(...){}}
      }
#ifdef F2_LINUX_DATA_PREFIX
      if(batch) {
        const Status result=failed?Status::IOError:own.status;
        std::shared_ptr<experimental::BatchCompletion> pending_batch;
        if(failed) {
          lock.lock();
          s.poisoned.store(true,std::memory_order_release);
          s.pending.clear();
          pending_batch=std::move(s.collecting_batch);
          s.collecting=false;
          s.collect_cv.notify_all();
          lock.unlock();
        }
        if(s.opt_batch_profile) {
          std::lock_guard<std::mutex> profile_lock(s.mutex);
          s.batch_completed_at=std::chrono::steady_clock::now();
        }
        if(pending_batch)pending_batch->Complete(true);
        batch->Complete(failed);
        return result;
      }
#endif
      lock.lock();
      if(failed) {
        s.poisoned.store(true,std::memory_order_relaxed);
        for(auto* r:s.pending){r->status=Status::IOError;r->done=true;}
        s.pending.clear();
      }
      for(auto* r:group){r->status=failed?Status::IOError:r->status;r->done=true;}
      s.leading.store(false,std::memory_order_relaxed);
      s.cv.notify_all();
    }
    return own.status;
  }
#endif
  template<class Native>
  Status SyncRead(Native native,bool sample=false) {
#if defined(__APPLE__) && defined(F2_NATIVE_IO_PROFILE)
    FASTER::environment::NativeIoReadScope io_read_scope;
#endif
    auto& s=*sync_state_;
    std::chrono::steady_clock::time_point start;
    if(sample)start=std::chrono::steady_clock::now();
    std::shared_lock<experimental::ReadWriteGate> gate(s.gate);
    if(sample){auto now=std::chrono::steady_clock::now();s.profile_gate_wait_ns.fetch_add(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now-start).count(),std::memory_order_relaxed);start=now;}
#ifdef F2_DURABLE_COMPACTION
    if(prefix_gc_copying_.load())++prefix_gc_reads_;
#endif
    if(s.poisoned) return Status::IOError;
    std::chrono::steady_clock::time_point session_start;
    if(sample)session_start=std::chrono::steady_clock::now();
    if(s.reuse_read_guid) {
      static thread_local Guid read_session_guid=Guid::Create();
      StartSession(read_session_guid);
    } else {
      StartSession();
    }
    if(sample){auto now=std::chrono::steady_clock::now();s.profile_session_start_ns.fetch_add(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now-session_start).count(),std::memory_order_relaxed);start=now;}
    try {
      auto result=native();
      if(sample){auto now=std::chrono::steady_clock::now();s.profile_read_await_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now-start).count(),std::memory_order_relaxed);start=now;}
      CompletePending(true);
      if(sample){auto now=std::chrono::steady_clock::now();s.profile_complete_pending_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now-start).count(),std::memory_order_relaxed);start=now;}
      StopSession();
      if(sample){auto now=std::chrono::steady_clock::now();s.profile_session_stop_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now-start).count(),std::memory_order_relaxed);
        s.profile_read_samples.fetch_add(1,std::memory_order_relaxed);}
      return result;
    } catch(...) {
      CompletePending(true);StopSession();
      if(sample)s.profile_read_samples.fetch_add(1,std::memory_order_relaxed);
      throw;
    }
  }
  uint64_t SyncSerial() const {return sync_state_->serial;}
  uint64_t SyncLogCommits() const {return sync_state_->groups;}
  uint64_t SyncFullCheckpoints() const {return sync_state_->full_checkpoints;}
#ifdef F2_LINUX_DATA_PREFIX
  void SyncPrintBatchProfile() const {
    const auto& s=*sync_state_;
    if(!s.opt_batch_profile)return;
    std::fprintf(stderr,"F2_BATCH_PROFILE collect_ns=%llu ready_delay_ns=%llu gap_ns=%llu checkpoint_ns=%llu publication_ns=%llu\n",
      (unsigned long long)s.batch_collect_ns,(unsigned long long)s.batch_ready_delay_ns,
      (unsigned long long)s.batch_gap_ns,(unsigned long long)s.batch_checkpoint_ns,
      (unsigned long long)s.batch_publication_ns);
  }
  bool SyncBatchNotifyEnabled() const {return sync_state_->opt_batch_notify;}
  bool SyncBatchTakeoverEnabled() const {return sync_state_->opt_batch_takeover;}
  uint64_t SyncBatchDirectOwners() const {return sync_state_->batch_direct_owners;}
  uint64_t SyncBatchFallbackOwners() const {return sync_state_->batch_fallback_owners;}
#endif
  bool SyncReadProfileEnabled() const {return sync_state_->profile_reads;}
  bool SyncReadReuseGuidEnabled() const {return sync_state_->reuse_read_guid;}
  bool SyncWriteReuseGuidEnabled() const {return sync_state_->reuse_write_guid;}
  void StartWriteSession() {
    if(sync_state_->reuse_write_guid) {
      static thread_local Guid session_guid=Guid::Create();
      StartSession(session_guid);
    } else {
      StartSession();
    }
  }
  uint64_t SyncReadProfileSamples() const {return sync_state_->profile_read_samples.load();}
  uint64_t SyncReadProfileGateWaitNs() const {return sync_state_->profile_gate_wait_ns.load();}
  uint64_t SyncReadProfileSessionStartNs() const {return sync_state_->profile_session_start_ns.load();}
  uint64_t SyncReadProfileReadAwaitNs() const {return sync_state_->profile_read_await_ns.load();}
  uint64_t SyncReadProfileCompletePendingNs() const {return sync_state_->profile_complete_pending_ns.load();}
  uint64_t SyncReadProfileSessionStopNs() const {return sync_state_->profile_session_stop_ns.load();}
  bool SyncReadProfileNextSample() {
    if(!sync_state_->profile_reads)return false;
    static thread_local uint64_t profile_tick=0;
    return (profile_tick++&63)==0;
  }
  void SyncReadProfileAddGetTotal(uint64_t ns) {
    sync_state_->profile_get_total_ns.fetch_add(ns,std::memory_order_relaxed);
  }
  uint64_t SyncReadProfileGetTotalNs() const {return sync_state_->profile_get_total_ns.load();}
  void SyncClose() {
#ifdef F2_DURABLE_COMPACTION
    PrefixGcStop();
#endif
#ifdef F2_DATA_PREFIX
    // Each acknowledged data-log operation is already recoverable. Closing
    // must not turn single-request commits into per-close checkpoints.
#ifdef F2_LINUX_DATA_PREFIX
    PrefixStopDurableWriter();
#endif
    return;
#else
    auto& s=*sync_state_;std::unique_lock<experimental::ReadWriteGate> gate(s.gate);
    if(!s.poisoned && s.serial!=s.cutoff) {
      StartSession();try {SyncCheckpointAndReclaim();StopSession();}
      catch(...) {StopSession();throw;}
    }
#endif
  }
 private:
  std::unique_ptr<experimental::SyncState> sync_state_;
