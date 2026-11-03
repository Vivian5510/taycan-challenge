# Round 1 Results

Date: 2026-10-27

## Index capacity model

Command:

```sh
python3 experiments/index_layout.py --output experiments/results/index-layout.json
```

The model uses 80% target load, 16-byte index slots, 15 slots per 256-byte
bucket, 8 KiB physical pages, a 16-byte sparse directory entry, and two physical
copies of each index page. It excludes data records, log files and allocator
overhead.

| Keys | Flat 256B bucket total | Flat bytes/key |
| ---: | ---: | ---: |
| 100,000 | 0.004 GiB | 42.80 B |
| 16,000,000 | 0.636 GiB | 42.71 B |
| 200,000,000 | 7.955 GiB | 42.71 B |

The result rejects the first fixed full-bucket proposal for a 4 GiB index budget.
It can fit the raw index in a 16 GiB budget, but leaves less room for data
structures, page cache, logs, allocator overhead and the process itself. A
compressed/sparse physical representation is not optional if the 4 GiB tier is
in scope.

This is a capacity model, not a correctness or I/O benchmark. It does not yet
model overflow buckets, tombstones, fragmentation, slab reuse or directory
locality.

## Durable log sync benchmark

Environment: `taycan-ubuntu24`, Ubuntu 24.04 x86_64 guest, 8 vCPU, 24 GiB RAM,
QEMU virtual disk. The benchmark used 2,000 144-byte data records per run,
three repetitions, and batch sizes 1/4/16/64. Raw output:
`log-sync-ubuntu24-qemu.csv`.

Representative means across the three repetitions:

| Mode | Batch | Syncs/run | Approx ops/s |
| --- | ---: | ---: | ---: |
| single log | 1 | 2,000 | 952 |
| dual data+WAL | 1 | 2,000 each | 719 |
| single log | 16 | 125 | 11,236 |
| dual data+WAL | 16 | 125 each | 7,656 |
| single log | 64 | 32 | 34,588 |
| dual data+WAL | 64 each | 32 | 19,865 |

The relative result supports measuring a unified durable log and Group Commit.
It does not prove that two files are incorrect or that a particular batch window
is optimal. QEMU virtual-disk latency is not the evaluation NVMe latency, and
the benchmark does not include index-page publication or crash injection.

## Decisions after this round

- Do not freeze a 256B full-bucket double-copy index for the 4 GiB tier.
- Do not freeze a 50-200 microsecond commit window from this run.
- Continue with a page-size/read-amplification experiment and a real sparse
  bucket prototype before changing the main design document.
