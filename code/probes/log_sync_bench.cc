#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Args {
  std::string dir;
  std::string output;
  uint64_t ops = 2000;
  uint64_t value_bytes = 128;
  uint64_t repetitions = 3;
};

void die(const std::string& message) {
  std::cerr << message << ": " << std::strerror(errno) << "\n";
  std::exit(2);
}

void write_all(int fd, const void* data, size_t bytes) {
  const char* cursor = static_cast<const char*>(data);
  while (bytes != 0) {
    const ssize_t written = ::write(fd, cursor, bytes);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) die("write");
    cursor += written;
    bytes -= static_cast<size_t>(written);
  }
}

void reset_file(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error) die("remove " + path.string());
}

uint64_t ns_since(std::chrono::steady_clock::time_point start) {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - start).count());
}

void append_record(int fd, uint64_t id, size_t value_bytes) {
  std::vector<char> record(16 + value_bytes);
  std::memcpy(record.data(), &id, sizeof(id));
  const uint64_t marker = id ^ 0x9e3779b97f4a7c15ULL;
  std::memcpy(record.data() + sizeof(id), &marker, sizeof(marker));
  for (size_t i = 16; i < record.size(); ++i) {
    record[i] = static_cast<char>((id + i * 131) & 0xff);
  }
  write_all(fd, record.data(), record.size());
}

void open_file(const std::filesystem::path& path, int& fd) {
  fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0) die("open " + path.string());
}

void run_case(const Args& args, const std::string& mode, uint64_t batch,
              uint64_t repetition, std::ofstream& csv) {
  const std::filesystem::path root(args.dir);
  std::filesystem::create_directories(root);
  reset_file(root / "data.log");
  reset_file(root / "wal.log");
  int data_fd = -1;
  int wal_fd = -1;
  open_file(root / "data.log", data_fd);
  if (mode == "dual") open_file(root / "wal.log", wal_fd);

  uint64_t syncs = 0;
  uint64_t operations_since_sync = 0;
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t id = 0; id < args.ops; ++id) {
    append_record(data_fd, id, args.value_bytes);
    if (mode == "dual") {
      append_record(wal_fd, id, 32);
    }
    ++operations_since_sync;
    if (operations_since_sync == batch || id + 1 == args.ops) {
      if (::fdatasync(data_fd) != 0) die("fdatasync data");
      if (mode == "dual" && ::fdatasync(wal_fd) != 0) die("fdatasync wal");
      ++syncs;
      operations_since_sync = 0;
    }
  }
  const uint64_t elapsed_ns = ns_since(start);
  if (::close(data_fd) != 0) die("close data");
  if (wal_fd >= 0 && ::close(wal_fd) != 0) die("close wal");
  csv << mode << ',' << batch << ',' << repetition << ',' << args.ops << ','
      << syncs << ',' << elapsed_ns << ','
      << std::fixed << std::setprecision(2)
      << (static_cast<double>(args.ops) * 1e9 / elapsed_ns) << '\n';
}

Args parse(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string flag(argv[i]);
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) die(std::string("missing value for ") + name);
      return argv[++i];
    };
    if (flag == "--dir") args.dir = next("--dir");
    else if (flag == "--output") args.output = next("--output");
    else if (flag == "--ops") args.ops = std::stoull(next("--ops"));
    else if (flag == "--value-bytes") args.value_bytes = std::stoull(next("--value-bytes"));
    else if (flag == "--repetitions") args.repetitions = std::stoull(next("--repetitions"));
    else die("unknown option " + flag);
  }
  if (args.dir.empty() || args.output.empty() || args.ops == 0 || args.repetitions == 0) {
    die("--dir, --output, --ops and --repetitions are required");
  }
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = parse(argc, argv);
  std::filesystem::create_directories(std::filesystem::path(args.output).parent_path());
  std::ofstream csv(args.output, std::ios::trunc);
  if (!csv) die("open output");
  csv << "mode,batch,repetition,operations,syncs,elapsed_ns,ops_per_sec\n";
  const std::vector<uint64_t> batches = {1, 4, 16, 64};
  for (uint64_t repetition = 0; repetition < args.repetitions; ++repetition) {
    for (uint64_t batch : batches) run_case(args, "single", batch, repetition, csv);
    for (uint64_t batch : batches) run_case(args, "dual", batch, repetition, csv);
  }
  std::cout << "wrote " << args.output << "\n";
}
