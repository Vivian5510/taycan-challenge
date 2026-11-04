"""Validate the restored caller baseline; preserve raw failures, never retry."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib
import json
import os
import shutil
import subprocess
import sys
import threading

B = Path(__file__).resolve().parent
ROOT = B.parents[1]
BUILD = ROOT / 'build/f2-pre-dfly-main-20261104'
sys.path[:0] = [str(ROOT / 'taycan_participant_sdk/tests'),
               str(ROOT / 'taycan_participant_sdk/benchmarks')]
from process_runner import ProcessRunner
from native_platform import read_macos_rss

OUT = B / 'validation'
OUT.mkdir(exist_ok=False)
env = {k: v for k, v in os.environ.items() if not k.startswith('F2_')}
env['F2_WAL_GROUP_WAIT_US'] = '50'
now = lambda: datetime.now(timezone.utc).isoformat()
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
identity = json.loads(subprocess.check_output([str(BUILD / 'taycan_kv_bench'), '--describe'], text=True))
old = json.loads((ROOT / 'analysis/f2-open-source-wait-20261103/results.json').read_text())['engines']['caller']['identity']
assert identity == old
report = dict(status='RUNNING', started_at=now(), engine_identity=identity,
              identity_matches_historical_caller=True, runs=[],
              binaries={n: sha(BUILD / n) for n in ['taycan_kv_bench', 'basic_test', 'concurrent_test',
                                                   'recovery_test', 'crash_worker', 'engine/f2_sync_api_probe']},
              parameters=dict(mem_limit_bytes=4 << 30, key_size=32, value_size=256,
                              sdk_check_keys=2000, reader_threads=31,
                              boundary_suite='Unmodified historical run_data_log_validation.py: 64 keys; span probe 32MiB'),
              storage=dict(minimum_free_bytes=20 << 30, maximum_database_bytes=2 << 30,
                           free_min_bytes=shutil.disk_usage(B).free, database_peak_bytes=0))

def save():
    (OUT / 'results.json').write_text(json.dumps(report, indent=2) + '\n')

def run(name, command, timeout=180):
    if shutil.disk_usage(B).free < 20 << 30:
        raise RuntimeError('less than 20GiB free')
    folder = OUT / name
    folder.mkdir()
    row = dict(test=name, command=list(map(str, command)), status='RUNNING', started_at=now())
    report['runs'].append(row)
    save()
    print('START', name, flush=True)
    stop = threading.Event()
    try:
        with (folder / 'output.log').open('w') as log:
            with ProcessRunner(row['command'], log, (4 << 30) * 11 // 10, timeout, .02,
                               folder / 'processes.jsonl', env=env, rss_reader=read_macos_rss,
                               rss_source='macOS libproc; not Linux VmRSS') as process:
                def guard():
                    while not stop.wait(1):
                        free = shutil.disk_usage(B).free
                        report['storage']['free_min_bytes'] = min(report['storage']['free_min_bytes'], free)
                        if free < 20 << 30:
                            process._fail('storage_guard: less than 20GiB free')
                            return
                        for marker in OUT.rglob('F2-SYNC-CURRENT'):
                            size = sum(p.stat().st_size for p in marker.parent.rglob('*') if p.is_file())
                            report['storage']['database_peak_bytes'] = max(report['storage']['database_peak_bytes'], size)
                            if size > 2 << 30:
                                process._fail('storage_guard: database larger than 2GiB')
                                return
                monitor = threading.Thread(target=guard, daemon=True)
                monitor.start()
                try:
                    row['monitor'] = process.finish()
                finally:
                    stop.set()
                    monitor.join()
        if name == 'historical-boundaries':
            suite = json.loads((OUT / 'boundaries/results.json').read_text())
            assert suite['status'] == 'PASS' and all(x['status'] == 'PASS' for x in suite['tests'])
            row['checks'] = len(suite['tests'])
        row.update(status='PASS', finished_at=now())
        print('PASS', name, row.get('checks', ''), flush=True)
    except BaseException as error:
        row.update(status='FAIL', reason=repr(error), finished_at=now())
        raise
    finally:
        save()

try:
    for name in ['basic', 'concurrent', 'recovery']:
        command = [BUILD / (name + '_test'), '--num_keys=2000', '--key_size=32', '--value_size=256',
                   '--mem_limit_bytes=' + str(4 << 30), '--seed=20261021', '--gtest_color=no',
                   '--data_dir=' + str(OUT / name / 'db')]
        if name == 'concurrent':
            command += ['--reader_threads=31']
        run(name, command)
        shutil.rmtree(OUT / name / 'db')
    run('historical-boundaries', [sys.executable, ROOT / 'experiments/faster-native-adapter/run_data_log_validation.py',
                                 '--build-dir', BUILD, '--results', OUT / 'boundaries'], timeout=600)
    suite = json.loads((OUT / 'boundaries/results.json').read_text())
    databases = set()
    for row in suite['tests']:
        command = row.get('command', [])
        for arg in command:
            if arg.startswith('--data_dir=') or arg.startswith('--db_path='):
                databases.add(Path(arg.split('=', 1)[1]))
        if command and command[0].endswith('f2_sync_api_probe') and len(command) > 2:
            databases.add(Path(command[2]))
    for marker in (OUT / 'boundaries').rglob('F2-SYNC-CURRENT'):
        databases.add(marker.parent)
    for db in databases:
        assert db.is_relative_to(OUT)
        if db.is_dir():
            shutil.rmtree(db)
    report.update(status='PASS', boundary_checks=len(suite['tests']),
                  cleanup='Successful databases removed after all dependent recovery checks; logs/results retained')
    for n, expected in report['binaries'].items():
        assert sha(BUILD / n) == expected
except BaseException as error:
    report.update(status='FAIL', reason=repr(error))
    raise
finally:
    report['finished_at'] = now()
    save()
print('COMPLETE', report['status'], flush=True)
