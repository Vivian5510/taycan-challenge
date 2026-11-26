// Included in F2Kv. This coordinator reuses native F2 compaction and index GC;
// it does not implement a second data store or journal.
 public:
  void PrefixGcConfigure() {
    hot_store.disk.log().PrefixDeferTruncation();
    cold_store.disk.log().PrefixDeferTruncation();
    cold_store.hash_index_.PrefixIndexStore().disk.log().PrefixDeferTruncation();
  }
  void PrefixGcPhase(const char* phase) {
    experimental::GcPhase(phase,prefix_gc_serial_,prefix_gc_cycle_);
  }
  template<class S> uint64_t PrefixGcUntil(const S& store) const {
    const auto tail=std::min(store.hlog.GetTailAddress().control(),
                             store.hlog.safe_read_only_address.control());
    // Keep the tail page. Native compaction scans immutable complete pages.
    return tail ? ((tail-1)/store.hlog.kPageSize)*store.hlog.kPageSize : 0;
  }
  template<class S> void PrefixGcFlush(S& store) {
    const auto tail=store.hlog.ShiftReadOnlyToTail();
    while(store.hlog.flushed_until_address.load()<tail) {
      CompletePending(false);Refresh();std::this_thread::yield();
    }
    CompletePending(true);
  }
  template<class S> void PrefixGcShift(S& store,uint64_t until) {
    if(until<=store.hlog.begin_address.control())return;
    if(!store.ShiftBeginAddress(Address(until),nullptr,nullptr))
      throw std::runtime_error("native GC shift rejected");
    while(store.system_state_.phase()!=Phase::REST) {
      CompletePending(false);Refresh();std::this_thread::yield();
    }
    CompletePending(true);
  }
  void PrefixGcRecord(const char* name,uint64_t begin,uint64_t until,uint64_t tail,bool success) {
    std::ostringstream fields;
    fields << "\"event\":\"native_compaction\",\"store\":" << std::quoted(name)
           << ",\"cycle\":" << prefix_gc_cycle_ << ",\"serial\":" << prefix_gc_serial_
           << ",\"begin\":" << begin << ",\"until\":" << until
           << ",\"tail_after\":" << tail << ",\"success\":" << (success?"true":"false");
    experimental::GcTrace(fields.str());
  }
  template<class Publish> void PrefixGcWithStableWal(Publish publish) {
    auto& s=*sync_state_;
    for(;;) {
      // Match the WAL checkpoint worker's lock order. A group may have a
      // durable WAL frame reserved while its F2 apply is still pending. Keep
      // both locks through logical index reclamation, root publication, and
      // physical retirement so no writer enters F2's non-REST maintenance phase.
      std::unique_lock<std::mutex> apply(s.wal_apply_mutex);
      std::unique_lock<std::mutex> stage(s.stage_mutex);
      if(s.poisoned)throw std::runtime_error("cannot publish compaction root for failed WAL store");
      if(s.reserved_serial==s.serial) {
        publish();
        return;
      }
      stage.unlock();
      apply.unlock();
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
  void PrefixGcRun(uint64_t hot_until) {
    auto& s=*sync_state_;
    std::unique_lock<std::mutex> snapshot_guard(s.snapshot_mutex);
    ++prefix_gc_cycle_;
    const auto hot_begin=hot_store.hlog.begin_address.control();
    if(hot_until<=hot_begin)return;
    std::shared_lock<experimental::ReadWriteGate> copy_gate(s.gate);
    prefix_gc_copying_=true;
    const uint64_t reads_before=prefix_gc_reads_.load(),writes_before=prefix_gc_writes_.load();
    bool session=false;
    try {
      StartSession();session=true;PrefixGcPhase("before-copy");
      const bool hot_ok=CompactHotLog(hot_until,false,2);
      PrefixGcRecord("hot",hot_begin,hot_until,cold_store.hlog.GetTailAddress().control(),hot_ok);
      if(!hot_ok)throw std::runtime_error("native hot-to-cold compaction failed");
      if(const char* hold=std::getenv("F2_GC_COPY_HOLD_DIR")) {
        const auto directory=std::filesystem::path(hold);
        {std::ofstream ready(directory/"ready");ready << prefix_gc_cycle_ << '\n';}
        PrefixGcPhase("copy-hold");
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(!std::filesystem::exists(directory/"release")) {
          if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("GC concurrency probe did not release copy");
          CompletePending(false);Refresh();std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      PrefixGcFlush(cold_store);
      const auto cold_begin=cold_store.hlog.begin_address.control();
      const auto cold_until=PrefixGcUntil(cold_store);
      if(cold_until>cold_begin) {
        const bool cold_ok=CompactColdLog(cold_until,false,2);
        PrefixGcRecord("cold",cold_begin,cold_until,cold_store.hlog.GetTailAddress().control(),cold_ok);
        if(!cold_ok)throw std::runtime_error("native cold-to-cold compaction failed");
      }
      auto& index=cold_store.hash_index_.PrefixIndexStore();
      const auto index_until=PrefixGcUntil(index);
      PrefixGcFlush(index);
      if(index_until>index.hlog.begin_address.control()) {
        const auto index_begin=index.hlog.begin_address.control();
        const bool index_ok=index.CompactWithLookup(index_until,false,1);
        PrefixGcRecord("cold-index",index_begin,index_until,index.hlog.GetTailAddress().control(),index_ok);
        if(!index_ok)throw std::runtime_error("native disk-index compaction failed");
      }
      PrefixGcPhase("after-copy");
      std::ostringstream overlap;
      overlap << "\"event\":\"copy_overlap\",\"cycle\":" << prefix_gc_cycle_
              << ",\"reads\":" << prefix_gc_reads_.load()-reads_before
              << ",\"writes\":" << prefix_gc_writes_.load()-writes_before;
      experimental::GcTrace(overlap.str());
      prefix_gc_copying_=false;
      StopSession();session=false;copy_gate.unlock();
      // No protected session waits for the exclusive publication gate.
      std::unique_lock<experimental::ReadWriteGate> publish_gate(s.gate);
      prefix_gc_publishing_=true;
      PrefixGcWithStableWal([&] {
        StartSession();session=true;prefix_gc_serial_=s.serial;
        PrefixGcShift(hot_store,hot_until);
        PrefixGcShift(cold_store,cold_until);
        PrefixGcShift(index,index_until);
        PrefixGcPhase("after-logical-gc");
        SyncCheckpointAndReclaim();
        // Every checkpoint dependency and the root are stable at this point.
        // Pass a cross-store drain so that PrefixReleaseTruncation's epoch spin
        // can service I/O on all three handlers, not just the one being truncated.
        // Without this, a reader with a pending I/O on a different store's handler
        // can block epoch advancement and stall the truncation indefinitely.
        auto cross_drain=[this]{CompletePending(false);Refresh();};
        hot_store.disk.log().PrefixReleaseTruncation(cross_drain);
        experimental::SyncDirectory(s.root/"hot");PrefixGcPhase("after-hot-truncate");
        cold_store.disk.log().PrefixReleaseTruncation(cross_drain);
        experimental::SyncDirectory(s.root/"cold");PrefixGcPhase("after-cold-truncate");
        index.disk.log().PrefixReleaseTruncation(cross_drain);
        experimental::SyncDirectory(s.root/"cold"/"cold-index");PrefixGcPhase("after-index-truncate");
        StopSession();session=false;PrefixGcPhase("complete");
      });
      prefix_gc_publishing_=false;
    } catch(...) {
      prefix_gc_copying_=false;prefix_gc_publishing_=false;s.poisoned=true;
      if(session) {try{CompletePending(true);StopSession();}catch(...){}}
      throw;
    }
  }
  void PrefixGcStart() {
    prefix_gc_stop_=false;
    prefix_gc_thread_=std::thread([this] {
      while(!prefix_gc_stop_.load()) {
        {
          std::unique_lock<std::mutex> wait_lock(prefix_gc_wait_mutex_);
          prefix_gc_cv_.wait_for(wait_lock,std::chrono::milliseconds(50),[this]{return prefix_gc_stop_.load();});
        }
        if(prefix_gc_stop_.load())break;
        auto& s=*sync_state_;std::unique_lock<experimental::ReadWriteGate> gate(s.gate,std::try_to_lock);
        if(!gate.owns_lock() || s.poisoned)continue;
        const auto bytes=hot_store.hlog.GetTailAddress().control()-hot_store.hlog.begin_address.control();
        if(bytes<s.gc_reclaim_bytes)continue;
        const auto until=PrefixGcUntil(hot_store);
        prefix_gc_serial_=s.serial;prefix_gc_active_=true;gate.unlock();
        try{PrefixGcRun(until);}
        catch(const std::exception& e) {
          prefix_gc_active_=false;std::fprintf(stderr,"F2_GC_FAIL %s\n",e.what());std::fflush(stderr);break;
        }
        prefix_gc_active_=false;
      }
    });
  }
  void PrefixGcStop() {
    prefix_gc_stop_=true;prefix_gc_cv_.notify_all();
    if(prefix_gc_thread_.joinable())prefix_gc_thread_.join();
  }
 private:
  std::thread prefix_gc_thread_;
  std::atomic<bool> prefix_gc_stop_{true};
  std::mutex prefix_gc_wait_mutex_;
  std::condition_variable prefix_gc_cv_;
  uint64_t prefix_gc_cycle_=0;
  uint64_t prefix_gc_serial_=0;
  bool prefix_gc_publishing_=false;
  std::atomic<bool> prefix_gc_active_{false},prefix_gc_copying_{false};
  std::atomic<uint64_t> prefix_gc_reads_{0},prefix_gc_writes_{0};
