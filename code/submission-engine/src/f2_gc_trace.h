#pragma once
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace FASTER { namespace core { namespace experimental {
inline std::mutex gc_trace_mutex;
inline void GcTrace(const std::string& fields) {
  const char* path=std::getenv("F2_GC_TRACE");
  if(!path)return;
  std::lock_guard<std::mutex> guard(gc_trace_mutex);
  std::ofstream output(path,std::ios::app);
  output << "{\"pid\":" << ::getpid() << "," << fields << "}\n";
  output.flush();
  if(!output)throw std::runtime_error("write GC evidence failed");
}
inline void GcPhase(const char* phase,uint64_t serial,uint64_t cycle) {
  std::ostringstream fields;
  fields << "\"event\":\"phase\",\"phase\":" << std::quoted(phase)
         << ",\"serial\":" << serial << ",\"cycle\":" << cycle;
  GcTrace(fields.str());
  const char* requested=std::getenv("F2_GC_PAUSE_PHASE");
  if(requested && std::string(requested)==phase) {
    // A test controller, rather than the engine, must perform the SIGKILL.
    if(::raise(SIGSTOP))throw std::runtime_error("GC phase pause failed");
  }
}
inline void GcDeleted(const std::string& path,uint64_t bytes) {
  std::ostringstream fields;
  fields << "\"event\":\"segment_deleted\",\"path\":" << std::quoted(path)
         << ",\"bytes\":" << bytes;
  GcTrace(fields.str());
}
}}}
