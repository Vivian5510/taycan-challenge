#!/usr/bin/env python3
"""Calculate candidate index layout sizes without allocating the index."""
import argparse
import json
import math
import platform
import sys
from datetime import datetime, timezone


KEY_COUNTS = (100_000, 16_000_000, 200_000_000)
LOAD = 0.80
SLOT_BYTES = 16
FLAT_BUCKET_BYTES = 256
PAGE_BYTES = 8192
PAGE_HEADER_BYTES = 64
DIRECTORY_ENTRY_BYTES = 16


def ceil_div(a, b):
    return (a + b - 1) // b


def mib(n):
    return n / (1024 * 1024)


def gib(n):
    return n / (1024 * 1024 * 1024)


def flat_bucket(n, slots=15, copies=2):
    buckets = math.ceil(n / (slots * LOAD))
    pages = ceil_div(buckets * FLAT_BUCKET_BYTES, PAGE_BYTES)
    physical = pages * PAGE_BYTES * copies
    directory = pages * DIRECTORY_ENTRY_BYTES
    return {
        "buckets": buckets,
        "pages": pages,
        "index_bytes": physical,
        "directory_bytes": directory,
        "total_bytes": physical + directory,
        "bytes_per_key": (physical + directory) / n,
    }


def packed_bucket(n, bucket_bytes, slots=None, copies=2):
    slots = slots or max(1, (bucket_bytes - 16) // SLOT_BYTES)
    buckets = math.ceil(n / (slots * LOAD))
    pages = ceil_div(buckets * bucket_bytes, PAGE_BYTES)
    physical = pages * PAGE_BYTES * copies
    directory = pages * DIRECTORY_ENTRY_BYTES
    return {
        "bucket_bytes": bucket_bytes,
        "slots": slots,
        "buckets": buckets,
        "pages": pages,
        "index_bytes": physical,
        "directory_bytes": directory,
        "total_bytes": physical + directory,
        "bytes_per_key": (physical + directory) / n,
    }


def mini_slab(n, slots_per_slab=4, header_bytes=16, slot_bytes=SLOT_BYTES,
              copies=2):
    slabs = math.ceil(n / (slots_per_slab * LOAD))
    slab_bytes = header_bytes + slots_per_slab * slot_bytes
    physical = slabs * slab_bytes * copies
    directory = slabs * DIRECTORY_ENTRY_BYTES
    return {
        "slots_per_slab": slots_per_slab,
        "slab_bytes": slab_bytes,
        "slabs": slabs,
        "index_bytes": physical,
        "directory_bytes": directory,
        "total_bytes": physical + directory,
        "bytes_per_key": (physical + directory) / n,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    rows = []
    for n in KEY_COUNTS:
        rows.append({
            "keys": n,
            "flat_256b": flat_bucket(n),
            "packed_64b": packed_bucket(n, 64),
            "packed_128b": packed_bucket(n, 128),
            "packed_256b": packed_bucket(n, 256),
            "mini_slab_4": mini_slab(n, 4),
            "mini_slab_8": mini_slab(n, 8),
        })

    result = {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "host": platform.platform(),
        "python": sys.version,
        "assumptions": {
            "target_load_factor": LOAD,
            "slot_bytes": SLOT_BYTES,
            "flat_bucket_bytes": FLAT_BUCKET_BYTES,
            "index_page_bytes": PAGE_BYTES,
            "page_header_bytes": PAGE_HEADER_BYTES,
            "directory_entry_bytes": DIRECTORY_ENTRY_BYTES,
            "physical_index_copies": 2,
            "note": "This is a capacity model, not a runtime benchmark. It excludes data records, WAL and allocator overhead.",
        },
        "rows": rows,
    }
    with open(args.output, "w", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
        output.write("\n")
    for row in rows:
        print(f"keys={row['keys']}")
        for name in ("flat_256b", "packed_64b", "packed_128b", "packed_256b", "mini_slab_4", "mini_slab_8"):
            item = row[name]
            print(f"  {name}: {gib(item['total_bytes']):.3f} GiB total, {item['bytes_per_key']:.2f} B/key")


if __name__ == "__main__":
    main()
