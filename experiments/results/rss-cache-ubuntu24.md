# RSS and index-page residency experiment

Date: 2026-10-27. Environment: Ubuntu 24.04 x86_64, QEMU VM, Linux
`/proc/self/status`.

The demo touched 8,192 8 KiB pages (64 MiB target) in three ways:

| Access | VmRSS before | VmRSS after |
| --- | ---: | ---: |
| heap frames | 3.81 MiB | 67.81 MiB |
| mmap, touch every page | 3.60 MiB | 67.65 MiB |
| one 8 KiB `pread` buffer | 3.64 MiB | 3.81 MiB |

A second heap run with 32,768 frames (256 MiB target) reached 260.35 MiB RSS.

The exact values include process/runtime overhead, but the relationship is the
important result: touching mapped index pages makes them resident and counted in
VmRSS. `pread` into a bounded reusable frame does not retain the whole file in
the process RSS. The engine therefore must not mmap the full persistent index or
rely on the host page cache as its memory-control mechanism. A bounded explicit
PageCache with `pread`, pin counts and eviction matches the `mem_limit_bytes`
contract.

This does not prove the final cache size or eviction policy. It also does not
measure the kernel page cache, which is outside the process RSS in the public
runner when file pages are not mapped.
