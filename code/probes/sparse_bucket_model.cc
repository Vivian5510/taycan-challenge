#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t kSlots = 15;
constexpr uint32_t kBucketsPerPage = 31;
constexpr uint64_t kPageBytes = 8192;
constexpr uint64_t kBucketBytes = 256;
constexpr uint64_t kDirectoryEntryBytes = 8;

struct Bucket {
  std::vector<uint64_t> tags;
  std::vector<uint32_t> overflow;
  Bucket() : tags(kSlots, 0) {}
};

uint64_t mix(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

uint64_t ceil_div(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

void insert(Bucket& bucket, uint64_t tag, std::vector<Bucket>& overflow,
            uint64_t& max_chain, uint64_t& overflow_buckets) {
  Bucket* current = &bucket;
  uint64_t chain = 0;
  while (true) {
    for (auto& slot : current->tags) {
      if (slot == 0) {
        slot = tag == 0 ? 1 : tag;
        max_chain = std::max(max_chain, chain + 1);
        return;
      }
    }
    if (current->overflow.empty()) {
      current->overflow.push_back(static_cast<uint32_t>(overflow.size()));
      overflow.emplace_back();
      ++overflow_buckets;
    }
    current = &overflow[current->overflow.front()];
    ++chain;
  }
}

struct Result {
  uint64_t keys;
  double load;
  uint64_t logical_pages;
  uint64_t touched_pages;
  uint64_t overflow_buckets;
  uint64_t max_chain;
  uint64_t page_bytes;
  uint64_t directory_bytes;
  uint64_t overflow_bytes;
  double bytes_per_key;
};

Result run(uint64_t keys, double load, uint64_t seed) {
  const uint64_t slots_per_page = kSlots * kBucketsPerPage;
  const uint64_t logical_pages = ceil_div(keys, static_cast<uint64_t>(slots_per_page * load));
  std::vector<Bucket> pages(logical_pages * kBucketsPerPage);
  std::vector<uint8_t> touched(logical_pages, 0);
  std::vector<Bucket> overflow;
  overflow.reserve(keys / kSlots / 10 + 1);
  uint64_t max_chain = 1;
  uint64_t overflow_buckets = 0;
  for (uint64_t id = 1; id <= keys; ++id) {
    const uint64_t hash = mix(id ^ seed);
    const uint64_t page_id = hash % logical_pages;
    const uint64_t bucket_id = (hash / logical_pages) % kBucketsPerPage;
    touched[page_id] = 1;
    insert(pages[page_id * kBucketsPerPage + bucket_id], hash, overflow,
           max_chain, overflow_buckets);
  }
  uint64_t touched_pages = 0;
  for (auto value : touched) touched_pages += value != 0;
  const uint64_t page_bytes = touched_pages * kPageBytes;
  const uint64_t directory_bytes = logical_pages * kDirectoryEntryBytes;
  const uint64_t overflow_bytes = overflow_buckets * kBucketBytes;
  const uint64_t total = page_bytes + directory_bytes + overflow_bytes;
  return {keys, load, logical_pages, touched_pages, overflow_buckets, max_chain,
          page_bytes, directory_bytes, overflow_bytes,
          static_cast<double>(total) / keys};
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t keys = 1'000'000;
  uint64_t seed = 20261028;
  std::string output;
  for (int i = 1; i < argc; ++i) {
    const std::string flag(argv[i]);
    if (flag == "--keys" && i + 1 < argc) keys = std::stoull(argv[++i]);
    else if (flag == "--seed" && i + 1 < argc) seed = std::stoull(argv[++i]);
    else if (flag == "--output" && i + 1 < argc) output = argv[++i];
    else {
      std::cerr << "usage: --keys N --output FILE [--seed N]\n";
      return 2;
    }
  }
  std::ofstream csv(output, std::ios::trunc);
  if (!csv) {
    std::cerr << "cannot open output\n";
    return 2;
  }
  csv << "keys,load,logical_pages,touched_pages,overflow_buckets,max_chain,"
         "page_bytes,directory_bytes,overflow_bytes,bytes_per_key\n";
  for (double load : {0.5, 0.8, 0.95}) {
    const auto result = run(keys, load, seed);
    csv << result.keys << ',' << result.load << ',' << result.logical_pages << ','
        << result.touched_pages << ',' << result.overflow_buckets << ','
        << result.max_chain << ',' << result.page_bytes << ','
        << result.directory_bytes << ',' << result.overflow_bytes << ','
        << std::fixed << std::setprecision(3) << result.bytes_per_key << '\n';
    std::cout << "keys=" << result.keys << " load=" << result.load
              << " touched_pages=" << result.touched_pages
              << " logical_pages=" << result.logical_pages
              << " overflow_buckets=" << result.overflow_buckets
              << " max_chain=" << result.max_chain
              << " bytes_per_key=" << result.bytes_per_key << '\n';
  }
}
