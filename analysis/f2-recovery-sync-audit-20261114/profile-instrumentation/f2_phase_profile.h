#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
namespace FASTER { namespace core { namespace phase_profile {
using Clock=std::chrono::steady_clock;
enum Phase{Queue,Gate,Session,Apply,Seal,Flush,Sync,Publish,Checkpoint,Total,Count};
inline std::array<std::atomic<unsigned long long>,Count> ns{},calls{};
inline void Record(Phase id,Clock::time_point start){ns[id]+=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();++calls[id];}
struct Scope{Phase id;Clock::time_point start=Clock::now();explicit Scope(Phase p):id(p){}~Scope(){Record(id,start);}};
inline void Print(){const char* names[]={"queue","gate","session","apply","seal","flush","sync","publish","checkpoint","total"};for(unsigned i=0;i<Count;++i)std::fprintf(stderr,"F2_PHASE phase=%s calls=%llu ns=%llu\n",names[i],calls[i].load(),ns[i].load());}
}}}
