#include "kv_engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef TAYCAN_BENCH_ENGINE_ID
#define TAYCAN_BENCH_ENGINE_ID "contestant_engine"
#define TAYCAN_BENCH_ENGINE_VERSION "workspace"
#define TAYCAN_BENCH_ENGINE_POLICY "KVEngine synchronous durable acknowledgement; VmRSS monitored"
#endif

#if defined(TAYCAN_BENCH_PLATFORM_SYNC)
constexpr const char* kPlatformSync = TAYCAN_BENCH_PLATFORM_SYNC;
#elif defined(__APPLE__)
constexpr const char* kPlatformSync = "macOS: contestant Sync uses F_FULLFSYNC; verify baseline implementation separately";
#else
constexpr const char* kPlatformSync = "Linux: contestant Sync uses fdatasync; verify baseline implementation separately";
#endif

namespace {
using Clock = std::chrono::steady_clock;
// 64 sub-buckets per power of two: percentile upper bounds have <1.563% error.
struct Histogram {
  std::array<uint64_t, 4096> bins{};
  uint64_t count = 0, maximum = 0;
  void Add(uint64_t ns) {
    ns = std::max<uint64_t>(ns, 1);
    const unsigned exponent = 63 - unsigned(__builtin_clzll(ns));
    const uint64_t base = uint64_t(1) << exponent;
    const unsigned sub = exponent >= 6 ? unsigned((ns - base) >> (exponent - 6))
                                       : unsigned((ns - base) << (6 - exponent));
    ++bins[exponent * 64 + sub];
    ++count;
    maximum = std::max(maximum, ns);
  }
  void Merge(const Histogram& other) {
    for (size_t i = 0; i < bins.size(); ++i) bins[i] += other.bins[i];
    count += other.count;
    maximum = std::max(maximum, other.maximum);
  }
  double Quantile(double fraction) const {
    const uint64_t target = uint64_t(std::ceil(fraction * double(count)));
    uint64_t seen = 0;
    for (size_t i = 0; i < bins.size(); ++i) {
      seen += bins[i];
      if (count && seen >= target) {
        const unsigned exponent = unsigned(i / 64), sub = unsigned(i % 64);
        const long double base = std::ldexp(1.L, int(exponent));
        const long double upper = exponent >= 6 ? base + std::ldexp(static_cast<long double>(sub + 1), int(exponent) - 6) - 1
                                                : base + std::floor(static_cast<long double>(sub) * base / 64);
        return double(std::min<long double>(upper, maximum)) / 1000.;
      }
    }
    return 0;
  }
};

struct Options {
  std::string data_dir, workload = "read", distribution = "uniform";
  bool prepared_dataset = false;
  uint64_t num_keys = 10000, mem_limit_bytes = 256ULL << 20, seed = 20261021;
  uint64_t sample_every = 0;
  uint32_t key_size = 64, value_size = 128, threads = 1;
  uint32_t duration_seconds = 15, warmup_seconds = 2;
  uint32_t progress_interval_seconds = 1;
};

uint64_t Number(const std::string& name, const std::string& text) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
    throw std::runtime_error("invalid unsigned integer for " + name);
  errno = 0;
  char* end = nullptr;
  const auto value = std::strtoull(text.c_str(), &end, 10);
  if (errno == ERANGE || *end) throw std::runtime_error("integer overflow for " + name);
  return value;
}
Options Parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--describe") {
      std::cout << "{\"engine\":\"" << TAYCAN_BENCH_ENGINE_ID
                << "\",\"version\":\"" << TAYCAN_BENCH_ENGINE_VERSION
                << "\",\"policy\":\"" << TAYCAN_BENCH_ENGINE_POLICY
                << "\",\"platform_sync\":\"" << kPlatformSync << "\"}" << std::endl;
      std::exit(0);
    }
    if (arg == "--prepared-dataset") {
      o.prepared_dataset = true;
      continue;
    }
    if (arg == "--help") {
      std::cout << "--data-dir=PATH --workload=load|read|update|ycsb_a|contest_mix|latency_80_20|recovery\n"
                   "--num-keys=N --threads=N --duration-seconds=N --warmup-seconds=N\n"
                   "--key-size=N --value-size=N --mem-limit-bytes=N --seed=N\n"
                   "--sample-every=N (0 disables latency collection) --distribution=uniform|hotset\n"
                   "--progress-interval-seconds=N (0 disables live operation counters)\n";
      std::exit(0);
    }
    const auto split = arg.find('=');
    if (arg.rfind("--", 0) || split == std::string::npos)
      throw std::runtime_error("expected --name=value");
    const auto name = arg.substr(2, split - 2), value = arg.substr(split + 1);
    if (name == "data-dir") o.data_dir = value;
    else if (name == "workload") o.workload = value;
    else if (name == "distribution") o.distribution = value;
    else {
      const auto n = Number(name, value);
      auto small = [&] {
        if (n > UINT32_MAX) throw std::runtime_error("integer overflow for " + name);
        return uint32_t(n);
      };
      if (name == "num-keys") o.num_keys = n;
      else if (name == "mem-limit-bytes") o.mem_limit_bytes = n;
      else if (name == "seed") o.seed = n;
      else if (name == "sample-every") o.sample_every = n;
      else if (name == "key-size") o.key_size = small();
      else if (name == "value-size") o.value_size = small();
      else if (name == "threads") o.threads = small();
      else if (name == "duration-seconds") o.duration_seconds = small();
      else if (name == "warmup-seconds") o.warmup_seconds = small();
      else if (name == "progress-interval-seconds") o.progress_interval_seconds = small();
      else throw std::runtime_error("unknown option: " + name);
    }
  }
  const std::vector<std::string> workloads = {"load", "read", "update", "ycsb_a", "contest_mix", "latency_80_20", "recovery"};
  if (std::find(workloads.begin(), workloads.end(), o.workload) == workloads.end())
    throw std::runtime_error("unknown workload");
  if (o.data_dir.empty() || !o.num_keys || o.num_keys > 200000000 || !o.threads || o.threads > 128 ||
      o.key_size < 8 || o.key_size > (1U << 20) || o.value_size < 16 || o.value_size > (16U << 20) ||
      o.mem_limit_bytes < (16ULL << 20) || !o.duration_seconds || o.duration_seconds > 86400 ||
      o.warmup_seconds > 3600 || (o.distribution != "uniform" && o.distribution != "hotset"))
    throw std::runtime_error("invalid count, size, duration, distribution, or memory budget");
  const bool data_exists = std::filesystem::exists(o.data_dir);
  if (o.workload == "recovery") {
    if (!data_exists || std::filesystem::is_empty(o.data_dir))
      throw std::runtime_error("recovery workload requires an existing prepared database");
    if (o.num_keys < 100) throw std::runtime_error("recovery workload requires at least 100 keys");
  } else if (o.prepared_dataset) {
    if (o.workload == "load" || !data_exists || std::filesystem::is_empty(o.data_dir))
      throw std::runtime_error("prepared-dataset requires an existing populated database and a non-load workload");
  } else if (data_exists && !std::filesystem::is_empty(o.data_dir)) {
    throw std::runtime_error("benchmark requires a fresh empty data directory");
  }
  return o;
}

