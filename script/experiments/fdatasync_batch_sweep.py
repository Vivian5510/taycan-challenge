#!/usr/bin/env python3
"""Measure one buffered pwrite plus one fdatasync for several batch sizes."""

import argparse
import json
import os
from pathlib import Path
import statistics
import time


RECORD_BYTES = 384
FILE_BYTES = 128 << 20
REGION_BYTES = 64 << 20
WARMUP_GROUPS = 20
MEASURED_GROUPS = 500
BATCH_SIZES = (1, 2, 4, 8, 16, 32, 64, 128)


def write_all(descriptor, data, offset):
    view = memoryview(data)
    written_total = 0
    while written_total < len(view):
        written = os.pwrite(descriptor, view[written_total:], offset + written_total)
        if written <= 0:
            raise OSError("pwrite returned no bytes")
        written_total += written


def sync_data(descriptor):
    while True:
        try:
            os.fdatasync(descriptor)
            return
        except InterruptedError:
            continue


def percentile(values, fraction):
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, int(len(ordered) * fraction + 0.999999) - 1))
    return ordered[index]


def run_group(descriptor, payload, offset):
    group_start = time.perf_counter_ns()
    write_start = group_start
    write_all(descriptor, payload, offset)
    sync_start = time.perf_counter_ns()
    sync_data(descriptor)
    end = time.perf_counter_ns()
    return {
        "write_us": (sync_start - write_start) / 1000,
        "fdatasync_us": (end - sync_start) / 1000,
        "group_us": (end - group_start) / 1000,
    }


def prepare_file(descriptor):
    zeroes = bytes(1 << 20)
    for offset in range(0, FILE_BYTES, len(zeroes)):
        write_all(descriptor, zeroes, offset)
    sync_data(descriptor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    parser.add_argument("raw_jsonl", type=Path)
    args = parser.parse_args()

    descriptor = os.open(args.file, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o600)
    try:
        prepare_file(descriptor)
        raw_rows = []
        summaries = []
        for trial in range(1, 4):
            shift = (trial - 1) * 3
            order = BATCH_SIZES[shift:] + BATCH_SIZES[:shift]
            for batch_records in order:
                payload = bytes([(trial + batch_records) & 0xFF]) * (batch_records * RECORD_BYTES)
                slot_bytes = (len(payload) + 4095) & ~4095
                for group in range(WARMUP_GROUPS):
                    run_group(descriptor, payload, (group * slot_bytes) % REGION_BYTES)

                samples = []
                for group in range(MEASURED_GROUPS):
                    offset = ((group + WARMUP_GROUPS) * slot_bytes) % REGION_BYTES
                    sample = run_group(descriptor, payload, offset)
                    samples.append(sample)
                    raw_rows.append({
                        "trial": trial,
                        "batch_records": batch_records,
                        "bytes": len(payload),
                        "group": group,
                        **sample,
                    })

                write_times = [sample["write_us"] for sample in samples]
                sync_times = [sample["fdatasync_us"] for sample in samples]
                group_times = [sample["group_us"] for sample in samples]
                mean_group = statistics.fmean(group_times)
                summaries.append({
                        "type": "summary",
                        "trial": trial,
                        "batch_records": batch_records,
                        "bytes": len(payload),
                        "groups": len(samples),
                        "mean_write_us": statistics.fmean(write_times),
                        "mean_fdatasync_us": statistics.fmean(sync_times),
                        "p50_fdatasync_us": percentile(sync_times, 0.50),
                        "p99_fdatasync_us": percentile(sync_times, 0.99),
                        "mean_group_us": mean_group,
                        "p50_group_us": percentile(group_times, 0.50),
                        "p99_group_us": percentile(group_times, 0.99),
                        "group_ops_per_second": batch_records * 1_000_000 / mean_group,
                })
        args.raw_jsonl.write_text(
            "".join(json.dumps(row, separators=(",", ":")) + "\n" for row in raw_rows),
            encoding="utf-8",
        )
        for summary in summaries:
            print(json.dumps(summary, separators=(",", ":")), flush=True)
    finally:
        os.close(descriptor)
        args.file.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
