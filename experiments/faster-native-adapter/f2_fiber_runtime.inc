// Experiment: one OS owner, one F2 session, fixed reusable Boost.Fiber workers.
// No new WAL or I/O worker thread. POSIX aio_fsync completes before index
// publication and the SDK response. Only the owner polls F2 and resumes fibers.
class FiberBackend final:public FasterBackend {
  struct Request {
    unsigned operation;
    const std::string *key,*value;
    std::string* output;
    reference::CommitWait::Writer waiter;
    F2Result result{F2ResultCode::error,"unfinished fiber request"};
    FStatus read_status=FStatus::Corruption;
    std::shared_ptr<Completion> read_completion;
    std::shared_ptr<owner_runtime::CompletionEvent> event;
  };
  struct SyncJob {
    aiocb control{};
    boost::fibers::mutex mutex;
    boost::fibers::condition_variable ready;
    int error=0;
    bool done=false;
  };
 public:
  FiberBackend(const std::string& directory,uint64_t budget) {
    const char* mode=std::getenv("F2_FIBER_SYNC_MODE");
    blocking_sync_=mode && std::string(mode)=="blocking";
    if(mode && !blocking_sync_ && std::string(mode)!="aio")throw std::runtime_error("unknown fiber sync mode");
    if(const char* collect=std::getenv("F2_FIBER_COLLECT_US"))collect_us_=std::stoul(collect);
    if(collect_us_>1000)throw std::runtime_error("fiber collection window exceeds 1000us");
    if(const char* completion=std::getenv("F2_FIBER_COMPLETION")) {
      if(std::string(completion)=="event")event_completion_=true;
      else if(std::string(completion)!="cv")throw std::runtime_error("unknown owner completion mode");
    }
    if(const char* inbox=std::getenv("F2_FIBER_INBOX")) {
      if(std::string(inbox)=="helio")ring_inbox_=true;
      else if(std::string(inbox)!="deque")throw std::runtime_error("unknown owner inbox");
    }
    if(const char* consumer=std::getenv("F2_FIBER_CONSUMER")) {
      if(std::string(consumer)=="tasklet")tasklet_consumer_=true;
      else if(std::string(consumer)!="poller")throw std::runtime_error("unknown owner consumer");
    }
    if(const char* dispatch=std::getenv("F2_FIBER_DISPATCH_MODE")) {
      const std::string value(dispatch);
      if(value=="targeted")dispatch_=1;
      else if(value=="batch16")dispatch_=2;
      else if(value=="inline")dispatch_=3;
      else if(value=="inline32")dispatch_=4;
      else if(value!="legacy")throw std::runtime_error("unknown fiber dispatch mode");
    }
    thread_=std::thread([this,directory,budget]{Run(directory,budget);});
    std::unique_lock<std::mutex> lock(mutex_);initialized_.wait(lock,[this]{return ready_;});
    if(!error_.empty()){lock.unlock();thread_.join();throw std::runtime_error(error_);}
  }
  ~FiberBackend() override {
    {std::lock_guard<std::mutex> lock(mutex_);closing_=true;work_.notify_one();}
    if(thread_.joinable())thread_.join();
  }
  F2Result Put(const std::string& k,const std::string& v) override{return Submit(1,k,&v,nullptr);}
  F2Result Delete(const std::string& k) override{return Submit(2,k,nullptr,nullptr);}
  F2Result Get(const std::string& k,std::string* v) override {
    if(!v)return {F2ResultCode::error,"null output"};return Submit(0,k,nullptr,v);
  }
 private:
  F2Result Submit(unsigned operation,const std::string& key,const std::string* value,std::string* output) {
    if(key.empty() || key.size()+(value?value->size():0)+128>(256ULL<<10))return {F2ResultCode::error,"invalid record"};
    Request request{operation,&key,value,output};
    if(event_completion_) {
      static thread_local auto event=std::make_shared<owner_runtime::CompletionEvent>();
      event->Reset();request.event=event;
    }
    auto await=[&]{if(event_completion_)request.event->Wait();else wait_.Wait(&request.waiter);};
    if(ring_inbox_) {
      if(closing_)return {F2ResultCode::error,"fiber owner closed"};
      ++ring_outstanding_;
      if(!helio_inbox_.try_enqueue(&request)) {
#ifdef F2_HELIO_SATURATION_TEST
        if(!saturation_full_.exchange(true)) {
          const char* fd=std::getenv("F2_HELIO_FULL_FD");const char byte='F';
          if(fd&&::write(std::stoi(fd),&byte,1)!=1)throw std::runtime_error("saturation full notice failed");
        }
#endif
        std::unique_lock<std::mutex> lock(mutex_);++ring_space_waiters_;
        space_.wait(lock,[&]{return helio_inbox_.try_enqueue(&request);});
        --ring_space_waiters_;
      }
      // The poller announces parking before checking the atomic predicate.
      // Notify under the sleep mutex only on the slow path; no lost wakeup
      // between its predicate check and the OS condition-variable wait.
      if(ring_parked_.load()) {
        std::lock_guard<std::mutex> lock(mutex_);work_.notify_one();++owner_notifications_;
      }
      await();return request.result;
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if(!dispatch_ || outstanding_>=256) {
        ++space_waiters_;space_.wait(lock,[this]{return outstanding_<256||closing_;});--space_waiters_;
      }
      if(closing_)return {F2ResultCode::error,"fiber owner closed"};
      ++outstanding_;incoming_.push_back(&request);peak_=std::max(peak_,outstanding_);
      if(!dispatch_ || owner_parked_){work_.notify_one();++owner_notifications_;}
    }
    await();return request.result;
  }
  void Finish(Request* request,F2Result result) {
    // Like Helio Done's shared Impl: keep notification storage alive after
    // publishing readiness. The waiter may immediately destroy its request.
    auto event=request->event;
    request->result=std::move(result);
    if(ring_inbox_)--ring_outstanding_;
    else {std::lock_guard<std::mutex> lock(mutex_);--outstanding_;
      if(!dispatch_ || space_waiters_){space_.notify_one();++space_notifications_;}}
    if(event){++event_completions_;if(event->Notify())++event_wakes_;}
    else wait_.Complete(&request->waiter); // Last request access.
  }
  static void Cooperate(void*) {boost::this_fiber::yield();}
  static int Sync(void* owner,int fd) {return static_cast<FiberBackend*>(owner)->SyncFile(fd);}
  int SyncFile(int fd) {
    // Test-only fence, before the real sync: no ACK may pass this boundary.
    if(!test_hold_used_) {
      const char* release=std::getenv("F2_FIBER_TEST_RELEASE_FD");
      const char* notice=std::getenv("F2_FIBER_TEST_NOTICE_FD");
      if(release && notice) {
        test_hold_used_=true;sync_held_=true;
        char byte='R';if(::write(std::stoi(notice),&byte,1)!=1)throw std::runtime_error("fiber notice failed");
        pollfd event{std::stoi(release),POLLIN,0};
        while(::poll(&event,1,0)==0)boost::this_fiber::yield();
        if(::read(event.fd,&byte,1)!=1 || byte!='G')throw std::runtime_error("fiber sync release failed");
        sync_held_=false;
      }
    }
    if(blocking_sync_)return ::fsync(fd);
    SyncJob job;job.control.aio_fildes=fd;job.control.aio_sigevent.sigev_notify=SIGEV_NONE;
    if(::aio_fsync(O_SYNC,&job.control)<0)return -1;
    ++async_syncs_;sync_jobs_.push_back(&job);
    std::unique_lock<boost::fibers::mutex> lock(job.mutex);
    job.ready.wait(lock,[&]{return job.done;});
    if(job.error){errno=job.error;return -1;}return 0;
  }
  void PollSync() {
    for(auto it=sync_jobs_.begin();it!=sync_jobs_.end();) {
      auto* job=*it;const int error=::aio_error(&job->control);++sync_polls_;
      if(error==EINPROGRESS){++it;continue;}
      const auto result=::aio_return(&job->control);
      job->error=error?error:(result<0?errno:0);job->done=true;
      it=sync_jobs_.erase(it);job->ready.notify_one();
    }
  }
  size_t Receive(Request** batch,size_t limit) {
    size_t count=0;
#ifdef F2_HELIO_SATURATION_TEST
    if(!saturation_hold_used_) {
      const char* fd=std::getenv("F2_HELIO_RELEASE_FD");
      if(fd) {
        saturation_hold_used_=true;pollfd release{std::stoi(fd),POLLIN,0};char byte=0;
        while(::poll(&release,1,0)==0)boost::this_fiber::yield();
        if(::read(release.fd,&byte,1)!=1||byte!='G')throw std::runtime_error("saturation release failed");
      }
    }
#endif
    if(ring_inbox_) {
      Request* request;
      while(count<limit && helio_inbox_.try_dequeue_sc(request))batch[count++]=request;
      // Like Helio FiberQueue: notify full-queue producers once per drain,
      // rather than taking an OS mutex for every completed request.
      if(count&&ring_space_waiters_.load()) {
        std::lock_guard<std::mutex> lock(mutex_);space_.notify_all();++space_notifications_;
      }
    } else {
      std::lock_guard<std::mutex> lock(mutex_);
      while(!incoming_.empty()&&count<limit){batch[count++]=incoming_.front();incoming_.pop_front();}
    }
    return count;
  }
  bool HasIncoming() {
    if(ring_inbox_)return !helio_inbox_.empty();
    std::lock_guard<std::mutex> lock(mutex_);return !incoming_.empty();
  }
  void Run(const std::string& directory,uint64_t budget) {
    std::unique_ptr<SyncBackend> backend;
    try{backend=std::make_unique<SyncBackend>(directory,budget);backend->FiberBegin();}
    catch(const std::exception& e){error_=e.what();}
    {std::lock_guard<std::mutex> lock(mutex_);ready_=true;initialized_.notify_one();}
    if(!error_.empty())return;
    ex::fiber_owner=this;ex::fiber_cooperate=Cooperate;ex::fiber_sync=Sync;
    boost::fibers::mutex internal_mutex;
    boost::fibers::condition_variable writes_ready,reads_ready,tasks_ready;
    std::deque<Request*> writes,reads;
    bool stop=false,maintenance=false,failed=false;
    size_t active_reads=0;
    size_t sleeping_readers=0;
    bool writer_waiting=false;
    bool consumer_waiting=false;
    uint64_t requests=0,read_requests=0,reads_while_sync=0,groups=0,written=0,checkpoints=0;
    uint64_t read_notifications=0,write_notifications=0,dispatch_batches=0,inline_reads=0,pending_reads=0;
    uint64_t consumer_batches=0;
    const bool start_reads=dispatch_>=3 || tasklet_consumer_;
    std::vector<boost::fibers::fiber> readers;
    for(unsigned i=0;i<32;++i)readers.emplace_back(std::allocator_arg,boost::fibers::fixedsize_stack(128<<10),[&]{
      for(;;) {
        Request* request;
        {
          std::unique_lock<boost::fibers::mutex> lock(internal_mutex);
          ++sleeping_readers;
          reads_ready.wait(lock,[&]{return stop||!reads.empty();});
          --sleeping_readers;
          if(reads.empty()&&stop)return;
          request=reads.front();reads.pop_front();
        }
        if(!start_reads){++read_requests;if(!sync_jobs_.empty()||sync_held_)++reads_while_sync;}
        auto result=failed?F2Result{F2ResultCode::error,"fiber owner failed"}:
          start_reads?backend->FiberFinishRead(request->read_status,request->read_completion,request->output):
            backend->FiberGet(*request->key,request->output);
        --active_reads;Finish(request,std::move(result));
      }
    });
    boost::fibers::fiber writer(std::allocator_arg,boost::fibers::fixedsize_stack(256<<10),[&]{
      for(;;) {
        std::vector<Request*> group;size_t bytes=0;
        {
          std::unique_lock<boost::fibers::mutex> lock(internal_mutex);
          writer_waiting=true;
          writes_ready.wait(lock,[&]{return stop||!writes.empty();});
          writer_waiting=false;
          if(writes.empty()&&stop)return;
          // A bounded window, never a minimum batch size. Main/poller and read
          // fibers keep running while requests accumulate on this writer.
          const auto deadline=std::chrono::steady_clock::now()+std::chrono::microseconds(collect_us_);
          lock.unlock();
          while(collect_us_ && writes.size()<32 &&
                std::chrono::steady_clock::now()<deadline)boost::this_fiber::yield();
          lock.lock();
          do {
            auto* request=writes.front();const auto size=request->key->size()+(request->value?request->value->size():0)+128;
            if(!group.empty()&&bytes+size>(256ULL<<10))break;
            group.push_back(request);bytes+=size;writes.pop_front();
          }while(!writes.empty()&&group.size()<64);
        }
        F2Result result{F2ResultCode::error,"fiber owner poisoned"};
        if(!failed)try {
          rocksdb::WriteBatch batch;
          for(auto* request:group)reference::Check(request->operation==1?batch.Put(*request->key,*request->value):batch.Delete(*request->key));
          result=backend->FiberBatch(batch);++groups;written+=group.size();
          if(result.code==F2ResultCode::error)failed=true;
          if(!failed&&backend->FiberNeedsCheckpoint()) {
            maintenance=true;
            while(active_reads)boost::this_fiber::yield();
            backend->FiberCheckpoint();++checkpoints;maintenance=false;
          }
        }catch(const std::exception& e){failed=true;maintenance=false;result={F2ResultCode::error,e.what()};}
        for(auto* request:group)Finish(request,result);
      }
    });
    auto deliver=[&](Request** arrived,size_t count) {
      if(count) {
        ++dispatch_batches;
        size_t added_writes=0,added_reads=0;
        for(size_t i=0;i<count;++i) {
          auto* request=arrived[i];
          ++requests;
          if(request->operation){writes.push_back(request);++added_writes;}
          else if(start_reads) {
            ++read_requests;if(!sync_jobs_.empty()||sync_held_)++reads_while_sync;
            auto started=backend->FiberStartRead(*request->key);
            if(started.first!=FStatus::Pending) {
              ++inline_reads;Finish(request,backend->FiberFinishRead(started.first,started.second,request->output));
            } else {
              ++pending_reads;request->read_status=started.first;request->read_completion=std::move(started.second);
              reads.push_back(request);++active_reads;++added_reads;
            }
          } else {reads.push_back(request);++active_reads;++added_reads;}
        }
        if(!dispatch_) {
          writes_ready.notify_one();reads_ready.notify_all();++write_notifications;read_notifications+=32;
        } else {
          if(added_writes&&writer_waiting){writes_ready.notify_one();++write_notifications;}
          const auto count=std::min(added_reads,sleeping_readers);
          for(size_t i=0;i<count;++i)reads_ready.notify_one();
          read_notifications+=count;
        }
      }
    };
    boost::fibers::fiber consumer;
    if(tasklet_consumer_)consumer=boost::fibers::fiber(std::allocator_arg,boost::fibers::fixedsize_stack(128<<10),[&]{
      Request* batch[16];
      for(;;) {
        {
          std::unique_lock<boost::fibers::mutex> lock(internal_mutex);consumer_waiting=true;
          tasks_ready.wait(lock,[&]{return stop||(!maintenance&&HasIncoming());});consumer_waiting=false;
          if(stop)return;
        }
        const auto count=Receive(batch,16);++consumer_batches;
        // Corresponds to Helio's consumer-side tasklet execution: short
        // synchronous reads finish here; Pending operations are dispatched
        // to an existing read fiber, while the poller keeps advancing I/O.
        deliver(batch,count);
        boost::this_fiber::yield();
      }
    });
    Request* arrived[256];
    for(;;) {
      if(ring_inbox_) {
        peak_=std::max(peak_,ring_outstanding_.load());
        if(!ring_outstanding_.load()) {
          std::unique_lock<std::mutex> lock(mutex_);ring_parked_=true;
          work_.wait(lock,[&]{return ring_outstanding_.load()||closing_;});ring_parked_=false;
        }
        if(!ring_outstanding_.load()&&closing_)break;
      } else {
        std::unique_lock<std::mutex> lock(mutex_);
        if(!outstanding_&&!closing_) {
          owner_parked_=true;work_.wait(lock,[&]{return outstanding_||closing_;});owner_parked_=false;
        }
        if(!outstanding_&&closing_)break;
      }
      if(!maintenance) {
        if(tasklet_consumer_) {
          if(consumer_waiting&&HasIncoming())tasks_ready.notify_one();
        } else {
          const auto count=Receive(arrived,dispatch_<2||dispatch_==4?256:16);deliver(arrived,count);
        }
      }
      backend->FiberPoll();PollSync();boost::this_fiber::yield();
    }
    stop=true;writes_ready.notify_one();reads_ready.notify_all();tasks_ready.notify_one();
    if(consumer.joinable())consumer.join();writer.join();for(auto& reader:readers)reader.join();
    ex::fiber_sync=nullptr;ex::fiber_cooperate=nullptr;ex::fiber_owner=nullptr;
    backend->FiberEnd();
    std::fprintf(stderr,"F2_COMPLETION event=%u completions=%llu kernel_wakes=%llu\n",unsigned(event_completion_),(unsigned long long)event_completions_,(unsigned long long)event_wakes_);
    std::fprintf(stderr,"F2_HELIO inbox=%u tasklet=%u consumer_batches=%llu\n",unsigned(ring_inbox_),unsigned(tasklet_consumer_),(unsigned long long)consumer_batches);
    std::fprintf(stderr,"F2_DISPATCH mode=%u batches=%llu inline_reads=%llu pending_reads=%llu read_notifies=%llu write_notifies=%llu owner_notifies=%llu space_notifies=%llu\n",
      dispatch_,(unsigned long long)dispatch_batches,(unsigned long long)inline_reads,(unsigned long long)pending_reads,
      (unsigned long long)read_notifications,(unsigned long long)write_notifications,
      (unsigned long long)owner_notifications_,(unsigned long long)space_notifications_);
    std::fprintf(stderr,"F2_FIBER requests=%llu reads=%llu groups=%llu writes=%llu reads_while_sync=%llu async_syncs=%llu sync_polls=%llu peak_requests=%zu checkpoints=%llu sync_mode=%s\n",
      (unsigned long long)requests,(unsigned long long)read_requests,(unsigned long long)groups,(unsigned long long)written,
      (unsigned long long)reads_while_sync,(unsigned long long)async_syncs_,(unsigned long long)sync_polls_,peak_,
      (unsigned long long)checkpoints,blocking_sync_?"blocking":"aio");
  }
  std::mutex mutex_;
  std::condition_variable initialized_,work_,space_;
  std::deque<Request*> incoming_;
  std::vector<SyncJob*> sync_jobs_;
  std::thread thread_;
  reference::CommitWait wait_;
  std::string error_;
  bool ready_=false,blocking_sync_=false,test_hold_used_=false,sync_held_=false;
  std::atomic<bool> closing_{false};
  size_t outstanding_=0,peak_=0;
  uint64_t async_syncs_=0,sync_polls_=0;
  unsigned collect_us_=0;
  unsigned dispatch_=0;
  bool owner_parked_=false;
  size_t space_waiters_=0;
  uint64_t owner_notifications_=0,space_notifications_=0;
  bool ring_inbox_=false,tasklet_consumer_=false;
  bool event_completion_=false;
  uint64_t event_completions_=0,event_wakes_=0;
  base::mpmc_bounded_queue<Request*> helio_inbox_{256};
  alignas(64) std::atomic<size_t> ring_outstanding_{0};
  alignas(64) std::atomic<bool> ring_parked_{false};
  std::atomic<size_t> ring_space_waiters_{0};
#ifdef F2_HELIO_SATURATION_TEST
  bool saturation_hold_used_=false;
  std::atomic<bool> saturation_full_{false};
#endif
};
