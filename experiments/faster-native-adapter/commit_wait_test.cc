// Exercise completion-before-wait and destruction-after-notification races.
#include "core/f2_commit_wait.h"
#include <atomic>
#include <array>
#include <iostream>
#include <thread>

int main() {
  reference::CommitWait wait;
  for(unsigned i=0;i<10000;++i) {
    reference::CommitWait::Writer completed;
    wait.Complete(&completed);
    wait.Wait(&completed);
  }
  std::array<std::atomic<reference::CommitWait::Writer*>,16> slots{};
  for(auto& slot:slots)slot.store(nullptr);
  std::atomic<unsigned> finished{0};
  std::array<std::thread,16> callers;
  for(unsigned id=0;id<callers.size();++id)callers[id]=std::thread([&,id] {
    for(unsigned round=0;round<2000;++round) {
      reference::CommitWait::Writer request;
      auto* expected=static_cast<reference::CommitWait::Writer*>(nullptr);
      while(!slots[id].compare_exchange_weak(expected,&request))expected=nullptr;
      wait.Wait(&request);
      // request immediately dies: completer must have finished every access.
    }
    ++finished;
  });
  while(finished!=callers.size())for(auto& slot:slots) {
    auto* request=slot.exchange(nullptr);
    if(request)wait.Complete(request);
  }
  for(auto& caller:callers)caller.join();
  std::cout<<"PASS complete_before_wait=10000 concurrent_stack_lifetimes=32000\n";
}