uint64_t Mix(uint64_t x) {
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}
uint64_t Next(uint64_t* state) { *state += 0x9e3779b97f4a7c15ULL; return Mix(*state); }
void Store(std::string& s, uint64_t x, size_t at) {
  for (size_t i = 0; i < 8; ++i) s[at + i] = char(x >> (8 * i));
}
uint64_t ReadWord(const std::string& s, size_t at) {
  uint64_t x = 0;
  for (size_t i = 0; i < 8; ++i) x |= uint64_t(static_cast<unsigned char>(s[at + i])) << (8 * i);
  return x;
}
void Fill(std::string& s, uint64_t seed, size_t at) {
  while (at < s.size()) {
    const auto word = Next(&seed);
    for (unsigned i = 0; i < 8 && at < s.size(); ++i, ++at) s[at] = char(word >> (8 * i));
  }
}
void Key(std::string& key, uint64_t id, uint64_t seed) {
  for (size_t i = 0; i < 8; ++i) key[i] = char(id >> (8 * (7 - i)));
  Fill(key, Mix(id ^ seed), 8);
}
void Value(std::string& value, uint64_t id, uint64_t version, uint64_t seed) {
  Store(value, id, 0);
  Store(value, version, 8);
  Fill(value, Mix(id ^ seed ^ version), 16);
}

