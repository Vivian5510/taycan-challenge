// Included in FileSystemSegmentedFile. Logical FASTER GC finishes normally;
// physical retirement waits for a durable F2 recovery root covering its work.
 public:
  bool prefix_defer_truncation_=false;
  uint64_t prefix_deferred_begin_=0;
  void PrefixDeferTruncation() {prefix_defer_truncation_=true;}
  void PrefixReleaseTruncation() {
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
    while(!done.load()) {handler_->TryComplete();epoch_->ProtectAndDrain();std::this_thread::yield();}
  }
