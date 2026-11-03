// Owner-thread hooks only. The F2 core does not depend on a fiber runtime.
#pragma once
#include <thread>
namespace FASTER { namespace core { namespace experimental {
inline thread_local void* fiber_owner = nullptr;
inline thread_local void (*fiber_cooperate)(void*) = nullptr;
inline thread_local int (*fiber_sync)(void*, int) = nullptr;
inline void FiberCooperate() {
  if (fiber_cooperate) fiber_cooperate(fiber_owner);
  else std::this_thread::yield();
}
}}}
