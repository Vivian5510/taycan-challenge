#!/usr/bin/env python3
"""Fixed native synchronous workload: concurrency sweep or F2 I/O A/B."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'taycan_participant_sdk/tests'))
sys.path.insert(0, str(ROOT / 'taycan_participant_sdk/benchmarks'))
from process_runner import ProcessRunner
from native_platform import read_macos_rss


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mode', choices=['rocksdb', 'f2'], required=True)
    p.add_argument('--results', type=Path, required=True)
    p.add_argument('--inline-build', type=Path)
    p.add_argument('--aio-build', type=Path)
    args = p.parse_args()
    out = args.results.resolve()
    out.mkdir(parents=True, exist_ok=False)
    if args.mode == 'rocksdb':
        binaries = {'rocksdb': ROOT / 'build/native-rocksdb-8.9.1-arm64/taycan_kv_bench'}
        threads = [1, 4, 16, 64, 128]
        workloads = ['update']
    else:
        if not args.inline_build or not args.aio_build:
            p.error('f2 requires both builds')
        binaries = {k: v.resolve() / 'taycan_kv_bench' for k, v in
                    [('inline', args.inline_build), ('posix-aio', args.aio_build)]}
        threads = [4, 16, 64]
        workloads = ['update', 'contest_mix']
    report = dict(status='RUNNING', started_at=datetime.now(timezone.utc).isoformat(), mode=args.mode,
                  classification='native macOS diagnostic, synchronous durable APIs, not contest scoring',
                  parameters=dict(num_keys=4096, key_size=32, value_size=256, mem_limit_bytes=4 << 30,
                                  duration_seconds=5, warmup_seconds=1, sample_every=1, repeats=2,
                                  threads=threads, workloads=workloads),
                  shared_workload_sha256=sha(ROOT / 'taycan_participant_sdk/benchmarks/kv_bench.cc'),
                  engines={}, runs=[], summary=[])
    for name, binary in binaries.items():
        report['engines'][name] = dict(binary=str(binary), sha256=sha(binary),
                                      identity=json.loads(subprocess.check_output([str(binary), '--describe'], text=True)))

    def save():
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')

    save()
    try:
        for workload in workloads:
            for repeat in (1, 2):
                for count in threads if repeat == 1 else reversed(threads):
                    for variant in list(binaries) if repeat == 1 else reversed(list(binaries)):
                        name = f'{workload}-{variant}-t{count}-r{repeat}'
                        folder = out / name
                        folder.mkdir()
                        cmd = [str(binaries[variant]), '--data-dir=' + str(folder / 'db'),
                               '--workload=' + workload, '--num-keys=4096', '--key-size=32', '--value-size=256',
                               '--mem-limit-bytes=' + str(4 << 30), '--threads=' + str(count),
                               '--duration-seconds=5', '--warmup-seconds=1', '--sample-every=1',
                               '--progress-interval-seconds=1', '--seed=20261021']
                        row = dict(case=name, variant=variant, threads=count, workload=workload,
                                   repeat=repeat, command=cmd, status='FAIL')
                        print('START', name, flush=True)
                        try:
                            with (folder / 'output.log').open('w') as log:
                                with ProcessRunner(cmd, log, (4 << 30) * 11 // 10, 180, .02,
                                                   folder / 'processes.jsonl', rss_reader=read_macos_rss,
                                                   rss_source='macOS libproc',
                                                   env=dict(os.environ, F2_WAL_GROUP_WAIT_US='50')) as worker:
                                    row['monitor'] = worker.finish()
                            output = (folder / 'output.log').read_text()
                            row['measurement'] = next(json.loads(s) for s in output.splitlines()
                                                      if s.startswith('{"type":"result"'))
                            if row['measurement']['status'] != 'PASS':
                                raise RuntimeError('failed measurement')
                            for tag, key in [('F2_ROCKS_WAL ', 'groups'), ('F2_DATA_PREFIX ', 'prefix'),
                                             ('F2_NATIVE_IO ', 'io')]:
                                lines = [s for s in output.splitlines() if s.startswith(tag)]
                                if lines:
                                    row[key] = [{k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', s)} for s in lines]
                            row['status'] = 'PASS'
                        except Exception as error:
                            row['reason'] = str(error)
                        report['runs'].append(row)
                        save()
                        print('COMPLETE', name, row['status'], row.get('measurement', {}).get('ops_per_second'), flush=True)
                        if row['status'] != 'PASS':
                            raise RuntimeError('cell failed: ' + name)
        for workload in workloads:
            for count in threads:
                for variant in binaries:
                    rows = [r for r in report['runs'] if r['workload'] == workload and r['threads'] == count and r['variant'] == variant]
                    report['summary'].append(dict(workload=workload, threads=count, variant=variant,
                        ops_per_second=statistics.median(r['measurement']['ops_per_second'] for r in rows),
                        min_ops_per_second=min(r['measurement']['ops_per_second'] for r in rows),
                        max_ops_per_second=max(r['measurement']['ops_per_second'] for r in rows),
                        put_p99_us=statistics.median(r['measurement']['put_latency']['p99_us'] for r in rows),
                        get_p99_us=statistics.median(r['measurement']['get_latency']['p99_us'] for r in rows) if workload != 'update' else None,
                        rss_peak_bytes=max(r['monitor']['rss_peak_bytes'] for r in rows)))
        for variant, binary in binaries.items():
            if sha(binary) != report['engines'][variant]['sha256']:
                raise RuntimeError('binary changed during experiment')
        report['status'] = 'PASS'
    except BaseException as error:
        report.update(status='FAIL', reason=str(error))
        raise
    finally:
        report['finished_at'] = datetime.now(timezone.utc).isoformat()
        save()


if __name__ == '__main__':
    main()
