// Single OS-thread owner, one session, one mutation group in flight. Reads use
// the committed hash heads while the writer's unpublished heads remain private.
 public:
  void FiberOwnerBegin() { StartSession(); }
  void FiberOwnerEnd() { CompletePending(true); StopSession(); }
  void FiberOwnerPoll() { CompletePending(false); }
  bool FiberOwnerNeedsCheckpoint() const {
    const auto& s=*sync_state_;
    return !s.poisoned && s.prefix.stable_tail-s.prefix.base_tail>=s.reclaim_bytes;
  }
  bool FiberOwnerFailed() const { return sync_state_->poisoned; }
  uint64_t FiberOwnerReadSerial() const {
    return std::max<uint64_t>(1,std::max(sync_state_->serial,
      std::max(hot_store.thread_ctx().serial_num,cold_store.thread_ctx().serial_num)));
  }
  void FiberOwnerCheckpoint() {
    // The scheduler first drains all reads and blocks admission to the F2
    // session. No OS mutex is held across fiber suspension or checkpoint I/O.
    SyncCheckpointAndReclaim();
  }
  rocksdb::Status FiberOwnerApply(rocksdb::WriteBatch& batch) {
    auto& s=*sync_state_;
    if(s.poisoned)return rocksdb::Status::IOError("fiber F2 owner failed");
    const auto first=s.serial+1;
    try {
      hot_store.PrefixBeginIndex();
      struct Handler final:rocksdb::WriteBatch::Handler {
        experimental::SyncState& state;
        uint64_t next;
        Handler(experimental::SyncState& s,uint64_t sequence):state(s),next(sequence){}
        rocksdb::Status PutCF(uint32_t cf,const rocksdb::Slice& k,const rocksdb::Slice& v) override {
          return Apply(cf,1,k,v);
        }
        rocksdb::Status DeleteCF(uint32_t cf,const rocksdb::Slice& k) override { return Apply(cf,2,k,{}); }
        rocksdb::Status Apply(uint32_t cf,uint32_t op,const rocksdb::Slice& k,const rocksdb::Slice& v) {
          if(cf)return rocksdb::Status::Corruption("unexpected column family");
          if(std::getenv("F2_SYNC_FAIL_APPLY"))return rocksdb::Status::IOError("injected apply error");
          const auto result=state.rocks_apply(op,k.ToString(),v.ToString(),next++);
          return result==Status::Ok?rocksdb::Status::OK():rocksdb::Status::IOError("F2 apply failed");
        }
      } handler(s,first);
      reference::Check(batch.Iterate(&handler));s.serial=handler.next-1;
      PrefixCommit(first,s.serial,batch.Count());
      const char* minimum=std::getenv("F2_SYNC_FAULT_MIN_SERIAL");
      const bool inject=minimum && s.serial>=std::stoull(minimum);
      if(inject)experimental::Fault("after-wal");
      hot_store.PrefixPublishIndex(); // No fiber switch inside publication.
      ++s.groups;s.operations+=batch.Count();++s.log_commits;
      if(inject)experimental::Fault("before-ack");
      return rocksdb::Status::OK();
    } catch(const std::exception& e) {
      s.poisoned=true;hot_store.PrefixAbortIndex();
      return rocksdb::Status::IOError(e.what());
    }
  }
