#define _GNU_SOURCE

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <numeric>
#include <string>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr size_t kGroups = 1200;
constexpr size_t kRecords = 32;
constexpr size_t kRecordBytes = 384;
constexpr size_t kPrewriteBytes = 64 << 20;

using Clock = std::chrono::steady_clock;

void WriteAll(int fd, const void* data, size_t size, off_t offset) {
  const auto* bytes = static_cast<const char*>(data);
  while (size) {
    const auto written = ::pwrite(fd, bytes, size, offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) throw std::runtime_error("pwrite failed");
    bytes += written;
    size -= static_cast<size_t>(written);
    offset += written;
  }
}

void Prepare(int fd) {
  std::vector<char> zeros(1 << 20);
  for (off_t offset = 0; offset < static_cast<off_t>(kPrewriteBytes); offset += zeros.size())
    WriteAll(fd, zeros.data(), zeros.size(), offset);
  if (::fdatasync(fd) != 0) throw std::runtime_error("prewrite fdatasync failed");
}

std::vector<double> Run(const std::string& mode, const std::string& path) {
  int flags = O_CREAT | O_RDWR | O_CLOEXEC;
  if (mode == "odsync") flags |= O_DSYNC;
  const int fd = ::open(path.c_str(), flags, 0600);
  if (fd < 0) throw std::runtime_error("open failed");
  Prepare(fd);
  std::vector<char> data(kRecords * kRecordBytes, 0x5a);
  std::vector<struct iovec> vectors(kRecords);
  for (size_t i = 0; i < kRecords; ++i) {
    vectors[i].iov_base = data.data() + i * kRecordBytes;
    vectors[i].iov_len = kRecordBytes;
  }
  std::vector<double> samples;
  samples.reserve(kGroups);
  for (size_t group = 0; group < kGroups + 20; ++group) {
    const off_t offset = static_cast<off_t>((group * kRecords * kRecordBytes) %
                                             (kPrewriteBytes - data.size()));
    const auto begin = Clock::now();
    ssize_t result;
    if (mode == "rwfdsync") {
      result = ::pwritev2(fd, vectors.data(), static_cast<int>(vectors.size()), offset, RWF_DSYNC);
    } else {
      result = ::pwrite(fd, data.data(), data.size(), offset);
    }
    if (result != static_cast<ssize_t>(data.size())) throw std::runtime_error("data write failed");
    if (mode == "fdatasync" && ::fdatasync(fd) != 0) throw std::runtime_error("fdatasync failed");
    const auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
    if (group >= 20) samples.push_back(elapsed);
  }
  ::close(fd);
  std::sort(samples.begin(), samples.end());
  const double mean = std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
  const auto percentile = [&](size_t p) {
    return samples[(p * samples.size() + 99) / 100 - 1];
  };
  return {mean, percentile(50), percentile(99), kRecords * 1e6 / mean};
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  try {
    for (const auto& mode : {std::string("fdatasync"), std::string("odsync"), std::string("rwfdsync")}) {
      const std::string path = std::string(argv[1]) + "/" + mode;
      auto values = Run(mode, path);
      std::cout << "{\"mode\":\"" << mode << "\",\"mean_us\":" << values[0]
                << ",\"p50_us\":" << values[1] << ",\"p99_us\":" << values[2]
                << ",\"ops_per_second\":" << values[3] << "}" << std::endl;
      ::unlink(path.c_str());
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    return 1;
  }
}
