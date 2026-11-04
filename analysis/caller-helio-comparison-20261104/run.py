"""Frozen caller main versus audited Helio: validate, then compare serially."""
from pathlib import Path
from datetime import datetime, timezone
import contextlib
import hashlib
import json
import os
import platform
import re
import resource
import shutil
import struct
import subprocess
import sys
import threading
from unittest import mock

B = Path(__file__).resolve().parent
ROOT = B.parents[1]
sys.path[:0] = [str(ROOT / 'taycan_participant_sdk/tests'), str(ROOT / 'taycan_participant_sdk/benchmarks')]
from process_runner import ProcessRunner
from native_platform import read_macos_rss
import crash_test

OUT = B / 'runs'
OUT.mkdir(exist_ok=False)
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
now = lambda: datetime.now(timezone.utc).isoformat()
MODES = ['caller', 'helio10', 'helio50']
BUILDS = {'caller': ROOT / 'build/f2-caller-release-20261104',
          'helio10': ROOT / 'build/f2-helio-audit-20261103',
          'helio50': ROOT / 'build/f2-helio-audit-20261103'}
REVS = {'caller': 'cdbc644', 'helio10': '90eedee', 'helio50': '90eedee'}
envs = {'caller': {'F2_WAL_GROUP_WAIT_US': '50'}}
for mode in ['helio10', 'helio50']:
    envs[mode] = dict(F2_HELIO_PROFILE='audited', F2_HELIO_INVOKE='queue',
                     F2_HELIO_FAIRNESS='work', F2_HELIO_WRITE_ADMISSION='deferred',
                     F2_HELIO_SYNC_MODE='pool', F2_HELIO_IO_COMPLETION='queue',
                     F2_FIBER_COLLECT_US='10' if mode == 'helio10' else '50')

def cache(path):
    return dict((line.split(':', 1)[0], line.split('=', 1)[1]) for line in path.read_text().splitlines()
                if line and not line.startswith(('#', '//')) and ':' in line and '=' in line)

record = dict(status='FREEZING', started_at=now(), engines={}, validation=[], runs=[],
              parameters=dict(threads=32, mem_limit_bytes=4 << 30, key_size=32, value_size=256,
                              hot_num_keys=4096, hot_repeats=3, duration_seconds=8, warmup_seconds=2,
                              large_repeats=1, fsync=True, durable_gc=False,
                              collection_policy='caller adaptive <=50us; Helio bounded 10/50us; algorithms intentionally differ'),
              workload_sha256=sha(ROOT / 'taycan_participant_sdk/benchmarks/kv_bench.cc'),
              host=dict(platform=platform.platform(), machine=platform.machine(),
                        compiler=subprocess.check_output(['/usr/bin/c++', '--version'], text=True),
                        cpu=subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip(),
                        logical_cpus=os.cpu_count()),
              storage_policy=dict(minimum_free_bytes=20 << 30, maximum_database_bytes=2 << 30,
                                  interval_seconds=1, successful_database_cleanup=True))

def save():
    temp = B / 'results.json.tmp'
    temp.write_text(json.dumps(record, indent=2) + '\n')
    temp.replace(B / 'results.json')

