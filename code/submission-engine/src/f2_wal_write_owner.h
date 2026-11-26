// One durable WAL stream and one ordered apply owner. No reader gate is held.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#ifdef __linux__
#include <climits>
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

namespace FASTER { namespace core { namespace experimental {
class WalWriteOwner {
 public:
  class Completion {
   public:
    void Wait() {
#ifdef __linux__
      while(!done_.load(std::memory_order_acquire)) {
        const auto result=::syscall(SYS_futex,&done_,FUTEX_WAIT_PRIVATE,0,nullptr,nullptr,0);
        if(result<0&&errno!=EINTR&&errno!=EAGAIN)std::terminate();
      }
#else
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock,[&]{return done_.load(std::memory_order_acquire);});
#endif
    }
    void Complete() {
#ifdef __linux__
      done_.store(1,std::memory_order_release);
      if(::syscall(SYS_futex,&done_,FUTEX_WAKE_PRIVATE,INT_MAX,nullptr,nullptr,0)<0)std::terminate();
#else
      {std::lock_guard<std::mutex> lock(mutex_);done_.store(1,std::memory_order_release);}
      cv_.notify_all();
#endif
    }
   private:
    alignas(4) std::atomic<uint32_t> done_{0};
#ifndef __linux__
    std::mutex mutex_;std::condition_variable cv_;
#endif
  };
 public:
  using Group=std::vector<SyncRequest*>;
  WalWriteOwner(size_t target,uint64_t wait_us,uint64_t spin_us,
                std::function<bool(const Group&)> execute,size_t max_batch=0)
      :target_(target),max_batch_(max_batch),wait_us_(wait_us),spin_us_(spin_us),execute_(std::move(execute)),
       thread_([this]{Run();}) {}
  ~WalWriteOwner() {
    {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;}
    cv_.notify_all();thread_.join();
  }
  Status Execute(SyncRequest& request) {
    std::shared_ptr<Completion> batch;
    bool notify=false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if(max_batch_)cv_.wait(lock,[&]{return stopping_||failed_||queue_.size()<max_batch_;});
      if(stopping_||failed_)return Status::IOError;
      if(queue_.empty()) {collecting_=std::make_shared<Completion>();notify=true;}
      batch=collecting_;queue_.push_back(&request);
      depth_.store(queue_.size(),std::memory_order_release);
      notify=notify||queue_.size()>=target_;
    }
    if(notify)cv_.notify_one();
    // All synchronous callers wait on the same group completion. Only the
    // owner spins, avoiding 32 callers competing across storage latency.
    batch->Wait();
    return request.status;
  }
 private:
  using Clock=std::chrono::steady_clock;
  static void Pause() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
  }
  void Run() {
    Group group;group.reserve(128);
    for(;;) {
      if(spin_us_) {
        const auto until=Clock::now()+std::chrono::microseconds(spin_us_);
        while(depth_.load(std::memory_order_acquire)==0&&Clock::now()<until)Pause();
      }
      std::shared_ptr<Completion> batch;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock,[&]{return stopping_||!queue_.empty();});
        if(queue_.empty()&&stopping_)return;
        const auto until=Clock::now()+std::chrono::microseconds(wait_us_);
        while(queue_.size()<target_&&!stopping_) {
          if(spin_us_) {
            lock.unlock();
            while(depth_.load(std::memory_order_acquire)<target_&&Clock::now()<until)Pause();
            lock.lock();break;
          }
          if(cv_.wait_until(lock,until)==std::cv_status::timeout)break;
        }
        group.assign(queue_.begin(),queue_.end());queue_.clear();
        batch=std::move(collecting_);depth_.store(0,std::memory_order_release);
        if(max_batch_)cv_.notify_all();
      }
      bool failed=true;
      try{failed=!execute_(group);}catch(...){}
      std::shared_ptr<Completion> pending;
      if(failed) {
        std::lock_guard<std::mutex> lock(mutex_);failed_=true;
        for(auto* request:group)request->status=Status::IOError;
        for(auto* request:queue_)request->status=Status::IOError;
        queue_.clear();depth_.store(0,std::memory_order_release);
        pending=std::move(collecting_);
        cv_.notify_all();
      }
      if(pending)pending->Complete();
      batch->Complete();
    }
  }
  size_t target_;
  size_t max_batch_;
  uint64_t wait_us_,spin_us_;
  std::function<bool(const Group&)> execute_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<SyncRequest*> queue_;
  std::shared_ptr<Completion> collecting_;
  std::atomic<size_t> depth_{0};
  bool stopping_=false,failed_=false;
  std::thread thread_;
};
}}}
