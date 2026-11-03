#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

struct Args {
  std::string dir;
  std::string output;
  uint64_t file_bytes = 128ULL << 20;
  uint64_t reads = 10000;
  uint64_t repetitions = 3;
};

[[noreturn]] void die(const std::string& message) {
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
    else if (flag == "--file-bytes") args.file_bytes = std::stoull(next("--file-bytes"));
    else if (flag == "--reads") args.reads = std::stoull(next("--reads"));
    else if (flag == "--repetitions") args.repetitions = std::stoull(next("--repetitions"));
    else die("unknown option " + flag);
  }
  if (args.dir.empty() || args.output.empty() || args.file_bytes == 0 ||
      args.reads == 0 || args.repetitions == 0) {
    die("--dir, --output, --file-bytes, --reads and --repetitions are required");
  }
  return args;
}

void create_file(const std::filesystem::path& path, uint64_t bytes) {
  int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0) die("open " + path.string());
  std::vector<char> buffer(1 << 20);
  for (size_t i = 0; i < buffer.size(); ++i) buffer[i] = static_cast<char>(i);
  uint64_t remaining = bytes;
  while (remaining != 0) {
    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
    write_all(fd, buffer.data(), chunk);
    remaining -= chunk;
  }
  if (::fsync(fd) != 0) die("fsync file");
  if (::close(fd) != 0) die("close file");
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = parse(argc, argv);
  const std::filesystem::path root(args.dir);
  std::filesystem::create_directories(root);
  std::filesystem::create_directories(std::filesystem::path(args.output).parent_path());
  std::ofstream csv(args.output, std::ios::trunc);
  if (!csv) die("open output");
  csv << "page_bytes,repetition,reads,logical_bytes_read,elapsed_ns,reads_per_sec\n";

  const std::vector<uint64_t> page_sizes = {4096, 8192, 16384, 32768};
  std::mt19937_64 rng(0x9e3779b97f4a7c15ULL);
  for (uint64_t page_bytes : page_sizes) {
    const auto path = root / ("pages-" + std::to_string(page_bytes) + ".dat");
    create_file(path, args.file_bytes);
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) die("open read " + path.string());
    std::vector<char> buffer(page_bytes);
    const uint64_t page_count = args.file_bytes / page_bytes;
    for (uint64_t repetition = 0; repetition < args.repetitions; ++repetition) {
      if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) != 0) {
        std::cerr << "warning: POSIX_FADV_DONTNEED failed\n";
      }
      std::uniform_int_distribution<uint64_t> distribution(0, page_count - 1);
      volatile unsigned char checksum = 0;
      const auto start = std::chrono::steady_clock::now();
      for (uint64_t read = 0; read < args.reads; ++read) {
        const off_t offset = static_cast<off_t>(distribution(rng) * page_bytes);
        const ssize_t got = ::pread(fd, buffer.data(), buffer.size(), offset);
        if (got != static_cast<ssize_t>(buffer.size())) die("pread");
        checksum ^= static_cast<unsigned char>(buffer[read % buffer.size()]);
      }
      const uint64_t elapsed_ns = static_cast<uint64_t>(std::chrono::duration_cast<
          std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
      csv << page_bytes << ',' << repetition << ',' << args.reads << ','
          << args.reads * page_bytes << ',' << elapsed_ns << ','
          << std::fixed << std::setprecision(2)
          << (static_cast<double>(args.reads) * 1e9 / elapsed_ns) << '\n';
      if (checksum == 0xff) std::cerr << "checksum=" << checksum << '\n';
    }
    if (::close(fd) != 0) die("close read");
  }
  std::cout << "wrote " << args.output << "\n";
}
