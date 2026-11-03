#include "owner_completion_event.h"
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
int main(){
 using Event=owner_runtime::CompletionEvent;
 auto completed=std::make_shared<Event>();
 for(unsigned i=0;i<10000;++i){completed->Reset();completed->Notify();if(completed->Wait())return 1;}
 struct Request{std::shared_ptr<Event> event;unsigned value=0;};
 std::array<std::atomic<Request*>,32> slots{};
 std::array<std::thread,32> callers;
 std::atomic<unsigned> finished{0},errors{0};
 uint64_t wakes=0;
 for(unsigned id=0;id<callers.size();++id)callers[id]=std::thread([&,id]{
  auto event=std::make_shared<Event>();
  for(unsigned round=0;round<5000;++round){
   event->Reset();Request request{event};slots[id].store(&request,std::memory_order_release);
   if(round%19==0)std::this_thread::yield();
   event->Wait();if(request.value!=round+1)++errors;
   // request is destroyed immediately; caller may Reset before Notify returns.
  }
  ++finished;
 });
 std::array<unsigned,32> rounds{};
 while(finished.load()!=callers.size())for(unsigned id=0;id<slots.size();++id){
  auto* request=slots[id].exchange(nullptr,std::memory_order_acq_rel);
  if(!request)continue;
  auto event=request->event;request->value=++rounds[id];
  wakes+=event->Notify(); // no further request access; retained event owns lifetime.
 }
 for(auto& caller:callers)caller.join();
 // Deliberately release caller ownership immediately after a delayed wake.
 for(unsigned i=0;i<100;++i){
  auto event=std::make_shared<Event>();event->Reset();
  std::thread notifier([copy=event]{std::this_thread::sleep_for(std::chrono::microseconds(20));copy->Notify();});
  event->Wait();event.reset();notifier.join();
 }
 if(errors.load())return 2;
 std::cout<<"PASS before_wait=10000 reusable_rounds=160000 lifetime=100 wakes="<<wakes<<std::endl;
}
