#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 1 ]]; then
    echo "usage: $0 RESULT_DIR" >&2
    exit 2
fi

out=$1
mkdir -p "$out"
source_file="$(dirname "$0")/fdatasync_batch_sweep.py"
test -f "$source_file"
test "$(df -PB1 "$out" | awk 'NR==2 {print $4}')" -ge 21474836480

{
    uname -srmo
    printf 'logical_cpus='
    nproc
    free -h
    findmnt -T "$out" -o SOURCE,FSTYPE,OPTIONS
    lsblk -d -o NAME,MODEL,SIZE,ROTA,TYPE
    df -h "$out"
    python3 --version
} > "$out/platform.txt"

sha256sum "$source_file" > "$out/source.sha256"
/usr/bin/time -f 'elapsed_seconds=%e max_rss_kib=%M' -o "$out/runtime.txt" \
    python3 "$source_file" "$out/benchmark-data.bin" "$out/raw.jsonl" \
    > "$out/summary.jsonl"
test ! -e "$out/benchmark-data.bin"

python3 - "$out" <<'PY'
import json
from collections import Counter
from pathlib import Path
import sys

out = Path(sys.argv[1])
raw = [json.loads(line) for line in (out / "raw.jsonl").read_text().splitlines()]
summaries = [json.loads(line) for line in (out / "summary.jsonl").read_text().splitlines()]
counts = Counter((row["trial"], row["batch_records"]) for row in raw)
assert len(raw) == 12_000, len(raw)
assert len(summaries) == 24, len(summaries)
assert len(counts) == 24 and set(counts.values()) == {500}, counts
assert all(row["fdatasync_us"] > 0 for row in raw)
assert all(row["groups"] == 500 for row in summaries)
print(f"validated samples={len(raw)} summaries={len(summaries)}")
PY

sha256sum "$out/platform.txt" "$out/runtime.txt" "$out/raw.jsonl" \
    "$out/summary.jsonl" > "$out/results.sha256"