def freeze():
    prior = json.loads(subprocess.check_output(['git', 'show',
         '90eedee:analysis/helio-integration-audit-20261103/comparison/results.json'], cwd=ROOT, text=True))
    assert sha(BUILDS['helio50'] / 'taycan_kv_bench') == prior['engines']['audited50']['sha256']
    assert record['workload_sha256'] == prior['workload_sha256']
    for mode in MODES:
        build = BUILDS[mode]
        binary = build / 'taycan_kv_bench'
        rev = subprocess.check_output(['git', 'rev-parse', REVS[mode]], cwd=ROOT, text=True).strip()
        files = subprocess.check_output(['git', 'ls-tree', '-r', '--name-only', rev,
                                        'experiments/faster-native-adapter'], cwd=ROOT, text=True).splitlines()
        sources = {p: hashlib.sha256(subprocess.check_output(['git', 'show', rev + ':' + p], cwd=ROOT)).hexdigest()
                   for p in files}
        if mode == 'caller':
            assert all(sha(ROOT / p) == h for p, h in sources.items())
        paths = [Path(line.strip().split(' (')[0]) for line in
                 subprocess.check_output(['otool', '-L', str(binary)], text=True).splitlines()[1:]]
        data = cache(build / 'CMakeCache.txt')
        flags = subprocess.check_output([str(ROOT / 'build/deps/ninja/bin/ninja'), '-C', str(build),
                                         '-t', 'commands', 'taycan_kv_bench'], text=True)
        (B / (mode + '-compile-commands.txt')).write_text('\n'.join(line for line in flags.splitlines()
                     if 'faster_backend.cc.o' in line or 'kv_bench.cc.o' in line) + '\n')
        record['engines'][mode] = dict(revision=rev, binary=str(binary), sha256=sha(binary),
                    identity=json.loads(subprocess.check_output([str(binary), '--describe'], text=True)),
                    libraries={str(p): sha(p) for p in paths if p.is_absolute() and p.is_file()},
                    cache=data, source_sha256=sources,
                    generated_source_sha256={str(p.relative_to(build / 'engine/faster-src')): sha(p)
                               for p in (build / 'engine/faster-src').rglob('*') if p.is_file()},
                    validation_binary_sha256={str(p.relative_to(build)): sha(p)
                               for p in [build / n for n in ['basic_test', 'concurrent_test', 'recovery_test', 'crash_worker', 'engine/f2_sync_api_probe']]},
                    reused_frozen_binary=mode != 'caller')
    common = ['CMAKE_BUILD_TYPE', 'CMAKE_CXX_COMPILER', 'CMAKE_CXX_FLAGS', 'CMAKE_CXX_FLAGS_RELEASE',
              'TAYCAN_F2_SYNC_STRATEGY', 'TAYCAN_F2_MAC_SYNC', 'TAYCAN_F2_NATIVE_IO',
              'TAYCAN_F2_LOG_PAGE_BITS', 'TAYCAN_F2_OVERFLOW_PAGE_BITS', 'TAYCAN_F2_PREFIX_BLOCK_BYTES',
              'TAYCAN_F2_DURABLE_COMPACTION', 'TAYCAN_F2_PREFIX_PIPELINE',
              'TAYCAN_F2_WRITE_ADAPTIVE_YIELD', 'TAYCAN_F2_WRITE_MAX_YIELD_US', 'TAYCAN_F2_AIO_COMPLETION_BATCH']
    for key in common:
        values = [record['engines'][mode]['cache'][key] for mode in MODES]
        assert len(set(values)) == 1, (key, values)
    assert record['engines']['caller']['cache']['CMAKE_BUILD_TYPE'] == 'Release'
    record['matched_build_parameters'] = {k: record['engines']['caller']['cache'][k] for k in common}
    record['helio_revision'] = subprocess.check_output(['git', '-C', str(ROOT / 'third_party/helio'), 'rev-parse', 'HEAD'], text=True).strip()
    assert record['helio_revision'] == '40b7b2042c81ed4accde17a03d5662eddef5730f'
    assert not subprocess.check_output(['git', '-C', str(ROOT / 'third_party/helio'), 'status', '--porcelain'], text=True).strip()
    record['boost_static_sha256'] = {str(p): sha(p) for p in (ROOT / 'build/deps/boost_1_85_0/stage/lib').glob('*.a')}
    save()

def verify():
    assert sha(ROOT / 'taycan_participant_sdk/benchmarks/kv_bench.cc') == record['workload_sha256']
    for mode, e in record['engines'].items():
        assert sha(e['binary']) == e['sha256']
        assert json.loads(subprocess.check_output([e['binary'], '--describe'], text=True)) == e['identity']
        for p, h in e['libraries'].items():
            assert sha(p) == h
        for p, h in e['validation_binary_sha256'].items():
            assert sha(BUILDS[mode] / p) == h
    for p, h in record['boost_static_sha256'].items():
        assert sha(p) == h

def environment(mode):
    e = {k: v for k, v in os.environ.items() if not k.startswith('F2_')}
    e.update(envs[mode])
    return e

