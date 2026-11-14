"""Instrument only a disposable generated F2 source copy, never engine source."""
from pathlib import Path
import sys
p=Path(sys.argv[1]);core=p/'engine/faster-src/core'
header=core/'f2_phase_profile.h'
header.write_text(r'''#pragma once
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
''')
f=core/'f2_sync_experiment.h';f.write_text(f.read_text().replace('#pragma once','#pragma once\n#include "f2_phase_profile.h"'))
f=core/'f2_sync_members.inc';t=f.read_text().replace('    std::unique_lock<std::mutex> request(s.mutex);','''    phase_profile::Scope total(phase_profile::Total);
    auto queue_started=phase_profile::Clock::now();
    std::unique_lock<std::mutex> request(s.mutex);
    phase_profile::Record(phase_profile::Queue,queue_started);''')
t=t.replace('      gate.lock();\n      StartSession();session=true;','''      auto gate_started=phase_profile::Clock::now();gate.lock();phase_profile::Record(phase_profile::Gate,gate_started);
      auto session_started=phase_profile::Clock::now();
      StartSession();session=true;''')
t=t.replace('      const auto status=native(serial);','''      phase_profile::Record(phase_profile::Session,session_started);
      const auto status=[&]{phase_profile::Scope timing(phase_profile::Apply);return native(serial);}();''')
t=t.replace('      hot_store.PrefixPublishIndex();','      auto publish_started=phase_profile::Clock::now();\n      hot_store.PrefixPublishIndex();')
t=t.replace('      StopSession();session=false;\n      const bool needs_checkpoint','      StopSession();session=false;phase_profile::Record(phase_profile::Publish,publish_started);\n      const bool needs_checkpoint')
t=t.replace('  void SyncCheckpointAndReclaim() {','  void SyncCheckpointAndReclaim() {\n    phase_profile::Scope profile(phase_profile::Checkpoint);')
t=t.replace('  void SyncClose() {','  void SyncClose() {\n    phase_profile::Print();')
f.write_text(t)
f=core/'f2_prefix_members.inc';t=f.read_text().replace('  void PrefixCommit(uint64_t first,uint64_t last,uint64_t count) {','  void PrefixCommit(uint64_t first,uint64_t last,uint64_t count) {\n    auto seal_started=phase_profile::Clock::now();')
t=t.replace('    Address boundary=hot_store.hlog.ShiftReadOnlyToTail();', '    phase_profile::Record(phase_profile::Seal,seal_started);\n    auto flush_started=phase_profile::Clock::now();\n    Address boundary=hot_store.hlog.ShiftReadOnlyToTail();')
t=t.replace('    if(inject)experimental::Fault("before-data-sync");','    phase_profile::Record(phase_profile::Flush,flush_started);\n    if(inject)experimental::Fault("before-data-sync");')
t=t.replace('    PrefixSyncDataFiles();','    {phase_profile::Scope profile(phase_profile::Sync);PrefixSyncDataFiles();}')
f.write_text(t)
print('Only generated sources instrumented',core)
