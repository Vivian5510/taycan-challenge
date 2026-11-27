# CentOS 7 build compatibility

This package keeps the synchronous WAL acknowledgement boundary while supporting the SDK's CentOS 7 / GCC 8.3 baseline.

- libuuid's vendored compatibility header declares `strndup` only when libc does not provide it. CMake now detects the declaration with `_GNU_SOURCE` and defines `HAVE_STRNDUP` for the vendored libuuid target.
- Linux WAL files that require durable writes are opened with `O_DSYNC`; the aligned WAL descriptor uses `O_DIRECT | O_DSYNC`. Synchronous `pwritev` return and Linux AIO completion remain the durability barrier before acknowledgement. This avoids requiring `pwritev2` and `RWF_DSYNC` at compile time.
- Extent preparation retains its separate buffered descriptor and explicit `fdatasync`; only the frame-writing descriptor carries `O_DSYNC`. This avoids adding a redundant durability barrier to each 4 MiB extent preparation. The historical `F2_WAL_RWF_DSYNC` environment switch is retained for compatibility; in this package it selects descriptor-based data sync.
- FASTER's concurrent log page atomic status is initialized in the `std::atomic` constructor initializer list. This is required by GCC 8.3, which deletes the generic atomic default constructor.
- The native FASTER AIO submission path handles a full queue (`-EAGAIN`) by consuming a completion before retrying the same request; `-EINTR` is retried. Older AIO rings otherwise fill before the initial index checkpoint has submitted its 256 writes, leaving its completion counter waiting forever.
- Some filesystems reject `O_DIRECT` with `EINVAL` even when the target directory is new. FASTER files now reopen buffered and continue with Linux AIO; WAL falls back to buffered `O_DSYNC` `pwritev`. On a persistent filesystem the WAL acknowledgement still waits for synchronous data completion. A volatile filesystem such as tmpfs cannot provide power-loss durability regardless of this fallback.

The package remains x86_64/Linux-only. A successful build does not imply all public tests pass; the exact build and basic-test result should be recorded separately for each target environment.
