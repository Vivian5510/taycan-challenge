# Round 3 Results

Date: 2026-10-28. Environment: Ubuntu 24.04 x86_64, 8 vCPU, 24 GiB RAM,
QEMU virtual disk.

## Sparse Bucket prototype

The prototype uses the candidate physical shape:

- 8 KiB page;
- 64B page header;
- 31 buckets per page;
- 15 slots per bucket;
- 16B slot metadata;
- 256B bucket allocation including overflow metadata;
- one physical page copy in an append-only index segment;
- 8B logical-page directory entry;
- overflow buckets allocated only when a bucket is full.

It inserts 16 million uniformly mixed hashes, using three target load factors.
Raw output: `sparse-bucket-16m-ubuntu24-v3.csv`.

| Target load | Pages | Overflow buckets | Max chain | Approx index bytes/key |
| ---: | ---: | ---: | ---: | ---: |
| 0.50 | 68,966 | 9,714 | 2 | 35.50 B |
| 0.80 | 43,011 | 207,421 | 3 | 25.36 B |
| 0.95 | 36,282 | 397,033 | 3 | 24.95 B |

The bytes/key number includes allocated physical pages, the logical-page
directory and overflow buckets. It does not include data records, page headers
beyond the fixed allocation, allocator overhead, or page-cache residency.

The prototype confirms that physical page allocation can remain sparse and that
the directory is small relative to the disk index. It also shows a cost tradeoff:
the lower page count at high load is partly paid back by many overflow buckets.
The current prototype only reports chain length and does not benchmark exact Key
comparison or cache misses, so it does not freeze 0.8/0.95 as the final target.

## Consequence

The next implementation prototype should use the same 8 KiB/31-bucket/15-slot
shape, append-only index page generations, and a bounded `pread` PageCache. It
must measure real page-cache hit rate and Get P99 before changing the shape.
