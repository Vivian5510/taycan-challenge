// Opt-in native HybridLog commit experiment. Format v1: a standard invalid
// F2 record with an empty internal key seals a checksummed group at 4KiB.
#pragma once
#include <cstddef>
#include <set>
#include "util/crc32c.h"

namespace FASTER { namespace core { namespace experimental {
inline constexpr uint64_t kPrefixBlock=F2_PREFIX_BLOCK_BYTES;
inline constexpr uint64_t kPrefixSegment=1ULL<<
#ifdef F2_DURABLE_COMPACTION
  F2_DATA_SEGMENT_BITS;
#else
  30;
#endif
inline constexpr uint64_t kPrefixMagic=0x31474f4c32464350ULL;
inline constexpr uint64_t kPrefixEndMagic=0x454e443246435031ULL;
inline constexpr uint32_t kPrefixTag=0x31465850;
struct PrefixFooter {
  uint64_t magic,from,end,first,last,operations;
  uint32_t data_crc,footer_crc;
  uint64_t end_magic;
};
static_assert(sizeof(PrefixFooter)==64,"fixed native commit footer");
inline uint32_t PrefixFooterCRC(const PrefixFooter& f) {
  uint32_t crc=rocksdb::crc32c::Value(reinterpret_cast<const char*>(&f),offsetof(PrefixFooter,footer_crc));
  return rocksdb::crc32c::Extend(crc,reinterpret_cast<const char*>(&f.end_magic),sizeof(f.end_magic));
}
struct PrefixState {
  uint64_t stable_tail=0,prepared_tail=0,base_tail=0,commits=0,data_syncs=0,directory_syncs=0,recovered_commits=0;
  std::set<std::string> synced_files;
  std::atomic<uint64_t> last_mutation_ns{0};
#ifdef F2_LINUX_DATA_PREFIX
  struct DurabilityJob {
    uint64_t end = 0;
    uint64_t serial = 0;
    std::map<std::string,uint64_t> files;
    bool done = false;
    Status status = Status::Corruption;
    std::mutex mutex;
    std::condition_variable cv;
  };
  enum class Readers { Exclusive, DuringSync, UntilPublish };
  bool prewrite = false;
  Readers readers = Readers::Exclusive;
  uint64_t allocated_segment = UINT64_MAX;
  uint64_t allocated_end = 0;
  uint64_t prewritten_bytes = 0;
  uint64_t data_sync_ns = 0;
  uint64_t apply_ns = 0;
  uint64_t session_start_ns = 0;
  uint64_t index_apply_ns = 0;
  uint64_t record_prepare_ns = 0;
  uint64_t session_complete_ns = 0;
  uint64_t session_stop_ns = 0;
  uint64_t flush_ns = 0;
  uint64_t publish_wait_ns = 0;
#endif
};
inline std::atomic<uint64_t> prefix_write_calls{0},prefix_write_bytes{0};
}}}
