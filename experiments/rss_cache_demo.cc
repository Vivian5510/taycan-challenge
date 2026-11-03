#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Args {
  std::string mode;
  std::string file;
  uint64_t frames = 8192;
  uint64_t page_bytes = 8192;
};

[[noreturn]] void die(const std::string& message) {
  std::cerr << message << ": " << std::strerror(errno) << "\n";
  std::exit(2);
}

uint64_t rss_bytes() {
  std::ifstream input("/proc/self/status");
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("VmRSS:", 0) != 0) continue;
    const auto first_digit = line.find_first_of("0123456789");
    if (first_digit == std::string::npos) return 0;
    return std::stoull(line.substr(first_digit)) * 1024;
  }
  return 0;
}

Args parse(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string flag(argv[i]);
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) die(std::string("missing value for ") + name);
      return argv[++i];
    };
    if (flag == "--mode") args.mode = next("--mode");
    else if (flag == "--file") args.file = next("--file");
    else if (flag == "--frames") args.frames = std::stoull(next("--frames"));
    else if (flag == "--page-bytes") args.page_bytes = std::stoull(next("--page-bytes"));
    else die("unknown option " + flag);
  }
  if ((args.mode != "heap" && args.mode != "mmap" && args.mode != "pread") ||
      args.file.empty() || args.frames == 0 || args.page_bytes == 0) {
    die("use --mode heap|mmap|pread, --file, --frames and --page-bytes");
  }
  return args;
}

void create_file(const std::string& path, uint64_t bytes) {
  int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
  if (fd < 0) die("open file");
  if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) die("ftruncate");
  std::vector<char> page(4096, 'x');
  for (uint64_t offset = 0; offset < bytes; offset += page.size()) {
    const size_t length = static_cast<size_t>(std::min<uint64_t>(page.size(), bytes - offset));
    if (pwrite(fd, page.data(), length, static_cast<off_t>(offset)) != static_cast<ssize_t>(length)) {
      die("pwrite file");
    }
  }
  if (fsync(fd) != 0) die("fsync file");
  if (close(fd) != 0) die("close file");
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = parse(argc, argv);
  const uint64_t bytes = args.frames * args.page_bytes;
  std::filesystem::create_directories(std::filesystem::path(args.file).parent_path());
  create_file(args.file, bytes);
  std::cout << "mode=" << args.mode << " target_bytes=" << bytes
            << " rss_before=" << rss_bytes() << '\n';

  volatile unsigned char checksum = 0;
  int fd = ::open(args.file.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) die("open read");
  if (args.mode == "heap") {
    std::vector<std::unique_ptr<char[]>> frames;
    frames.reserve(args.frames);
    for (uint64_t i = 0; i < args.frames; ++i) {
      auto frame = std::make_unique<char[]>(args.page_bytes);
      frame[i % args.page_bytes] = static_cast<char>(i);
      checksum ^= static_cast<unsigned char>(frame[i % args.page_bytes]);
      frames.push_back(std::move(frame));
    }
    std::cout << "rss_after=" << rss_bytes() << '\n';
  } else if (args.mode == "mmap") {
    void* mapped = mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) die("mmap");
    char* data = static_cast<char*>(mapped);
    for (uint64_t i = 0; i < args.frames; ++i) {
      checksum ^= static_cast<unsigned char>(data[i * args.page_bytes]);
    }
    std::cout << "rss_after=" << rss_bytes() << '\n';
    if (munmap(mapped, bytes) != 0) die("munmap");
  } else {
    std::vector<char> frame(args.page_bytes);
    for (uint64_t i = 0; i < args.frames; ++i) {
      if (pread(fd, frame.data(), frame.size(), static_cast<off_t>(i * args.page_bytes)) !=
          static_cast<ssize_t>(frame.size())) {
        die("pread");
      }
      checksum ^= static_cast<unsigned char>(frame[i % frame.size()]);
    }
    std::cout << "rss_after=" << rss_bytes() << '\n';
  }
  if (close(fd) != 0) die("close read");
  std::cout << "checksum=" << static_cast<unsigned>(checksum) << '\n';
}
