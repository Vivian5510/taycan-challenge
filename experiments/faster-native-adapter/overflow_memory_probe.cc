// Exercise the upstream overflow allocator, with zero keys and no store adapter.
#include "core/async.h"
#include "index/hash_bucket.h"
#include "core/malloc_fixed_page_size.h"

#include <fstream>
#include <iostream>
#include <string>

struct DummyDisk {
  struct file_t {};
};

uint64_t ResidentBytes() {
  std::ifstream status("/proc/self/status");
  std::string label;
  while (status >> label) {
    if (label == "VmRSS:") {
      uint64_t kb;
      status >> kb;
      return kb * 1024;
    }
    std::getline(status, label);
  }
  return 0;
}

int main() {
  using Bucket = FASTER::index::HotLogIndexHashBucket;
  using Page = FASTER::core::FixedPage<Bucket>;
  using Allocator = FASTER::core::MallocFixedPageSize<Bucket, DummyDisk>;
  FASTER::core::LightEpoch epoch;
  const auto baseline = ResidentBytes();
  std::cout << "{\"phase\":\"baseline\",\"rss_bytes\":" << baseline
            << ",\"bucket_bytes\":" << sizeof(Bucket) << ",\"page_bytes\":" << sizeof(Page)
            << ",\"keys\":0}" << std::endl;
  Allocator first;
  first.Initialize(512, epoch);
  const auto one = ResidentBytes();
  std::cout << "{\"phase\":\"one_overflow_allocator\",\"rss_bytes\":" << one
            << ",\"delta_bytes\":" << one - baseline << "}" << std::endl;
  Allocator second;
  second.Initialize(512, epoch);
  const auto two = ResidentBytes();
  std::cout << "{\"phase\":\"two_overflow_allocators\",\"rss_bytes\":" << two
            << ",\"delta_bytes\":" << two - one << "}" << std::endl;
}
