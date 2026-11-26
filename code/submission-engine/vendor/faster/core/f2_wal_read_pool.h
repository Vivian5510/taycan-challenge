// Stable F2 reader sessions can keep serving reads during an online checkpoint.
// F2 cannot start a new session while its checkpoint state machine is active.
#pragma once
#include <chrono>
#include <cstddef>
#include <utility>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace FASTER { namespace core { namespace experimental {
class WalReadPool {
  struct Request {
    std::function<Status()> execute;
    Status result=Status::IOError;
    std::exception_ptr error;
    std::mutex mutex;
    std::condition_variable cv;
    bool done=false;
  };
 public:
  WalReadPool(size_t count,std::function<void()> start,
              std::function<void()> poll,std::function<void()> stop)
      :start_(std::move(start)),poll_(std::move(poll)),stop_(std::move(stop)) {
    try {
      for(size_t i=0;i<count;++i)threads_.emplace_back([this]{Run();});
      std::unique_lock<std::mutex> lock(mutex_);
      ready_cv_.wait(lock,[&]{return ready_==threads_.size();});
      if(error_)std::rethrow_exception(error_);
    } catch(...) {Stop();throw;}
  }
  ~WalReadPool() {Stop();}
  Status Execute(std::function<Status()> execute) {
    Request request;request.execute=std::move(execute);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if(error_)std::rethrow_exception(error_);
      if(stopping_)return Status::IOError;
      queue_.push_back(&request);
    }
    cv_.notify_one();
    std::unique_lock<std::mutex> lock(request.mutex);
    request.cv.wait(lock,[&]{return request.done;});
    if(request.error)std::rethrow_exception(request.error);
    return request.result;
  }
 private:
  void Stop() {
    {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;}
    cv_.notify_all();
    for(auto& thread:threads_)if(thread.joinable())thread.join();
  }
  void Run() {
    bool session=false,announced=false;
    try {
      start_();session=true;
      {std::lock_guard<std::mutex> lock(mutex_);++ready_;announced=true;}
      ready_cv_.notify_one();
      for(;;) {
        // Idle protected sessions must still advance epochs and checkpoints.
        poll_();
        Request* request=nullptr;
        {
          std::unique_lock<std::mutex> lock(mutex_);
          cv_.wait_for(lock,std::chrono::microseconds(50),[&]{return stopping_||!queue_.empty();});
          if(queue_.empty()) {if(stopping_)break;else continue;}
          request=queue_.front();queue_.pop_front();
        }
        try {request->result=request->execute();}
        catch(...) {request->error=std::current_exception();}
        {std::lock_guard<std::mutex> lock(request->mutex);
          request->done=true;request->cv.notify_one();}
      }
    } catch(...) {
      std::lock_guard<std::mutex> lock(mutex_);
      error_=std::current_exception();stopping_=true;
      if(!announced)++ready_;
      for(auto* request:queue_) {
        std::lock_guard<std::mutex> done(request->mutex);
        request->error=error_;request->done=true;request->cv.notify_one();
      }
      queue_.clear();cv_.notify_all();ready_cv_.notify_one();
    }
    if(session)try{stop_();}catch(...){}
  }
  std::function<void()> start_,poll_,stop_;
  std::mutex mutex_;
  std::condition_variable cv_,ready_cv_;
  std::deque<Request*> queue_;
  std::vector<std::thread> threads_;
  size_t ready_=0;
  bool stopping_=false;
  std::exception_ptr error_;
};
}}}