std::string Escape(const std::string& s) {
  const char hex[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
    else out += char(c);
  }
  return out;
}
void Progress(const std::string& stage) {
  std::cout << "{\"type\":\"stage\",\"stage\":\"" << stage << "\"}" << std::endl;
}

struct Worker {
  uint64_t operations = 0;
  std::array<uint64_t, 3> counts{}, misses{};
  std::array<Histogram, 3> latency;
  std::string key, value, output, error;
  explicit Worker(const Options& o) : key(o.key_size, '\0'), value(o.value_size, '\0') {
    output.reserve(o.value_size);
  }
};
struct Run {
  double seconds = 0;
  std::vector<Worker> workers;
};

unsigned Operation(const Options& o, uint64_t* rng) {
  if (o.workload == "read") return 0;
  if (o.workload == "load" || o.workload == "update") return 1;
  const auto p = Next(rng) % 100;
  if (o.workload == "ycsb_a") return p < 50 ? 0 : 1;
  if (o.workload == "latency_80_20") return p < 80 ? 0 : 1;
  return p < 60 ? 0 : (p < 90 ? 1 : 2);
}

struct alignas(64) LiveCounter {
  std::atomic<uint64_t> completed{0};
};

Run Execute(KVEngine& engine, const Options& o, const char* stage, bool warmup = false) {
  Run run;
  run.workers.reserve(o.threads);
  for (unsigned t = 0; t < o.threads; ++t) run.workers.emplace_back(o);
  std::mutex gate;
  std::condition_variable cv;
  unsigned ready = 0, finished = 0;
  Clock::time_point finished_at;
  auto live = std::make_unique<LiveCounter[]>(o.threads);
  bool go = false;
  std::atomic<bool> stop{false};
  Clock::time_point deadline;
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < o.threads; ++t) {
    threads.emplace_back([&, t] {
      auto& w = run.workers[t];
      {
        std::unique_lock<std::mutex> lock(gate);
        ++ready;
        cv.notify_all();
        cv.wait(lock, [&] { return go; });
      }
      try {
        uint64_t rng = o.seed ^ Mix(t + 1) ^ (warmup ? 0x12345678 : 0);
        uint64_t version = warmup ? 2 : (1ULL << 32);
        uint64_t load_id = t;
        while (!stop.load(std::memory_order_relaxed)) {
          uint64_t id;
          if (o.workload == "load") {
            if (load_id >= o.num_keys) break;
            id = load_id;
            load_id += o.threads;
          } else {
            if (Clock::now() >= deadline) break;
            uint64_t range = o.num_keys;
            if (o.distribution == "hotset" && Next(&rng) % 100 < 90)
              range = std::max<uint64_t>(1, o.num_keys / 100);
            id = Next(&rng) % range;
          }
          const auto op = Operation(o, &rng);
          Key(w.key, id, o.seed);
          if (op == 1) Value(w.value, id, o.workload == "load" ? 1 : version++, o.seed);
          // Independent sampling choice, avoiding correlation with workload periodicity.
          const bool sample = !warmup && o.sample_every && Mix(w.operations ^ Mix(t + o.seed)) % o.sample_every == 0;
          const auto begin = sample ? Clock::now() : Clock::time_point{};
          auto status = op == 0 ? engine.Get(w.key, &w.output)
                       : op == 1 ? engine.Put(w.key, w.value) : engine.Delete(w.key);
          const auto end = sample ? Clock::now() : Clock::time_point{};
          if (op == 0 && status.IsNotFound() && o.workload == "contest_mix") ++w.misses[op];
          else if (!status.IsOk()) throw std::runtime_error("API failure: " + status.ToString());
          else if (op == 0) {
            if (w.output.size() != o.value_size || ReadWord(w.output, 0) != id)
              throw std::runtime_error("Get returned corrupt or incorrect Key/Value");
            // Check all Value bytes outside the operation-latency interval.
            Value(w.value, id, ReadWord(w.output, 8), o.seed);
            if (w.output != w.value) throw std::runtime_error("Get returned corrupt Value bytes");
          }
          if (sample) w.latency[op].Add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()));
          ++w.counts[op];
          ++w.operations;
          // Independent cache lines, relaxed stores: no per-operation console
          // I/O or shared fetch_add in the benchmark workers.
          if (o.progress_interval_seconds)
            live[t].completed.store(w.operations, std::memory_order_relaxed);
        }
      } catch (const std::exception& e) {
        w.error = e.what();
        stop.store(true);
      }
      {
        std::lock_guard<std::mutex> lock(gate);
        finished_at = Clock::now();
        ++finished;
      }
      cv.notify_all();
    });
  }
  std::unique_lock<std::mutex> lock(gate);
  cv.wait(lock, [&] { return ready == o.threads; });
  const auto begin = Clock::now();
  deadline = begin + std::chrono::seconds(warmup ? o.warmup_seconds : o.duration_seconds);
  go = true;
  lock.unlock();
  cv.notify_all();
  uint64_t previous_count = 0;
  auto previous_at = begin;
  auto print_progress = [&](Clock::time_point now, uint64_t count, bool final) {
    const double elapsed = std::chrono::duration<double>(now - begin).count();
    const double interval = std::chrono::duration<double>(now - previous_at).count();
    std::cout << std::setprecision(12)
              << "{\"type\":\"progress\",\"stage\":\"" << stage
              << "\",\"completed\":" << count << ",\"elapsed_seconds\":" << elapsed
              << ",\"interval_seconds\":" << interval
              << ",\"ops_per_second\":" << (interval > 0 ? (count - previous_count) / interval : 0)
              << ",\"final\":" << (final ? "true" : "false");
    if (o.workload == "load") std::cout << ",\"total\":" << o.num_keys;
    std::cout << "}" << std::endl;
    previous_count = count;
    previous_at = now;
  };
  lock.lock();
  while (finished != o.threads) {
    if (!o.progress_interval_seconds) {
      cv.wait(lock, [&] { return finished == o.threads; });
    } else if (!cv.wait_until(lock, previous_at + std::chrono::seconds(o.progress_interval_seconds),
                              [&] { return finished == o.threads; })) {
      uint64_t count = 0;
      for (unsigned t = 0; t < o.threads; ++t)
        count += live[t].completed.load(std::memory_order_relaxed);
      const auto now = Clock::now();
      lock.unlock();
      print_progress(now, count, false);
      lock.lock();
    }
  }
  const auto end = finished_at;
  lock.unlock();
  for (auto& thread : threads) thread.join();
  run.seconds = std::chrono::duration<double>(end - begin).count();
  if (o.progress_interval_seconds) {
    uint64_t count = 0;
    for (const auto& worker : run.workers) count += worker.operations;
    print_progress(end, count, true);
  }
  return run;
}

