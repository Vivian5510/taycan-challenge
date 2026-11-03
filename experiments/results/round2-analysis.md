# Round 2 Results

Date: 2026-10-27

Environment: `taycan-ubuntu24`, Ubuntu 24.04 x86_64 guest, 8 vCPU, 24 GiB
RAM, QEMU virtual disk. Raw output: `page-read-ubuntu24-qemu.csv`.

## Page read amplification

The benchmark created a 64 MiB file for each page size, used 5,000 random
`pread` operations per repetition, called `POSIX_FADV_DONTNEED` before each
repetition, and read one complete page per operation.

| Page size | Logical bytes read/run | Mean reads/s |
| ---: | ---: | ---: |
| 4 KiB | 19.5 MiB | 9.68k |
| 8 KiB | 39.1 MiB | 10.17k |
| 16 KiB | 78.1 MiB | 9.65k |
| 32 KiB | 156.3 MiB | 7.10k |

The result is not a final index-page benchmark because it has no real Bucket
occupancy, no concurrent readers, no page-cache eviction and no NVMe device.
It does establish two useful constraints for the next prototype:

- 32 KiB has no obvious random-read advantage in this environment and carries
  four times the read amplification of 8 KiB.
- 8 KiB and 16 KiB need to be compared using a real sparse Bucket layout; the
  raw `reads/s` difference here is too small to select one.

## Next experiment

Implement a small sparse Bucket prototype with the actual slot metadata and
measure bytes/key, page-cache hit rate, random Get latency, and overflow-chain
length at 0.5/0.8/0.95 occupancy. Until that exists, the main design must not
freeze `Bucket=256B`, `page=8KiB`, or double physical copies as final constants.