def execute(row, folder, command, db=None, timeout=180):
    folder.mkdir(parents=True, exist_ok=False)
    row.update(command=list(map(str, command)), started_at=now(), env_overrides=envs[row['engine']], status='RUNNING')
    save()
    print('START', row['case'], flush=True)
    storage = dict(free_min_bytes=shutil.disk_usage(B).free, database_peak_logical_bytes=0)
    if storage['free_min_bytes'] < 20 << 30:
        raise RuntimeError('less than 20GiB free')
    stop = threading.Event()
    try:
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        with (folder / 'output.log').open('w') as log:
            with ProcessRunner(row['command'], log, (4 << 30) * 11 // 10, timeout, .02, folder / 'processes.jsonl',
                               rss_reader=read_macos_rss, rss_source='macOS libproc; strict 110%', env=environment(row['engine'])) as process:
                def guard():
                    while not stop.wait(1):
                        try:
                            free = shutil.disk_usage(B).free
                            storage['free_min_bytes'] = min(storage['free_min_bytes'], free)
                            size = sum(p.stat().st_size for p in db.rglob('*') if p.is_file()) if db and db.exists() else 0
                            storage['database_peak_logical_bytes'] = max(storage['database_peak_logical_bytes'], size)
                            if free < 20 << 30 or size > 2 << 30:
                                process._fail('storage_guard')
                                return
                        except FileNotFoundError:
                            pass
                        except Exception as error:
                            process._fail('storage_monitor: ' + repr(error))
                            return
                watcher = threading.Thread(target=guard, daemon=True)
                watcher.start()
                try:
                    row['monitor'] = process.finish()
                finally:
                    stop.set()
                    watcher.join()
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        row['cpu'] = dict(user_seconds=after.ru_utime-before.ru_utime, system_seconds=after.ru_stime-before.ru_stime)
        row['storage'] = storage
        assert row['monitor']['returncode'] == 0 and row['monitor']['status'] == 'PASS'
    except BaseException as error:
        row.update(status='FAIL', reason=repr(error), finished_at=now())
        raise
    finally:
        save()

