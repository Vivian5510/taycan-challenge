#!/usr/bin/env python3
"""Interleaved native F2 WAL versus committed HybridLog, identical synchronous APIs."""
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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results', type=Path, required=True)
    parser.add_argument('--variants', default='wal,data512,data4096')
    parser.add_argument('--data512-build-dir', type=Path)
    args = parser.parse_args()
    out = args.results.resolve()
    out.mkdir(parents=True, exist_ok=False)
    builds = {'wal': 'f2-data-log-reference-20261030', 'data512': 'f2-data-log-512-20261030',
              'data4096': 'f2-data-log-4k-20261030'}
    selected = args.variants.split(',')
    if not selected or len(set(selected)) != len(selected) or any(v not in builds for v in selected):
        raise ValueError('invalid variant selection')
    builds = {k: builds[k] for k in selected}
    binaries = {k: ROOT / 'build' / v / 'taycan_kv_bench' for k, v in builds.items()}
    if args.data512_build_dir:
        if 'data512' not in binaries:
            raise ValueError('data512 is not selected')
        binaries['data512'] = args.data512_build_dir.resolve() / 'taycan_kv_bench'
    report = dict(status='RUNNING', started_at=datetime.now(timezone.utc).isoformat(),
                  classification='native macOS ARM64, F_FULLFSYNC, diagnostic sizes; not contest scoring',
                  parameters=dict(key_size=32, value_size=256, group_wait_us=50, latency_sample_every=1,
                                  hot_keys=1024, hot_budget_bytes=4 << 30, spill_keys=32768,
                                  spill_budget_bytes=32 << 20, mix_duration_seconds=5, mix_warmup_seconds=1),
                  shared_workload_sha256=sha(ROOT / 'taycan_participant_sdk/benchmarks/kv_bench.cc'),
                  engines={}, runs=[], summary=[])
    for name, binary in binaries.items():
        report['engines'][name] = dict(binary=str(binary), sha256=sha(binary),
                                      identity=json.loads(subprocess.check_output([str(binary), '--describe'], text=True)))

    def save():
        (out / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')

    def cell(variant, workload, threads, repeat, spill=False):
        name = f'{"spill" if spill else "hot"}-{workload}-{variant}-t{threads}-r{repeat}'
        folder = out / name
        folder.mkdir()
        budget, count = ((32 << 20, 32768) if spill else (4 << 30, 1024))
        command = [str(binaries[variant]), '--data-dir=' + str(folder / 'db'), '--workload=' + workload,
                   '--num-keys=' + str(count), '--threads=' + str(threads), '--mem-limit-bytes=' + str(budget),
                   '--key-size=32', '--value-size=256', '--duration-seconds=5',
                   '--warmup-seconds=' + ('0' if workload == 'load' else '1'),
                   '--sample-every=1', '--progress-interval-seconds=1', '--seed=20261021']
        row = dict(case=name, variant=variant, workload=workload, threads=threads, repeat=repeat,
                   spill=spill, command=command, status='FAIL')
        print('START', name, flush=True)
        try:
            with (folder / 'output.log').open('w') as log:
                with ProcessRunner(command, log, budget * 11 // 10 + (8 << 20), 180, .02,
                                   folder / 'processes.jsonl', rss_reader=read_macos_rss,
                                   rss_source='macOS libproc; development only',
                                   env=dict(os.environ, F2_WAL_GROUP_WAIT_US='50')) as worker:
                    row['monitor'] = worker.finish()
            output = (folder / 'output.log').read_text()
            row['measurement'] = next(json.loads(s) for s in output.splitlines() if s.startswith('{"type":"result"'))
            if row['measurement']['status'] != 'PASS' or (workload == 'load' and row['measurement']['operations'] != count):
                raise RuntimeError('incomplete or failed measurement')
            for tag, key in [('F2_ROCKS_WAL ', 'wal'), ('F2_DATA_PREFIX ', 'prefix'),
                             ('F2_SYNC_IO ', 'native_io'), ('F2_COMMIT_COUNTS ', 'commits')]:
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
            raise RuntimeError('cell failed: ' + name)

    save()
    try:
        for workload in ('load', 'contest_mix'):
            for threads in (1, 4, 16):
                for repeat in (1, 2):
                    order = selected if repeat == 1 else list(reversed(selected))
                    for variant in order:
                        cell(variant, workload, threads, repeat)
        for variant in selected:
            cell(variant, 'load', 16, 1, spill=True)
        for workload in ('load', 'contest_mix'):
            for threads in (1, 4, 16):
                row = dict(workload=workload, threads=threads)
                for variant in binaries:
                    cells = [r for r in report['runs'] if not r['spill'] and r['variant'] == variant
                             and r['workload'] == workload and r['threads'] == threads]
                    row[variant] = dict(ops_per_second=statistics.median(r['measurement']['ops_per_second'] for r in cells),
                                        min_ops_per_second=min(r['measurement']['ops_per_second'] for r in cells),
                                        max_ops_per_second=max(r['measurement']['ops_per_second'] for r in cells),
                                        put_p99_us=statistics.median(r['measurement']['put_latency']['p99_us'] for r in cells),
                                        get_p99_us=statistics.median(r['measurement']['get_latency']['p99_us'] for r in cells) if workload != 'load' else None,
                                        rss_peak_bytes=max(r['monitor']['rss_peak_bytes'] for r in cells),
                                        file_bytes=statistics.median(r['measurement']['data_bytes_after_close'] for r in cells),
                                        write_bytes=statistics.median(r['native_io']['write_bytes'] + r['wal']['append_bytes'] for r in cells))
                report['summary'].append(row)
        for variant, binary in binaries.items():
            if sha(binary) != report['engines'][variant]['sha256']:
                raise RuntimeError('binary changed during comparison')
        report['status'] = 'PASS'
    except BaseException as error:
        report.update(status='FAIL', reason=str(error))
        raise
    finally:
        report['finished_at'] = datetime.now(timezone.utc).isoformat()
        save()


if __name__ == '__main__':
    main()
