// Included in FileSystemSegmentedFile. Logical FASTER GC finishes normally;
// physical retirement waits for a durable F2 recovery root covering its work.
//
// Stall fix (2026-11-15): the epoch drain that fires the completion callback
// requires every active session to advance its local_current_epoch past the
// epoch bumped by TruncateSegments.  A reader whose pending I/O lives on a
// *different* store's handler (e.g. cold log or disk-index) cannot complete
// its I/O when this spin loop only calls handler_->TryComplete() for the
// current store.  The reader stays blocked, its local epoch never advances,
// safe_to_reclaim_epoch stalls, and the drain callback never fires.
//
// extra_drain is called each iteration so the GC coordinator can service I/O
// across all stores (via CompletePending/Refresh at the F2Kv level).  When
// omitted the behaviour is identical to the original single-handler loop.
//
// A 5-second stderr diagnostic is printed while stalled so that future
// occurrences produce actionable epoch and segment data without a debugger.
 public:
  bool prefix_defer_truncation_=false;
  uint64_t prefix_deferred_begin_=0;
  void PrefixDeferTruncation() {prefix_defer_truncation_=true;}
  void PrefixReleaseTruncation(std::function<void()> extra_drain={}) {
    const uint64_t target=prefix_deferred_begin_;
    if(!target || !files_.load())return;
    struct Context final:core::IAsyncContext {
      std::atomic<bool>* done;
      explicit Context(std::atomic<bool>* value):done(value){}
      core::Status DeepCopy_Internal(core::IAsyncContext*& copy) override {
        return core::IAsyncContext::DeepCopy_Internal(*this,copy);
      }
    };
    std::atomic<bool> done{false};Context context(&done);core::GcState state;
    state.Initialize(core::Address(target*kSegmentSize),
      +[](core::IAsyncContext* pointer,uint64_t) {static_cast<Context*>(pointer)->done->store(true);},
      static_cast<core::GcState::internal_complete_callback_t>(nullptr),&context);
    begin_segment_.store(target);
    TruncateSegments(target,state);
    auto last_diag=std::chrono::steady_clock::now();
    while(!done.load()) {
      handler_->TryComplete();
      if(extra_drain)extra_drain();
      epoch_->ProtectAndDrain();
      std::this_thread::yield();
      const auto now=std::chrono::steady_clock::now();
      if(now-last_diag>=std::chrono::seconds(5)){
        last_diag=now;
        std::fprintf(stderr,
          "F2_GC_STALL PrefixReleaseTruncation: target_segment=%zu "
          "epoch_current=%zu epoch_safe=%zu\n",
          size_t(target),
          size_t(epoch_->current_epoch.load()),
          size_t(epoch_->safe_to_reclaim_epoch.load()));
        std::fflush(stderr);
      }
    }
  }
