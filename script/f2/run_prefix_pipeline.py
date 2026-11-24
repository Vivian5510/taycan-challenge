#!/usr/bin/env python3
"""Frozen F2 synchronous-ACK A/B: caller-led, depth-one, bounded pipeline."""
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
sys.path.insert(0, str(ROOT / 'script/taycan/tests'))
sys.path.insert(0, str(ROOT / 'script/taycan/benchmarks'))
from process_runner import ProcessRunner
from native_platform import read_macos_rss


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results', type=Path, required=True)
    parser.add_argument('--variants', default='caller,depth1,depth4')
    parser.add_argument('--depth4-build', type=Path)
    args = parser.parse_args()
    out = args.results.resolve()
    out.mkdir(parents=True, exist_ok=False)
    builds = {'caller': 'f2-prefix-caller-20261031', 'depth1': 'f2-prefix-depth1-20261031',
              'depth4': 'f2-prefix-pipeline-20261031'}
    selected = args.variants.split(',')
    if not selected or len(set(selected)) != len(selected) or any(v not in builds for v in selected):
        parser.error('invalid variant selection')
    builds = {name: builds[name] for name in selected}
    binaries = {name: ROOT / 'archive/build-history' / build / 'taycan_kv_bench' for name, build in builds.items()}
    if args.depth4_build:
        if 'depth4' not in binaries:
            parser.error('depth4-build requires selected depth4')
        binaries['depth4'] = args.depth4_build.resolve() / 'taycan_kv_bench'
    report = dict(status='RUNNING', started_at=datetime.now(timezone.utc).isoformat(),
        classification='native macOS ARM64 diagnostic, F_FULLFSYNC per durable covered prefix; not contest scoring',
        parameters=dict(num_keys=4096, key_size=32, value_size=256, mem_limit_bytes=4 << 30,
                        threads=[4, 16, 64], workloads=['update', 'contest_mix'], repeats=2,
                        warmup_seconds=1, duration_seconds=5, sample_every=1,
                        spill=dict(num_keys=32768, mem_limit_bytes=32 << 20, threads=64, repeats=1)),
        engines={}, shared_workload_sha256=sha(ROOT / 'code/taycan_participant_sdk/benchmarks/kv_bench.cc'),
        runs=[], summary=[])
    for name, binary in binaries.items():
        report['engines'][name] = dict(binary=str(binary), sha256=sha(binary),
            identity=json.loads(subprocess.check_output([str(binary), '--describe'], text=True)))

    def save():
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')

    def cell(variant, workload, threads, repeat, spill=False):
        name = f'{"spill" if spill else "hot"}-{workload}-{variant}-t{threads}-r{repeat}'
        folder = out / name
        folder.mkdir()
        keys, budget = (32768, 32 << 20) if spill else (4096, 4 << 30)
        cmd = [str(binaries[variant]), '--data-dir=' + str(folder / 'db'), '--workload=' + workload,
               '--num-keys=' + str(keys), '--mem-limit-bytes=' + str(budget), '--key-size=32', '--value-size=256',
               '--threads=' + str(threads), '--warmup-seconds=' + ('0' if spill else '1'),
               '--duration-seconds=5', '--sample-every=1', '--progress-interval-seconds=1', '--seed=20261021']
        row = dict(case=name, variant=variant, workload=workload, threads=threads, repeat=repeat,
                   spill=spill, command=cmd, status='FAIL')
        print('START', name, flush=True)
        try:
            with (folder / 'output.log').open('w') as log:
                with ProcessRunner(cmd, log, budget * 11 // 10 + (8 << 20), 180, .02,
                                   folder / 'processes.jsonl', rss_reader=read_macos_rss,
                                   rss_source='macOS libproc', env=dict(os.environ, F2_WAL_GROUP_WAIT_US='50')) as worker:
                    row['monitor'] = worker.finish()
            output = (folder / 'output.log').read_text()
            row['measurement'] = next(json.loads(s) for s in output.splitlines() if s.startswith('{"type":"result"'))
            if row['measurement']['status'] != 'PASS' or (spill and row['measurement']['operations'] != keys):
                raise RuntimeError('incomplete or failed workload')
            for tag, key in [('F2_ROCKS_WAL ', 'wal'), ('F2_DATA_PREFIX ', 'prefix'),
                             ('F2_PREFIX_PIPELINE ', 'pipeline'), ('F2_SYNC_IO ', 'io')]:
                line = next((s for s in output.splitlines() if s.startswith(tag)), None)
                if line:
                    row[key] = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)}
            row['status'] = 'PASS'
        except Exception as error:
            row['reason'] = str(error)
        report['runs'].append(row)
        save()
        print('COMPLETE', name, row['status'], row.get('measurement', {}).get('ops_per_second'), flush=True)
        if row['status'] != 'PASS':
            raise RuntimeError('failed cell: ' + name)

    save()
    try:
        for workload in ('update', 'contest_mix'):
            for threads in (4, 16, 64):
                for repeat in (1, 2):
                    for variant in list(builds) if repeat == 1 else reversed(list(builds)):
                        cell(variant, workload, threads, repeat)
        for variant in builds:
            cell(variant, 'load', 64, 1, True)
        for workload in ('update', 'contest_mix'):
            for threads in (4, 16, 64):
                summary = dict(workload=workload, threads=threads)
                for variant in builds:
                    rows = [r for r in report['runs'] if not r['spill'] and r['workload'] == workload
                            and r['threads'] == threads and r['variant'] == variant]
                    summary[variant] = dict(ops_per_second=statistics.median(r['measurement']['ops_per_second'] for r in rows),
                        min_ops_per_second=min(r['measurement']['ops_per_second'] for r in rows),
                        max_ops_per_second=max(r['measurement']['ops_per_second'] for r in rows),
                        put_p99_us=statistics.median(r['measurement']['put_latency']['p99_us'] for r in rows),
                        get_p99_us=statistics.median(r['measurement']['get_latency']['p99_us'] for r in rows) if workload != 'update' else None,
                        rss_peak_bytes=max(r['monitor']['rss_peak_bytes'] for r in rows))
                report['summary'].append(summary)
        for variant, binary in binaries.items():
            if sha(binary) != report['engines'][variant]['sha256']:
                raise RuntimeError('binary changed while running')
        report['status'] = 'PASS'
    except BaseException as error:
        report.update(status='FAIL', reason=str(error))
        raise
    finally:
        report['finished_at'] = datetime.now(timezone.utc).isoformat()
        save()


if __name__ == '__main__':
    main()
