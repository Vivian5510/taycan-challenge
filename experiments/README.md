# Taycan Design Experiments

These experiments are deliberately separate from the retired submission
implementation. They measure or
calculate design assumptions before the storage format is frozen.

## Index layout model

```sh
python3 experiments/index_layout.py --output experiments/results/index-layout.json
```

The model is not a workload benchmark. It reports the cost of several physical
layouts under the same key count and target load factor. The formulas and
assumptions are printed into the JSON result.

## Sparse Bucket prototype

```sh
limactl shell --workdir=/workspace/taycan-challenge taycan-ubuntu24 bash -c '
  g++ -O2 -std=c++17 -Wall -Wextra experiments/sparse_bucket_model.cc \
    -o /var/lib/taycan-workspace/sparse_bucket_model
  /var/lib/taycan-workspace/sparse_bucket_model --keys 16000000 \
    --output /var/lib/taycan-workspace/experiments/results/sparse-bucket.csv
'
```

This is a physical-layout model with actual bucket occupancy and overflow
allocation. The 16M-key result is recorded in
`experiments/results/round3-analysis.md`.

## Durable log sync benchmark

Compile and run on the Ubuntu x86_64 VM, with the database files on the VM's
local filesystem:

```sh
limactl shell --workdir=/workspace/taycan-challenge taycan-ubuntu24 bash -c '
  mkdir -p /var/lib/taycan-workspace/experiments/results
  g++ -O2 -std=c++17 -Wall -Wextra \
    experiments/log_sync_bench.cc -o /var/lib/taycan-workspace/log_sync_bench
  /var/lib/taycan-workspace/log_sync_bench \
    --dir /var/lib/taycan-data/log-sync \
    --ops 2000 --value-bytes 128 --repetitions 3 \
    --output /var/lib/taycan-workspace/experiments/results/log-sync.csv
'
```

The benchmark compares one durable lane log, two separately synced files, and
batch sizes 1/4/16/64. It is evidence about the relative cost on this virtual
disk, not a claim about the final evaluation NVMe device.

## Index page read amplification

```sh
limactl shell --workdir=/workspace/taycan-challenge taycan-ubuntu24 bash -c '
  g++ -O2 -std=c++17 -Wall -Wextra experiments/page_read_bench.cc \
    -o /var/lib/taycan-workspace/page_read_bench
  /var/lib/taycan-workspace/page_read_bench \
    --dir /var/lib/taycan-data/page-read \
    --file-bytes $((64*1024*1024)) --reads 5000 --repetitions 3 \
    --output /var/lib/taycan-workspace/experiments/results/page-read.csv
'
```

This only measures random whole-page `pread` cost with a best-effort
`POSIX_FADV_DONTNEED`; it does not represent the final Bucket index.

## RSS residency demo

```sh
limactl shell --workdir=/workspace/taycan-challenge taycan-ubuntu24 bash -c '
  g++ -O2 -std=c++17 -Wall -Wextra experiments/rss_cache_demo.cc \
    -o /var/lib/taycan-workspace/rss_cache_demo
  /var/lib/taycan-workspace/rss_cache_demo \
    --mode pread --file /var/lib/taycan-data/rss-demo.dat \
    --frames 8192 --page-bytes 8192
'
```

The demo compares reusable `pread` buffers with heap frames and `mmap` pages.
Its result is recorded in `experiments/results/rss-cache-ubuntu24.md`.

## Interpretation rules

- Keep raw CSV/JSON files with the command line and VM identity.
- Do not choose lane count, page size, or commit window from one run.
- Repeat after the actual engine has a real record format and workload.
- The QEMU virtual disk is useful for relative comparisons, not absolute NVMe
  throughput or P99 claims.