def validate():
    record['status'] = 'VALIDATING'
    for mode in MODES:
        build = BUILDS[mode]
        for test in ['basic', 'concurrent', 'recovery']:
            row = dict(case=mode + '-' + test, engine=mode, status='NOT_RUN')
            record['validation'].append(row)
            folder = OUT / 'validation' / row['case']
            db = folder / 'db'
            command = [build / (test + '_test'), '--num_keys=2000', '--key_size=32', '--value_size=256',
                       '--mem_limit_bytes=' + str(4 << 30), '--seed=20261021', '--gtest_color=no', '--data_dir=' + str(db)]
            if test == 'concurrent':
                command += ['--reader_threads=31']
            execute(row, folder, command, db)
            row.update(status='PASS', finished_at=now())
            shutil.rmtree(db)
            save()
            print('PASS', row['case'], flush=True)
        folder = OUT / 'validation' / (mode + '-SIGKILL')
        folder.mkdir()
        row = dict(case=mode + '-SIGKILL', engine=mode, trials=5, status='RUNNING', started_at=now())
        record['validation'].append(row)
        save()
        try:
            def monitored(*a, **kw):
                kw.update(env=environment(mode), rss_reader=read_macos_rss, rss_source='macOS libproc; strict 110%')
                return ProcessRunner(*a, **kw)
            with (folder / 'output.log').open('w') as log, contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
                with mock.patch.object(crash_test, 'ProcessRunner', monitored):
                    crash_test.main(['--worker', str(build / 'crash_worker'), '--num-keys', '2000', '--key-size', '32',
                                     '--value-size', '256', '--mem-limit-bytes', str(4 << 30), '--trials', '5', '--timeout', '120',
                                     '--rss-limit-bytes', str((4 << 30) * 11 // 10), '--data-dir', str(folder),
                                     '--monitor-report', str(folder / 'processes.jsonl')])
            row.update(status='PASS', finished_at=now())
            for db in folder.rglob('db-*'):
                if db.is_dir():
                    shutil.rmtree(db)
            print('PASS', row['case'], flush=True)
        except BaseException as error:
            row.update(status='FAIL', reason=repr(error), finished_at=now())
            raise
        finally:
            save()

def make_plan():
    def add(mode, workload, repeat, phase='hot', keys=4096):
        record['runs'].append(dict(case=f'{phase}-{workload}-{mode}-r{repeat}', engine=mode,
                                   phase=phase, workload=workload, repeat=repeat, num_keys=keys, status='NOT_RUN'))
    for workload in ['contest_mix', 'update', 'read']:
        for repeat, order in [(1, MODES), (2, list(reversed(MODES))), (3, MODES[1:] + MODES[:1])]:
            for mode in order:
                add(mode, workload, repeat)
    for mode in MODES:
        add(mode, 'load', 1, 'million', 1000000)
    for mode in reversed(MODES):
        add(mode, 'read', 1, 'disk-region-read', 2000000)
    assert len(record['runs']) == 33

def benchmark():
    record['status'] = 'BENCHMARKING'
    for row in record['runs']:
        folder = OUT / row['phase'] / row['case']
        db = folder / 'db'
        cmd = [BUILDS[row['engine']] / 'taycan_kv_bench', '--data-dir=' + str(db), '--workload=' + row['workload'],
               '--num-keys=' + str(row['num_keys']), '--threads=32', '--key-size=32', '--value-size=256',
               '--mem-limit-bytes=' + str(4 << 30), '--seed=20261021', '--distribution=uniform',
               '--duration-seconds=8', '--warmup-seconds=' + ('0' if row['workload'] == 'load' else '2'),
               '--sample-every=1', '--progress-interval-seconds=1']
        try:
            execute(row, folder, cmd, db, timeout=600)
            lines = (folder / 'output.log').read_text().splitlines()
            measurement = row['measurement'] = next(json.loads(s) for s in lines if s.startswith('{"type":"result"'))
            assert measurement['status'] == 'PASS'
            if row['workload'] == 'load':
                assert measurement['operations'] == row['num_keys']
            for tag in ['F2_DATA_PREFIX', 'F2_SYNC_IO', 'F2_HELIO_NATIVE', 'F2_HELIO_AUDIT', 'F2_ROCKS_WAL']:
                row[tag] = [{k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', s)} for s in lines if s.startswith(tag + ' ')]
            assert len(row['F2_DATA_PREFIX']) == 1
            if row['engine'] != 'caller':
                audit = row['F2_HELIO_AUDIT'][0]
                assert audit['profile'] == audit['queued_completions'] == 1
                assert audit['completion_fibers'] == audit['io_outstanding'] == 0 and audit['clock_hz'] > 0
            # Last marker = invalid RecordInfo + fixed 4-byte Key + fixed Value header + 64-byte footer.
            # Read the final committed serial before removing the successful database.
            tail = row['F2_DATA_PREFIX'][0]['stable_tail']
            with (db / 'hot/log.log0').open('rb') as stream:
                stream.seek(tail - 88)
                marker = stream.read(88)
            fields = struct.unpack_from('<6Q2IQ', marker, 20)
            assert fields[2] == tail
            row['whole_run_writes'] = fields[4]
            if row['engine'] != 'caller':
                assert row['whole_run_writes'] == row['F2_HELIO_NATIVE'][0]['writes']
            if row['workload'] == 'load':
                assert row['whole_run_writes'] == row['num_keys']
            row['disk_growth_bytes'] = measurement['data_bytes_after_measure'] - measurement['data_bytes_before_measure']
            row.update(status='PASS', finished_at=now())
            save()
            shutil.rmtree(db)
            row['database_cleanup'] = 'removed after PASS; logs/metrics/RSS retained'
            print('PASS', row['case'], round(measurement['ops_per_second']), flush=True)
        except BaseException as error:
            row.update(status='FAIL', reason=repr(error), finished_at=now())
            raise
        finally:
            save()

try:
    freeze()
    make_plan()
    verify()
    validate()
    verify()
    benchmark()
    verify()
    record.update(status='PASS', identity_checks='All binary, identity, workload and dynamic/static library hashes match before/after')
except BaseException as error:
    record.update(status='FAIL', reason=repr(error))
    raise
finally:
    record['finished_at'] = now()
    save()
print('COMPLETE', record['status'], flush=True)
