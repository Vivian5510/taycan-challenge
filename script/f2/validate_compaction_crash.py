#!/usr/bin/env python3
"""Supplement the public oracle with observed native GC and exact fault phases.

This does not replace or relax the public tests. Each phase uses a fresh copy
of a closed database; confirmed operations are checked by public crash_worker.
"""
import argparse
from argparse import Namespace
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT/'script/taycan/tests'),
               str(ROOT/'script/taycan/benchmarks')]
import crash_test
from process_runner import ProcessRunner
from native_platform import read_macos_rss

PHASES = ['after-copy', 'after-logical-gc', 'after-checkpoint-before-root',
          'after-root-before-truncate', 'after-hot-truncate',
          'after-cold-truncate', 'after-index-truncate']


def monitored(*args, **kwargs):
    kwargs.update(rss_reader=read_macos_rss, rss_source='macOS libproc, strict 110%')
    return ProcessRunner(*args, **kwargs)


def events(path):
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text().splitlines()]


def actual_gc(log, require_deletion=False):
    copies = [e for e in log if e['event'] == 'native_compaction']
    for store in ('hot', 'cold'):
        assert any(e['store'] == store and e['success'] and e['until'] > e['begin']
                   for e in copies), 'no real native {} compaction'.format(store)
    deleted = [e for e in log if e['event'] == 'segment_deleted' and e['bytes'] > 0]
    if require_deletion:
        assert any('/hot/log.log' in e['path'] for e in deleted), 'no actual hot data deletion'
        assert any('/cold/log.log' in e['path'] for e in deleted), 'no actual cold data deletion'
    # Any actual deletion must follow publication of its recovery root.
    published = False
    for event in log:
        if event['event'] == 'phase' and event['phase'] == 'before-copy':
            published = False
        elif event['event'] == 'phase' and event['phase'] == 'after-root-before-truncate':
            published = True
        elif event['event'] == 'segment_deleted':
            assert published, 'segment deleted before root publication'
    return copies, deleted


def options(build, folder):
    return Namespace(worker=str(build/'crash_worker'), seed=20261021,
                     num_keys=100000, key_size=32, key_mode='random',
                     value_size=256, mem_limit_bytes=4 << 30,
                     rss_limit_bytes=(4 << 30)*11//10, timeout=600,
                     sample_interval_ms=20, monitor_report=str(folder/'processes.jsonl'))


def prepare(args, folder, operations):
    db = folder/'seed-db'
    env = os.environ.copy()
    env.pop('F2_GC_PAUSE_PHASE', None)
    env['F2_GC_TRACE'] = str(folder/'seed-gc.jsonl')
    with (folder/'seed.log').open('w') as log:
        with monitored(crash_test.worker_command(args) + ['--mode=prepare',
                       '--db_path='+str(db), '--operations='+str(operations)],
                       log, args.rss_limit_bytes, args.timeout, .02,
                       args.monitor_report, env=env) as process:
            process.finish()
    return db


