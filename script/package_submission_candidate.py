#!/usr/bin/env python3
"""Assemble the current source candidate in the contest's code-archive format."""
from hashlib import sha256
from pathlib import Path
import shutil
import zipfile


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
STAGE = BUILD / ".taycan-submission-stage"
PACKAGE_ROOT = STAGE / "taycan_submission_candidate"
ARCHIVE = BUILD / "taycan_submission_candidate.zip"


def copy_tree(source: Path, destination: Path) -> None:
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns(
        ".git", "__pycache__", "*.pyc", ".DS_Store", "CMakePresets.json"))


def write_readme() -> None:
    readme = """# Taycan Submission Candidate

This archive follows the contest code-delivery shape: complete C++ implementation, CMake configuration, and this README. It is a source candidate, not a passing submission.

## Status

- Engine base: F2/FASTER, pinned at `research/FASTER`.
- Linux durability candidate: F2 `data-log`; the data log uses F2 HybridLog records and a checksummed commit footer before advancing the durable frontier and acknowledging writes.
- Compaction: requires `TAYCAN_F2_DURABLE_COMPACTION=ON`; the full official compaction and crash matrix has not been completed for this package snapshot.
- Checkpoint: `CreateCheckpoint` remains unsupported and is an optional scoring feature.
- Performance: the latest native Linux diagnostic was 300,000 keys, 32 callers, 4 GiB, with a 47,300 ops/s median. The contest load gate is 2e8 keys and at least 60,000 ops/s. This diagnostic does not pass that gate.
- Official 2e8-key load, both 4/16 GiB 600-second mixed workloads, and all required final correctness cases have not been verified on this exact archive.

Do not treat this archive as accepted or fully evaluated.

## Build

The build is intended for Linux x86_64 and requires an offline-provisioned toolchain and system libraries: CMake 3.24+, GCC 8.3+, Python 3.6+, oneTBB, libaio, libuuid, and RocksDB headers/library (used for CRC32C). GoogleTest 1.14 and gflags 2.2.2 source archives are bundled under `code/taycan_participant_sdk/third_party/`. No dependency download is performed by this project at configure time.

From this directory:

```sh
cmake -S code/taycan_participant_sdk -B /tmp/taycan-build -DCMAKE_BUILD_TYPE=Release -DTAYCAN_BUILD_PERF_BENCHMARKS=ON -DTAYCAN_F2_SYNC_STRATEGY=data-log -DTAYCAN_F2_DURABLE_COMPACTION=ON -DTAYCAN_F2_DATA_SEGMENT_BITS=23
cmake --build /tmp/taycan-build --parallel
```

Run the public correctness harness from this directory:

```sh
python3 script/taycan/tests/run_correctness.py --build-dir /tmp/taycan-build --results-dir /var/tmp/taycan-correctness
```

The archive was assembled without running this build. The listed system libraries and final Linux x86_64 configuration still need verification in the target evaluation environment.

## Layout

- `code/faster-native-adapter/`: F2 integration and persistence implementation.
- `code/taycan_participant_sdk/`: contest interface, CMake project, public C++ tests, benchmark workload, and bundled test dependencies.
- `script/taycan/`: correctness and performance runners.
- `research/FASTER/cc/src/`: pinned upstream FASTER/F2 source used by the build.
- `research/rocksdb-v8.9.1/`: CRC32C header dependency and upstream license notices.

The separate architecture design PDF is delivered beside this archive as `taycan_design_document.pdf`.
"""
    (PACKAGE_ROOT / "README.md").write_text(readme)


def main() -> None:
    BUILD.mkdir(parents=True, exist_ok=True)
    if STAGE.exists():
        shutil.rmtree(STAGE)
    if ARCHIVE.exists():
        ARCHIVE.unlink()
    PACKAGE_ROOT.mkdir(parents=True)

    copy_tree(ROOT / "code/faster-native-adapter", PACKAGE_ROOT / "code/faster-native-adapter")
    copy_tree(ROOT / "code/taycan_participant_sdk", PACKAGE_ROOT / "code/taycan_participant_sdk")
    copy_tree(ROOT / "script/taycan", PACKAGE_ROOT / "script/taycan")

    faster = PACKAGE_ROOT / "research/FASTER"
    faster.mkdir(parents=True)
    shutil.copy2(ROOT / "research/FASTER/LICENSE", faster / "LICENSE")
    copy_tree(ROOT / "research/FASTER/cc/src", faster / "cc/src")

    rocks = PACKAGE_ROOT / "research/rocksdb-v8.9.1"
    (rocks / "util").mkdir(parents=True)
    (rocks / "include/rocksdb").mkdir(parents=True)
    shutil.copy2(ROOT / "research/rocksdb-v8.9.1/util/crc32c.h", rocks / "util/crc32c.h")
    shutil.copy2(ROOT / "research/rocksdb-v8.9.1/include/rocksdb/rocksdb_namespace.h",
                 rocks / "include/rocksdb/rocksdb_namespace.h")
    shutil.copy2(ROOT / "research/rocksdb-v8.9.1/LICENSE.Apache", rocks / "LICENSE.Apache")
    shutil.copy2(ROOT / "research/rocksdb-v8.9.1/COPYING", rocks / "COPYING")
    write_readme()

    with zipfile.ZipFile(ARCHIVE, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(PACKAGE_ROOT.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(STAGE))

    digest = sha256(ARCHIVE.read_bytes()).hexdigest()
    print(f"archive={ARCHIVE}")
    print(f"bytes={ARCHIVE.stat().st_size}")
    print(f"sha256={digest}")
    shutil.rmtree(STAGE)


if __name__ == "__main__":
    main()