uint64_t DirectoryBytes(const std::string& directory) {
  uint64_t bytes = 0;
  for (const auto& item : std::filesystem::recursive_directory_iterator(directory)) {
    std::error_code error;
    if (item.is_regular_file(error)) {
      const auto size = item.file_size(error);
      if (!error) bytes += size;
    }
  }
  return bytes;
}
std::array<uint64_t, 2> IOBytes() {
  std::ifstream input("/proc/self/io");
  std::array<uint64_t, 2> result{};
  std::string name;
  uint64_t value;
  while (input >> name >> value) {
    if (name == "read_bytes:") result[0] = value;
    if (name == "write_bytes:") result[1] = value;
  }
  return result;
}
bool HasProcessIOCounters() {
  std::ifstream input("/proc/self/io");
  return bool(input);
}
void PrintHistogram(const Histogram& h) {
  std::cout << "{\"samples\":" << h.count;
  const std::array<const char*, 4> names = {"p50_us", "p95_us", "p99_us", "p999_us"};
  const std::array<double, 4> fractions = {.5, .95, .99, .999};
  for (unsigned i = 0; i < 4; ++i) {
    std::cout << ",\"" << names[i] << "\":";
    if (h.count) std::cout << h.Quantile(fractions[i]); else std::cout << "null";
  }
  std::cout << ",\"max_us\":";
  if (h.count) std::cout << double(h.maximum) / 1000.; else std::cout << "null";
  std::cout << "}";
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const auto o = Parse(argc, argv);
    std::string recovery_key, recovery_value, recovery_expected;
    std::string recovery_output;
    std::array<uint64_t, 2> recovery_io_before{};
    if (o.workload == "recovery") {
      recovery_key.assign(o.key_size, '\0');
      recovery_value.assign(o.value_size, '\0');
      recovery_expected.assign(o.value_size, '\0');
      recovery_output.reserve(o.value_size);
      recovery_io_before = IOBytes();
    }
    Progress("open");
    const auto open_begin = Clock::now();
    KVEngine* raw = nullptr;
    const auto status = KVEngine::Open(o.data_dir, o.mem_limit_bytes, &raw);
    std::unique_ptr<KVEngine> engine(raw);
    if (!status.IsOk() || !engine) throw std::runtime_error("Open failed: " + status.ToString());
    const double open_seconds = std::chrono::duration<double>(Clock::now() - open_begin).count();
    if (o.workload == "recovery") {
      uint64_t gets = 0, puts = 0;
      const auto verify_version = [&](uint64_t id, uint64_t version, const char* phase) {
        Key(recovery_key, id, o.seed);
        recovery_output.clear();
        const auto read_status = engine->Get(recovery_key, &recovery_output);
        if (!read_status.IsOk())
          throw std::runtime_error(std::string("recovery ") + phase + " Get failed at key=" + std::to_string(id));
        Value(recovery_expected, id, version, o.seed);
        if (recovery_output != recovery_expected)
          throw std::runtime_error(std::string("recovery ") + phase + " value mismatch at key=" + std::to_string(id));
        ++gets;
      };
      for (uint64_t id = 0; id < 100; ++id) verify_version(id, 1, "initial");
      for (uint64_t id = 0; id < 100; ++id) {
        Key(recovery_key, id, o.seed);
        Value(recovery_value, id, 2, o.seed);
        const auto write_status = engine->Put(recovery_key, recovery_value);
        if (!write_status.IsOk())
          throw std::runtime_error("recovery Put failed at key=" + std::to_string(id));
        ++puts;
      }
      for (uint64_t id = 0; id < 100; ++id) verify_version(id, 2, "readback");
      const double recovery_seconds = std::chrono::duration<double>(Clock::now() - open_begin).count();
      const auto recovery_io_after = IOBytes();
      const auto recovery_bytes_before_close = DirectoryBytes(o.data_dir);
      Progress("close");
      const auto close_begin = Clock::now();
      engine.reset();
      const double close_seconds = std::chrono::duration<double>(Clock::now() - close_begin).count();
      const auto recovery_bytes_after_close = DirectoryBytes(o.data_dir);
      std::cout << std::setprecision(12)
                << "{\"type\":\"result\",\"status\":\"PASS\",\"error\":\"\",\"operations\":300"
                << ",\"gets\":" << gets << ",\"puts\":" << puts << ",\"deletes\":0,\"read_misses\":0"
                << ",\"ops_per_second\":" << 300 / recovery_seconds
                << ",\"open_seconds\":" << open_seconds << ",\"setup_seconds\":0,\"warmup_seconds\":0"
                << ",\"measured_seconds\":" << recovery_seconds << ",\"recovery_seconds\":" << recovery_seconds
                << ",\"close_seconds\":" << close_seconds
                << ",\"data_bytes_before_measure\":" << recovery_bytes_before_close
                << ",\"data_bytes_after_measure\":" << recovery_bytes_before_close
                << ",\"data_bytes_after_close\":" << recovery_bytes_after_close
                << ",\"measure_read_bytes\":" << recovery_io_after[0] - recovery_io_before[0]
                << ",\"measure_write_bytes\":" << recovery_io_after[1] - recovery_io_before[1]
                << ",\"latency\":{\"samples\":0,\"p50_us\":null,\"p95_us\":null,\"p99_us\":null,\"p999_us\":null,\"max_us\":null}"
                << ",\"get_latency\":{\"samples\":0,\"p50_us\":null,\"p95_us\":null,\"p99_us\":null,\"p999_us\":null,\"max_us\":null}"
                << ",\"put_latency\":{\"samples\":0,\"p50_us\":null,\"p95_us\":null,\"p99_us\":null,\"p999_us\":null,\"max_us\":null}"
                << ",\"delete_latency\":{\"samples\":0,\"p50_us\":null,\"p95_us\":null,\"p99_us\":null,\"p999_us\":null,\"max_us\":null}}"
                << std::endl;
      return 0;
    }
    double setup_seconds = 0, warmup_seconds = 0;
    if (o.workload != "load" && !o.prepared_dataset) {
      Progress("prepare");
      auto load_options = o;
      load_options.workload = "load";
      load_options.sample_every = 0;
      auto setup = Execute(*engine, load_options, "prepare");
      uint64_t loaded = 0;
      for (const auto& w : setup.workers) {
        if (!w.error.empty()) throw std::runtime_error("prepare failed: " + w.error);
        loaded += w.operations;
      }
      if (loaded != o.num_keys) throw std::runtime_error("dataset was not fully loaded");
      setup_seconds = setup.seconds;
    }
    if (o.workload != "load" && o.warmup_seconds) {
      Progress("warmup");
      const auto warm = Execute(*engine, o, "warmup", true);
      for (const auto& w : warm.workers) if (!w.error.empty()) throw std::runtime_error("warmup failed: " + w.error);
      warmup_seconds = warm.seconds;
    }
    const auto before_bytes = DirectoryBytes(o.data_dir);
    const auto before_io = IOBytes();
    Progress("measure");
    const auto run = Execute(*engine, o, "measure");
    const auto after_io = IOBytes();
    const auto after_bytes = DirectoryBytes(o.data_dir);
    uint64_t operations = 0, misses = 0;
    std::array<uint64_t, 3> counts{};
    std::array<Histogram, 3> histograms;
    Histogram aggregate;
    std::string error;
    for (const auto& w : run.workers) {
      operations += w.operations;
      for (unsigned op = 0; op < 3; ++op) {
        counts[op] += w.counts[op];
        misses += w.misses[op];
        histograms[op].Merge(w.latency[op]);
        aggregate.Merge(w.latency[op]);
      }
      if (error.empty()) error = w.error;
    }
    if (!operations && error.empty()) error = "no operations completed";
    if (o.workload == "load" && operations != o.num_keys && error.empty()) error = "incomplete load";
    Progress("close");
    const auto close_begin = Clock::now();
    engine.reset();
    const double close_seconds = std::chrono::duration<double>(Clock::now() - close_begin).count();
    const auto final_bytes = DirectoryBytes(o.data_dir);
    std::cout << std::setprecision(12)
              << "{\"type\":\"result\",\"status\":\"" << (error.empty() ? "PASS" : "FAIL")
              << "\",\"error\":\"" << Escape(error) << "\",\"operations\":" << operations
              << ",\"gets\":" << counts[0] << ",\"puts\":" << counts[1] << ",\"deletes\":" << counts[2]
              << ",\"read_misses\":" << misses << ",\"ops_per_second\":" << operations / run.seconds
              << ",\"open_seconds\":" << open_seconds << ",\"setup_seconds\":" << setup_seconds
              << ",\"warmup_seconds\":" << warmup_seconds << ",\"measured_seconds\":" << run.seconds
              << ",\"close_seconds\":" << close_seconds << ",\"data_bytes_before_measure\":" << before_bytes
              << ",\"data_bytes_after_measure\":" << after_bytes << ",\"data_bytes_after_close\":" << final_bytes
              << ",\"measure_read_bytes\":";
    if (HasProcessIOCounters()) std::cout << after_io[0] - before_io[0];
    else std::cout << "null";
    std::cout << ",\"measure_write_bytes\":";
    if (HasProcessIOCounters()) std::cout << after_io[1] - before_io[1];
    else std::cout << "null";
    std::cout << ",\"latency\":";
    PrintHistogram(aggregate);
    const std::array<const char*, 3> names = {"get_latency", "put_latency", "delete_latency"};
    for (unsigned op = 0; op < 3; ++op) { std::cout << ",\"" << names[op] << "\":"; PrintHistogram(histograms[op]); }
    std::cout << "}" << std::endl;
    return error.empty() ? 0 : 1;
  } catch (const std::exception& e) {
    std::cout << "{\"type\":\"error\",\"error\":\"" << Escape(e.what()) << "\"}" << std::endl;
    return 1;
  }
}