def kill_at_phase(args, db, folder, initial, phase, concurrent=False):
    trace = folder/'gc.jsonl'
    env = os.environ.copy()
    env['F2_GC_TRACE'] = str(trace)
    if phase:
        env['F2_GC_PAUSE_PHASE'] = phase
    else:
        env.pop('F2_GC_PAUSE_PHASE', None)
    if concurrent:
        env.pop('F2_GC_PAUSE_PHASE', None)
        hold=folder/'copy-hold'
        hold.mkdir()
        env['F2_GC_COPY_HOLD_DIR']=str(hold)
    read_fd, write_fd = os.pipe()
    process = None
    buffer = bytearray()
    confirmed = initial
    try:
        with (folder/'serve.log').open('w') as log:
            process = monitored(crash_test.worker_command(args) + ['--mode=serve',
                                '--db_path='+str(db), '--operations='+str(initial),
                                '--progress_fd='+str(write_fd)], log,
                                args.rss_limit_bytes, args.timeout, .02,
                                args.monitor_report, stdin=subprocess.PIPE,
                                pass_fds=(write_fd,), env=env)
            os.close(write_fd)
            write_fd = -1
            deadline = time.monotonic()+60
            assert crash_test.read_line(read_fd, buffer, deadline, process.proc, log) == 'READY'
            old_root = (db/'F2-SYNC-CURRENT').read_bytes()
            for _ in range(16):
                process.proc.stdin.write(b'G')
                process.proc.stdin.flush()
                assert crash_test.read_line(read_fd, buffer, deadline, process.proc, log) == 'ACK '+str(confirmed+1)
                confirmed += 1
            if concurrent:
                while not (hold/'ready').exists() and time.monotonic()<deadline:
                    assert process.proc.poll() is None, 'worker exited before copy hold'
                    time.sleep(.02)
                assert (hold/'ready').exists(), 'native compaction did not reach copy hold'
                assert not any(e.get('phase')=='complete' for e in events(trace))
                # These successful API replies must happen before releasing the
                # held compaction, not merely before joining its thread.
                for _ in range(512):
                    process.proc.stdin.write(b'G')
                    process.proc.stdin.flush()
                    assert crash_test.read_line(read_fd,buffer,deadline,process.proc,log)=='ACK '+str(confirmed+1)
                    confirmed+=1
                assert not (hold/'release').exists()
                assert not any(e.get('phase')=='complete' for e in events(trace))
                (hold/'release').write_text('foreground ACKs received\n')
                while not any(e.get('phase')=='complete' for e in events(trace)) and time.monotonic()<deadline:
                    time.sleep(.02)
                evidence=events(trace)
                assert any(e.get('phase')=='complete' for e in evidence), 'compaction did not finish after release'
                actual_gc(evidence,require_deletion=True)
                assert any(e['event']=='copy_overlap' and e['writes']>=512 for e in evidence), 'no measured foreground write overlap'
            elif phase:
                stopped = False
                while time.monotonic() < deadline:
                    waited, status = os.waitpid(process.proc.pid, os.WUNTRACED | os.WNOHANG)
                    if waited:
                        assert os.WIFSTOPPED(status), 'worker exited before target GC phase'
                        stopped = True
                        break
                    time.sleep(.02)
                assert stopped, 'target GC phase not reached: '+phase
                evidence = events(trace)
                assert any(e.get('phase') == phase for e in evidence), 'paused at unexpected phase'
                actual_gc(evidence, phase == 'after-index-truncate')
                published = phase in PHASES[3:]
                assert ((db/'F2-SYNC-CURRENT').read_bytes() != old_root) == published, 'recovery-root transition mismatch'
                removed = [e for e in evidence if e['event'] == 'segment_deleted']
                for event in removed:
                    assert not Path(event['path']).exists(), 'segment deletion was only reported'
                if phase in PHASES[:4]:
                    assert not removed, 'old segments deleted before retirement phase'
                if phase in PHASES[4:]:
                    assert any('/hot/log.log' in e['path'] and e['bytes'] > 0 for e in removed), 'hot truncation did no work'
                if phase in PHASES[5:]:
                    assert any('/cold/log.log' in e['path'] and e['bytes'] > 0 for e in removed), 'cold truncation did no work'
            else:
                # Disabled-GC negative control: a normal acknowledged SIGKILL
                # can pass while the compaction coverage predicate rejects it.
                time.sleep(.5)
                evidence = events(trace)
            process.inject_kill()
            process.finish(expected_kill=True)
        with (folder/'verify.log').open('w') as log:
            recovered = crash_test.verify(args, db, confirmed, 0, log)
        assert recovered == confirmed
        return dict(confirmed=confirmed, recovered=recovered, reopens=2,
                    evidence=evidence, original_root_sha256=hashlib.sha256(old_root).hexdigest())
    finally:
        if process is not None:
            process.close()
            process.proc.stdin.close()
        os.close(read_fd)
        if write_fd >= 0:
            os.close(write_fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--results-dir', type=Path, required=True)
    parser.add_argument('--phases', default=','.join(PHASES))
    parser.add_argument('--negative-build-dir', type=Path)
    cli = parser.parse_args()
    build, base = cli.build_dir.resolve(), cli.results_dir.resolve()
    base.mkdir(parents=True, exist_ok=False)
    requested = cli.phases.split(',')
    assert all(p in PHASES for p in requested)
    crash_test.ProcessRunner = monitored
    results = dict(status='RUNNING', parameters=dict(num_keys=100000, rounds=10,
                   key_size=32, value_size=256, mem_limit_bytes=4 << 30),
                   worker_sha256=hashlib.sha256((build/'crash_worker').read_bytes()).hexdigest(),
                   phases=[], negative_control=None)

    def save():
        (base/'results.json').write_text(json.dumps(results,indent=2)+'\n')

    save()
    try:
        seed_args = options(build, base)
        seed = prepare(seed_args, base, 1000000)
        actual_gc(events(base/'seed-gc.jsonl'), require_deletion=True)
        print('SEED native hot/cold compaction and data segment deletion observed',flush=True)
        for phase in ['concurrent-copy']+requested:
            folder = base/phase
            folder.mkdir()
            db = folder/'db'
            shutil.copytree(seed, db)
            row = dict(phase=phase, status='RUNNING')
            results['phases'].append(row)
            save()
            start = time.monotonic()
            try:
                row.update(kill_at_phase(options(build, folder), db, folder, 1000000, phase,
                                        concurrent=phase=='concurrent-copy'))
                row['status'] = 'PASS'
            except BaseException as error:
                row.update(status='FAIL', reason=repr(error))
                raise
            finally:
                row['elapsed_seconds'] = time.monotonic()-start
                save()
            print('PASS {} acknowledged={} reopen=2'.format(phase,row['confirmed']),flush=True)
        if cli.negative_build_dir:
            folder = base/'negative-control'
            folder.mkdir()
            negative = options(cli.negative_build_dir.resolve(), folder)
            negative_db = prepare(negative, folder, 100000)
            result = kill_at_phase(negative, negative_db, folder, 100000, None)
            try:
                actual_gc(result['evidence'], require_deletion=True)
            except AssertionError as error:
                result.update(status='EXPECTED_REJECTION', reason=str(error), data_recovery='PASS')
            else:
                raise AssertionError('negative control incorrectly accepted disabled compaction')
            results['negative_control'] = result
            print('EXPECTED_REJECTION disabled compaction; ordinary recovery still PASS',flush=True)
        results['status'] = 'PASS'
    except BaseException as error:
        results.update(status='FAIL', reason=repr(error))
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
