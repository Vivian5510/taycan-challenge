// Included in F2Kv. RocksDB supplies the queue and complete WAL codec; F2 owns
// application, checkpoint coverage and WAL generations.
 public:
  void SyncPrintRocksCounters() const {
    const auto& c=sync_state_->rocks_stats;
    std::fprintf(stderr,"F2_ROCKS_WAL groups=%llu file_syncs=%llu directory_syncs=%llu append_calls=%llu append_bytes=%llu\n",
      (unsigned long long)sync_state_->rocks->groups(),(unsigned long long)c.file_syncs.load(),
      (unsigned long long)c.directory_syncs.load(),(unsigned long long)c.append_calls.load(),
      (unsigned long long)c.append_bytes.load());
    if constexpr(reference::kProfileWal) {
    std::fprintf(stderr,"F2_WAL_TIMING queue_wait_ns=%llu merge_ns=%llu append_ns=%llu sync_ns=%llu apply_ns=%llu leader_ns=%llu requests=%llu operations=%llu\n",
      (unsigned long long)c.queue_wait_ns.load(),(unsigned long long)c.merge_ns.load(),
      (unsigned long long)c.append_ns.load(),(unsigned long long)c.sync_ns.load(),
      (unsigned long long)c.apply_ns.load(),(unsigned long long)c.leader_ns.load(),
      (unsigned long long)c.grouped_requests.load(),(unsigned long long)c.grouped_operations.load());
    std::fprintf(stderr,"F2_WAL_GROUP_HIST");
    for(size_t i=0;i<c.group_histogram.size();++i)
      std::fprintf(stderr," le%llu=%llu",(unsigned long long)(1ULL<<i),(unsigned long long)c.group_histogram[i].load());
    std::fprintf(stderr,"\n");
    std::fprintf(stderr,"F2_WAL_COLLECT ns=%llu\n",(unsigned long long)c.collect_ns.load());
    }
  }
  void SyncSetApply(std::function<Status(uint32_t,const std::string&,const std::string&,uint64_t)> apply) {
    sync_state_->rocks_apply=std::move(apply);
  }
  Status SyncRocksMutate(uint32_t op,const std::string& key,const std::string& value) {
#ifdef F2_PREFIX_PIPELINE
    return PrefixPipelineMutate(op,key,value);
#else
    auto& s=*sync_state_;
    if(s.poisoned)return Status::IOError;
    rocksdb::WriteBatch batch;
    auto status=op==1?batch.Put(key,value):batch.Delete(key);
    if(status.ok())status=s.rocks->Write(batch);
    if(!status.ok()) {s.poisoned=true;return Status::IOError;}
    return Status::Ok;
#endif
  }
  rocksdb::Status SyncApplyRocksBatch(rocksdb::WriteBatch& batch,uint64_t first) {
    auto& s=*sync_state_;
#ifdef F2_DATA_PREFIX
    std::shared_lock<std::shared_mutex> gate(s.gate);
#else
    std::unique_lock<std::shared_mutex> gate(s.gate);
#endif
    if(s.poisoned)return rocksdb::Status::IOError("F2 instance failed");
    if(first!=s.serial+1)return rocksdb::Status::Corruption("F2/WAL sequence differs");
    bool session=false;
    try {
      const char* min=std::getenv("F2_SYNC_FAULT_MIN_SERIAL");
      const bool inject=min && first+batch.Count()-1>=std::stoull(min);
#ifndef F2_DATA_PREFIX
      if(inject)experimental::Fault("after-wal");
#endif
      StartSession();session=true;
#ifdef F2_DATA_PREFIX
      hot_store.PrefixBeginIndex();
#endif
      struct Handler final:rocksdb::WriteBatch::Handler {
        experimental::SyncState& state;
        uint64_t next;
        Handler(experimental::SyncState& s,uint64_t sequence):state(s),next(sequence){}
        rocksdb::Status PutCF(uint32_t cf,const rocksdb::Slice& k,const rocksdb::Slice& v) override {
          return Apply(cf,1,k,v);
        }
        rocksdb::Status DeleteCF(uint32_t cf,const rocksdb::Slice& k) override {return Apply(cf,2,k,{});}
        rocksdb::Status Apply(uint32_t cf,uint32_t op,const rocksdb::Slice& k,const rocksdb::Slice& v) {
          if(cf)return rocksdb::Status::Corruption("unexpected WAL column family");
          if(std::getenv("F2_SYNC_FAIL_APPLY"))return rocksdb::Status::IOError("injected F2 apply failure");
          auto status=state.rocks_apply(op,k.ToString(),v.ToString(),next++);
          return status==Status::Ok?rocksdb::Status::OK():rocksdb::Status::IOError("F2 apply failed");
        }
      } handler(s,first);
      reference::Check(batch.Iterate(&handler));
      s.serial=handler.next-1;
      CompletePending(true);
#ifdef F2_DATA_PREFIX
      PrefixCommit(first,s.serial,batch.Count());
      if(inject)experimental::Fault("after-wal");
      hot_store.PrefixPublishIndex();
#ifdef F2_DURABLE_COMPACTION
      if(prefix_gc_copying_.load())prefix_gc_writes_+=batch.Count();
#endif
      s.prefix.last_mutation_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
      ++s.groups;s.operations+=batch.Count();++s.log_commits;
#ifdef F2_DATA_PREFIX
      // Do not leave an epoch-protected session blocked on the maintenance
      // gate: checkpoints need every active session to refresh its epoch.
      StopSession();session=false;
      const bool needs_checkpoint=s.prefix.stable_tail-s.prefix.base_tail>=s.reclaim_bytes
#ifdef F2_DURABLE_COMPACTION
        && !prefix_gc_active_.load()
#endif
        ;
      gate.unlock();
      if(needs_checkpoint) {
        std::unique_lock<std::shared_mutex> checkpoint_gate(s.gate);
        if(s.prefix.stable_tail-s.prefix.base_tail>=s.reclaim_bytes) {
          StartSession();session=true;SyncCheckpointAndReclaim();StopSession();session=false;
        }
      }
#else
      if(s.rocks->Bytes()>=s.reclaim_bytes)SyncCheckpointAndReclaim();
#endif
      if(inject)experimental::Fault("before-ack");
      if(session) {StopSession();session=false;}
      return rocksdb::Status::OK();
    } catch(const std::exception& e) {
      s.poisoned=true;
#ifdef F2_DATA_PREFIX
      hot_store.PrefixAbortIndex();
#endif
      if(session) {try{CompletePending(true);StopSession();}catch(...){}}
      return rocksdb::Status::IOError(e.what());
    }
  }
  template<class Apply>
  uint64_t SyncReplayRocks(Apply apply) {
    auto& s=*sync_state_;
    struct Handler final:rocksdb::WriteBatch::Handler {
      Apply& callback;
      explicit Handler(Apply& fn):callback(fn){}
      rocksdb::Status PutCF(uint32_t cf,const rocksdb::Slice& k,const rocksdb::Slice& v) override {
        if(cf)return rocksdb::Status::Corruption("unexpected WAL column family");
        callback(1,k.ToString(),v.ToString());return rocksdb::Status::OK();
      }
      rocksdb::Status DeleteCF(uint32_t cf,const rocksdb::Slice& k) override {
        if(cf)return rocksdb::Status::Corruption("unexpected WAL column family");
        callback(2,k.ToString(),{});return rocksdb::Status::OK();
      }
    } handler(apply);
    const auto last=reference::Recover((s.root/"wal"/(s.base_token+".log")).string(),handler,s.cutoff);
    s.serial=last;s.rocks->SetRecoveredSequence(last);
    return last-s.cutoff;
  }
  void SyncPurgeOldRocksWal(const std::string& current) {
    auto& s=*sync_state_;
    for(const auto& entry:std::filesystem::directory_iterator(s.root/"wal")) {
      if(entry.is_regular_file() && entry.path().extension()==".log" && entry.path().stem()!=current)
        std::filesystem::remove(entry.path());
    }
    experimental::SyncDirectory(s.root/"wal");
  }
