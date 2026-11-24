// A restored committed prefix must satisfy a checkpoint even with no new writes.
#include "core/persistent_memory_malloc.h"
#include "device/null_disk.h"
#include <iostream>

int main() {
  using namespace FASTER::core;
  using Disk = FASTER::device::NullDisk;
  LightEpoch epoch;
  Disk disk("", epoch, "");
  const uint64_t page = PersistentMemoryMalloc<Disk>::kPageSize;
  PersistentMemoryMalloc<Disk> log(true, 6 * page, epoch, disk, disk.log(),
                                 1.0/3.0, false);
  const Address begin{0, 64};
  const Address recovered_tail{0, 512};
  log.RecoveryReset(begin, begin, recovered_tail);
  const auto checkpoint_target = log.ShiftReadOnlyToTail();
  const auto flushed = log.flushed_until_address.load();
  const bool ready = flushed >= checkpoint_target;
  std::cout << "{\"status\":\"" << (ready ? "PASS" : "FAIL")
            << "\",\"recovered_tail\":" << recovered_tail.control()
            << ",\"read_only\":" << log.read_only_address.load().control()
            << ",\"flushed_until\":" << flushed.control()
            << ",\"checkpoint_target\":" << checkpoint_target.control()
            << ",\"new_mutations\":0}" << std::endl;
  return ready ? 0 : 1;
}
