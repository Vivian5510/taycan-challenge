# Taycan Engine

This source directory is the submission engine. It builds the Linux F2 backend with synchronous `commit-log` WAL, the fixed WAL owner/group collector, and the WAL-coordinated F2 background compaction worker enabled by default. Successful Put/Delete calls wait for the WAL durability completion before returning. WAL prefers prewritten `O_DIRECT | O_DSYNC` AIO and falls back to buffered `O_DSYNC` writes if the filesystem rejects `O_DIRECT`. Cross-group WAL pipelining is not enabled.

## SDK integration

The official SDK archive contains the parent project and supplies `contest_sdk`. Extract this engine archive at the SDK root so that it creates or replaces `engine/`, then run the SDK commands unchanged:

```sh
tar -xzf engine.tar.gz -C .
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
python3 tests/run_tests.py --build-dir build --tests basic
```

`engine.cc` defines `KVEngine::Open`, the concrete `Adapter`, and its Put/Get/Delete overrides. Those methods convert SDK statuses and delegate to `FasterBackend`. `src/faster_backend.cc` selects the synchronous backend; `src/f2_sync_runtime.inc` constructs and recovers the F2 store, starts background checkpoint/compaction maintenance, and implements the public operations. The WAL format, serialization, AIO submission/completion, and durable ACK boundary live in `src/f2_commit_experiment.h` and `src/f2_sync_members.inc`. F2's generated Linux source, including the repository's F2 patches, is checked into `vendor/faster/`; the submission build runs no source generator and requires no files from the research checkout.

## Build defaults

The target is Linux x86_64 with C++17 and GCC 8.3 or newer, following the SDK's declared compiler floor. WAL persistence uses `O_DSYNC` on its descriptor and waits for synchronous `pwritev` or Linux AIO completion; this avoids a build-time dependency on `pwritev2`/`RWF_DSYNC` and supports the CentOS 7 toolchain. FASTER file I/O prefers `O_DIRECT` and retries buffered I/O when a filesystem rejects the flag. CMake builds oneTBB, libaio, and libuuid from the pinned source in `vendor/`. libaio and libuuid are static libraries. oneTBB is a shared library because its upstream CMake marks static builds unsupported; its RUNPATH points to the oneTBB library built inside the SDK's build directory. Configuration, compilation, and execution do not download dependencies or use system TBB/libaio/libuuid/RocksDB development packages. The ordinary compiler runtime, libc, and pthread support come from the target toolchain. `vendor/faster/` uses FASTER commit `332ea7da532a688acf648ee60c4f63a1caeaf1f7`; `vendor/faster/UPSTREAM-CHANGES.patch` records source transformations and injected F2 code.

`DEPENDENCIES.json` records upstream versions, source archive hashes, and licenses. `vendor/SOURCES.sha256` hashes the vendored files. CentOS 7-specific compiler and WAL compatibility details are documented in `CENTOS7-BUILD-FIXES.md`. CRC32C for the WAL is implemented in the current F2 adapter; this configuration does not use RocksDB CRC32C or the RocksDB storage engine.

If a filesystem rejects `O_DIRECT`, the fallback is logged once as `F2_IO_DIRECT_FALLBACK`. Unrecoverable Linux file open/close and native AIO errors emit `F2_IO_ERROR` with the path and syscall result. These diagnostics run only on errors.

The package is source-only. On CentOS 7.9 x86_64, kernel 3.10.0-1160, glibc 2.17, GCC 8.3.1 and CMake 3.30.9, the SDK configures and builds offline and the official `basic` test passes with unchanged defaults. Additional 128-key reopen/concurrency checks and one SIGKILL trial pass as small diagnostics. These results do not establish full default-scale correctness, compaction acceptance, memory-budget compliance at contest scale, or contest performance.
