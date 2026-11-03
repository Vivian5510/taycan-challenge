// Completion predicate/registration follows the Helio Done/EventCount pattern.
// This is an OS-thread boundary adapter, not Helio's fiber wait-queue code.
#pragma once
#include <atomic>
#include <cerrno>
#include <stdexcept>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/semaphore.h>
#include <mach/sync_policy.h>
#else
#include <semaphore.h>
#endif

namespace owner_runtime {
class CompletionEvent {
 public:
  CompletionEvent() {
#if defined(__APPLE__)
    if(semaphore_create(mach_task_self(),&semaphore_,SYNC_POLICY_FIFO,0)!=KERN_SUCCESS)
      throw std::runtime_error("create completion semaphore failed");
#else
    if(sem_init(&semaphore_,0,0))throw std::runtime_error("create completion semaphore failed");
#endif
  }
  ~CompletionEvent() {
#if defined(__APPLE__)
    semaphore_destroy(mach_task_self(),semaphore_);
#else
    sem_destroy(&semaphore_);
#endif
  }
  CompletionEvent(const CompletionEvent&)=delete;
  CompletionEvent& operator=(const CompletionEvent&)=delete;
  void Reset() {
    if(state_.exchange(kPending,std::memory_order_acq_rel)!=kDone)
      throw std::runtime_error("completion event reused before completion");
  }
  bool Wait() {
    unsigned expected=kPending;
    if(!state_.compare_exchange_strong(expected,kWaiting,std::memory_order_acq_rel)) {
      if(expected!=kDone)throw std::runtime_error("multiple waiters on completion event");
      return false;
    }
    // Once registered, always consume this round's semaphore token, even if
    // ready is already visible. This prevents a late signal leaking into the
    // next Reset/Wait round of the reusable per-caller event.
#if defined(__APPLE__)
    kern_return_t code;
    do{code=semaphore_wait(semaphore_);}while(code==KERN_ABORTED);
    if(code!=KERN_SUCCESS)throw std::runtime_error("completion semaphore wait failed");
#else
    int code;do{code=sem_wait(&semaphore_);}while(code<0&&errno==EINTR);
    if(code<0)throw std::runtime_error("completion semaphore wait failed");
#endif
    if(state_.load(std::memory_order_acquire)!=kDone)
      throw std::runtime_error("completion token without completed request");
    return true;
  }
  bool Notify() {
    const auto previous=state_.exchange(kDone,std::memory_order_acq_rel);
    if(previous==kPending)return false;
    if(previous!=kWaiting)throw std::runtime_error("duplicate completion event notification");
#if defined(__APPLE__)
    if(semaphore_signal(semaphore_)!=KERN_SUCCESS)
      throw std::runtime_error("completion semaphore signal failed");
#else
    if(sem_post(&semaphore_))throw std::runtime_error("completion semaphore signal failed");
#endif
    return true;
  }
 private:
  enum:unsigned{kPending=0,kWaiting=1,kDone=2};
  std::atomic<unsigned> state_{kDone};
#if defined(__APPLE__)
  semaphore_t semaphore_{};
#else
  sem_t semaphore_{};
#endif
};
} // namespace owner_runtime
