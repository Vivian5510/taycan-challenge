// Opt-in bounded WAL I/O pipeline. One submitter polls several immutable WAL
// frames; a separate worker applies completed frames in submission order.
#pragma once

namespace FASTER { namespace core { namespace experimental {
class WalPipelineOwner {
 public:
  using Group=WalWriteOwner::Group;
  using Write=std::shared_ptr<BatchLog::PendingWrite>;
  using Submit=std::function<Write(const Group&)>;
  using Finish=std::function<void(const Write&)>;
  using Apply=std::function<bool(const Group&,const Write&)>;
  WalPipelineOwner(size_t target,uint64_t wait_us,uint64_t spin_us,size_t depth,
                   Submit submit,std::function<void()> poll,Finish finish,Apply apply,
                   std::function<void()> poison,std::function<bool()> must_drain,
                   std::function<void()> checkpoint)
      :target_(target),depth_(depth),wait_us_(wait_us),spin_us_(spin_us),
       submit_(std::move(submit)),poll_(std::move(poll)),finish_(std::move(finish)),
       apply_(std::move(apply)),poison_(std::move(poison)),must_drain_(std::move(must_drain)),
       checkpoint_(std::move(checkpoint)) {
    apply_thread_=std::thread([this]{RunApply();});
    try {io_thread_=std::thread([this]{RunIo();});}
    catch(...) {{std::lock_guard<std::mutex> lock(mutex_);io_stopped_=true;}apply_cv_.notify_all();apply_thread_.join();throw;}
  }
  ~WalPipelineOwner() {
    {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;}
    cv_.notify_all();apply_cv_.notify_all();io_thread_.join();apply_thread_.join();
    std::fprintf(stderr,"F2_WAL_PIPELINE depth=%zu max_batch=%zu groups=%llu operations=%llu max_outstanding=%llu\n",
      depth_,target_,(unsigned long long)groups_,(unsigned long long)operations_,
      (unsigned long long)max_outstanding_);
  }
  Status Execute(SyncRequest& request) {
    std::shared_ptr<Job> job;
    bool notify=false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if(stopping_||failed_)return Status::IOError;
      if(waiting_.empty()||waiting_.back()->requests.size()>=target_) {
        waiting_.push_back(std::make_shared<Job>());
        notify=true;
      }
      job=waiting_.back();job->requests.push_back(&request);
      notify=notify||job->requests.size()>=target_;
    }
    if(notify)cv_.notify_one();job->completion.Wait();return request.status;
  }
 private:
  using Clock=std::chrono::steady_clock;
  struct Job {
    Group requests;
    Write write;
    WalWriteOwner::Completion completion;
    Clock::time_point created{};
    bool collecting=false;
  };
  static void Pause() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
  }
  void Fail() {
    failed_.store(true,std::memory_order_release);poison_();cv_.notify_all();apply_cv_.notify_all();
  }
  void Reject(const std::shared_ptr<Job>& job) {
    for(auto* request:job->requests)request->status=Status::IOError;
    job->completion.Complete();
  }
  void RunIo() {
    std::deque<std::shared_ptr<Job>> inflight;
    auto idle_since=Clock::now();
    for(;;) {
      std::shared_ptr<Job> submitting;
      bool checkpointing=false;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if(failed_) {
          for(auto& job:waiting_)Reject(job);
          waiting_.clear();
          break;
        }
        if(stopping_&&waiting_.empty()&&inflight.empty())break;
        const auto now=Clock::now();
        const bool draining=must_drain_();
        checkpointing=draining&&outstanding_==0;
        // Bound the entire pipeline as well as I/O. With 32 synchronous callers,
        // a third half-filled group otherwise fragments two 16-request groups.
        const auto capacity=std::max<size_t>(2,depth_);
        const bool collect=!draining&&inflight.size()<depth_&&outstanding_<capacity&&!waiting_.empty();
        if(collect&&!waiting_.front()->collecting) {
          waiting_.front()->collecting=true;waiting_.front()->created=now;
        }
        if(collect&&
           (waiting_.front()->requests.size()>=target_||stopping_||
            now>=waiting_.front()->created+std::chrono::microseconds(wait_us_))) {
          submitting=waiting_.front();waiting_.pop_front();++outstanding_;
          max_outstanding_=std::max<uint64_t>(max_outstanding_,outstanding_);
        }
        if(!submitting&&inflight.empty()&&now>=idle_since+std::chrono::microseconds(spin_us_)) {
          if(waiting_.empty()||!collect)cv_.wait_for(lock,std::chrono::microseconds(50));
          else cv_.wait_until(lock,waiting_.front()->created+std::chrono::microseconds(wait_us_));
          idle_since=Clock::now();
        }
      }
      try {
        if(checkpointing)checkpoint_();
        if(submitting) {
          submitting->write=submit_(submitting->requests);
          inflight.push_back(submitting);submitting.reset();idle_since=Clock::now();
        }
        if(!inflight.empty()) {
          poll_();
          while(!inflight.empty()&&inflight.front()->write->done) {
            auto job=inflight.front();
            finish_(job->write); // Reject short/error completion before publication.
            {
              std::lock_guard<std::mutex> lock(mutex_);
              ready_.push_back(job);
            }
            inflight.pop_front();apply_cv_.notify_one();idle_since=Clock::now();
          }
        }
      } catch(...) {
        Fail();if(submitting)Reject(submitting);
      }
      Pause();
    }
    // Kernel I/O owns copies, not request memory. The BatchLog retains all
    // buffers through io_destroy even when the instance fails before completion.
    for(auto& job:inflight)Reject(job);
    {
      std::lock_guard<std::mutex> lock(mutex_);io_stopped_=true;
    }
    apply_cv_.notify_all();
  }
  void RunApply() {
    for(;;) {
      std::shared_ptr<Job> job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        apply_cv_.wait(lock,[&]{return io_stopped_||!ready_.empty();});
        if(ready_.empty()&&io_stopped_)return;
        job=ready_.front();ready_.pop_front();
      }
      bool ok=false;
      if(!failed_)try{ok=apply_(job->requests,job->write);}catch(...){}
      if(!ok) {Fail();Reject(job);}
      else {++groups_;operations_+=job->requests.size();job->completion.Complete();}
      {
        std::lock_guard<std::mutex> lock(mutex_);--outstanding_;
      }
      cv_.notify_all();
    }
  }
  size_t target_,depth_;
  uint64_t wait_us_,spin_us_;
  Submit submit_;std::function<void()> poll_;Finish finish_;Apply apply_;std::function<void()> poison_;
  std::function<bool()> must_drain_;std::function<void()> checkpoint_;
  std::mutex mutex_;std::condition_variable cv_,apply_cv_;
  std::deque<std::shared_ptr<Job>> waiting_,ready_;
  std::atomic<bool> failed_{false};
  bool stopping_=false,io_stopped_=false;
  size_t outstanding_=0;
  uint64_t max_outstanding_=0,groups_=0,operations_=0;
  std::thread io_thread_,apply_thread_;
};
}}}
