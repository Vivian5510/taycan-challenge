#!/usr/bin/env python3
"""Measure a bare append + F_FULLFSYNC on macOS; no engine or batching."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import platform
import statistics
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True)
    parser.add_argument('--operations', type=int, default=256)
    parser.add_argument('--record-bytes', type=int, default=240)
    parser.add_argument('--repetitions', type=int, default=3)
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('macOS required')
    if min(args.operations, args.record_bytes, args.repetitions) <= 0:
        parser.error('counts and size must be positive')
    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    result = dict(classification='native component experiment; not KV-engine throughput',
                  environment=dict(platform=platform.platform(), machine=platform.machine()),
                  sync='fcntl F_FULLFSYNC, command 51 from macOS SDK sys/fcntl.h',
                  record_bytes=args.record_bytes, operations=args.operations, runs=[])
    payload = bytes((n*131+17) % 256 for n in range(args.record_bytes))
    for repeat in range(args.repetitions):
        sync_times = []
        with tempfile.TemporaryDirectory(prefix='native-log-probe-', dir=str(output.parent)) as directory:
            fd = os.open(str(Path(directory)/'log'), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
            try:
                start = time.perf_counter_ns()
                for operation in range(args.operations):
                    at = 0
                    while at < len(payload):
                        written = os.write(fd, payload[at:])
                        if written <= 0:
                            raise RuntimeError('write did not advance')
                        at += written
                    begin = time.perf_counter_ns()
                    fcntl.fcntl(fd, getattr(fcntl, 'F_FULLFSYNC', 51))
                    sync_times.append(time.perf_counter_ns()-begin)
                elapsed = (time.perf_counter_ns()-start)/1e9
                if os.fstat(fd).st_size != args.operations*args.record_bytes:
                    raise RuntimeError('incomplete log')
            finally:
                os.close(fd)
        ordered = sorted(sync_times)
        result['runs'].append(dict(repetition=repeat+1, elapsed_seconds=elapsed,
                                  ops_per_second=args.operations/elapsed,
                                  mean_sync_us=statistics.mean(sync_times)/1000,
                                  p50_sync_us=statistics.median(sync_times)/1000,
                                  p99_sync_us=ordered[max(0, (99*len(ordered)+99)//100-1)]/1000))
        output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
