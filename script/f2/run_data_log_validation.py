#!/usr/bin/env python3
"""Native F2 data-log validation; every acknowledged write must survive SIGKILL."""
import argparse
import contextlib
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'script/taycan/tests'))
sys.path.insert(0, str(ROOT / 'script/taycan/benchmarks'))
from process_runner import ProcessRunner
from native_platform import read_macos_rss
import crash_test


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--results', type=Path, required=True)
    args = parser.parse_args()
    build, out = args.build_dir.resolve(), args.results.resolve()
    cache = (build / 'CMakeCache.txt').read_text()
    segment_bits = next((int(line.split('=', 1)[1]) for line in cache.splitlines()
                         if line.startswith('TAYCAN_F2_DATA_SEGMENT_BITS:')), 30)
    block_bytes = next((int(line.split('=', 1)[1]) for line in cache.splitlines()
                        if line.startswith('TAYCAN_F2_PREFIX_BLOCK_BYTES:')), 4096)
    gc_enabled = 'TAYCAN_F2_DURABLE_COMPACTION:BOOL=ON' in cache
    segment_bytes = 1 << (segment_bits if gc_enabled else 30)
    out.mkdir(parents=True, exist_ok=False)
    probe = build / 'engine/f2_sync_api_probe'
    report = dict(status='RUNNING', started_at=datetime.now(timezone.utc).isoformat(),
                  classification=f'{sys.platform} development probes, not contest/power-cut acceptance',
                  binary_sha256={str(p.relative_to(build)): hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in [probe] + [build / n for n in
                                 ('basic_test', 'recovery_test', 'concurrent_test', 'crash_worker')]}, tests=[])

    def save():
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(name, command, expected=0, env=None):
        started = time.monotonic()
        row = dict(test=name, command=list(map(str, command)), expected_returncode=expected, status='FAIL')
        try:
            child = subprocess.run(row['command'], capture_output=True, text=True, timeout=120, env=env)
            (out / (name + '.stdout.log')).write_text(child.stdout)
            (out / (name + '.stderr.log')).write_text(child.stderr)
            row.update(returncode=child.returncode, seconds=time.monotonic() - started)
            if child.returncode != expected:
                raise RuntimeError('unexpected returncode: ' + str(child.returncode))
            row['status'] = 'PASS'
            return child
        except Exception as error:
            row.update(reason=str(error), seconds=time.monotonic() - started)
            raise
        finally:
            report['tests'].append(row)
            save()
            print(name, row['status'], flush=True)

    def boundary(point, concurrent, writers=16):
        name = ('concurrent-' if concurrent else 'single-') + point
        folder = out / name
        folder.mkdir()
        db = folder / 'db'
        env = dict(os.environ, F2_SYNC_FAULT=point, F2_SYNC_FAULT_MIN_SERIAL='65',
                   F2_SYNC_RECLAIM_BYTES='4096' if concurrent else '65536')
        common = [build / 'crash_worker', '--num_keys=64', '--key_size=32', '--value_size=256',
                  '--mem_limit_bytes=' + str(4 << 30), '--seed=20261021', '--db_path=' + str(db)]
        if not concurrent:
            run(name + '-prepare', common + ['--mode=prepare', '--operations=64'])
            read_fd, write_fd = os.pipe()
            command = list(map(str, common + ['--mode=serve', '--operations=64', '--progress_fd=' + str(write_fd)]))
        else:
            command = list(map(str, [probe, 'group-serve', db, str(writers)]))
        ack = [0] * writers if concurrent else 64
        start = time.monotonic()
        with (folder / 'stdout.log').open('w') as stdout, (folder / 'stderr.log').open('w') as stderr:
            child = subprocess.Popen(command, stdin=None if concurrent else subprocess.PIPE,
                                     stdout=subprocess.PIPE if concurrent else stdout,
                                     stderr=stderr, env=env,
                                     pass_fds=() if concurrent else (write_fd,))
            if concurrent:
                read_fd = child.stdout.fileno()
            else:
                os.close(write_fd)
            buffer = bytearray()

            def line():
                while b'\n' not in buffer:
                    if not select.select([read_fd], [], [], 120)[0]:
                        raise TimeoutError('ACK pipe timeout')
                    data = os.read(read_fd, 4096)
                    if not data:
                        return None
                    buffer.extend(data)
                value, _, rest = buffer.partition(b'\n')
                buffer[:] = rest
                text = value.decode()
                if concurrent:
                    stdout.write(text + '\n')
                return text

            try:
                if concurrent:
                    while True:
                        message = line()
                        if message is None:
                            break
                        if not message.startswith('ACK '):
                            continue
                        _, thread, version = message.split()
                        thread, version = int(thread), int(version)
                        if version != ack[thread] + 1:
                            raise RuntimeError('non-sequential ACK')
                        ack[thread] = version
                else:
                    if line() != 'READY':
                        raise RuntimeError('missing READY')
                    while ack < 1024:
                        child.stdin.write(b'G')
                        child.stdin.flush()
                        message = line()
                        if message is None:
                            break
                        if message != 'ACK ' + str(ack + 1):
                            raise RuntimeError('non-sequential ACK')
                        ack += 1
                code = child.wait(timeout=5)
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait()
                if concurrent:
                    child.stdout.close()
                else:
                    child.stdin.close()
                    os.close(read_fd)
        row = dict(test=name, status='FAIL', acknowledged=ack, serve_returncode=code,
                   environment={k: v for k, v in env.items() if k.startswith('F2_SYNC_')},
                   seconds=time.monotonic() - start)
        report['tests'].append(row)
        save()
        if code != -9 or ('F2_SYNC_FAULT ' + point) not in (folder / 'stderr.log').read_text():
            raise RuntimeError('injected boundary not reached: ' + name)
        if concurrent:
            result = run(name + '-verify1', [probe, 'group-verify', db, ','.join(map(str, ack)), 'allow-next'])
            recovered = next(json.loads(s)['recovered'] for s in result.stdout.splitlines() if s.startswith('{"recovered"'))
            run(name + '-verify2', [probe, 'group-verify', db, ','.join(map(str, recovered))])
        else:
            result = run(name + '-verify1', common + ['--mode=verify', '--operations=' + str(ack), '--possible_operation=' + str(ack + 1)])
            recovered = int(next(s.split()[1] for s in result.stdout.splitlines() if s.startswith('RECOVERED_PREFIX ')))
            run(name + '-verify2', common + ['--mode=verify', '--operations=' + str(recovered)])
        row.update(status='PASS', recovered=recovered, seconds=time.monotonic() - start)
        save()
        print(name, 'PASS', flush=True)

    save()
    try:
        for name in ('basic', 'recovery', 'concurrent'):
            run(name, [build / (name + '_test'), '--num_keys=64', '--key_size=32', '--value_size=256',
                       '--mem_limit_bytes=' + str(4 << 30), '--seed=20261021', '--gtest_color=no',
                       '--data_dir=' + str(out / (name + '-db'))])
        for name in ('sync-error', 'io-error', 'apply-error'):
            run(name, [probe, name, out / (name + '-db')])
        same = out / 'same-key-db'
        result = run('same-key-write', [probe, 'write-kill', same], expected=-9)
        value = next(s.split()[1] for s in result.stdout.splitlines() if s.startswith('EXPECTED '))
        for repeat in (1, 2):
            run('same-key-recover-' + str(repeat), [probe, 'verify', same, value])
        span = out / 'span-db'
        env = dict(os.environ, F2_PROBE_BUDGET_BYTES=str(32 << 20))
        result = run('span-write', [probe, 'span-write-kill', span], expected=-9, env=env)
        if 'SPANNED_PREFIX_ACK 649' not in result.stdout:
            raise RuntimeError('incorrect acknowledged count')
        base = int((span / 'F2-SYNC-CURRENT').read_text().splitlines()[3])
        for case in ('partial-tail', 'data-corrupt', 'footer-corrupt', 'missing-data', 'root-corrupt'):
            db = out / (case + '-db')
            shutil.copytree(span, db)
            logs = sorted((db / 'hot').glob('log.log*'), key=lambda p: int(p.name[7:]))
            log = logs[-1] if case in ('partial-tail', 'footer-corrupt') else db / ('hot/log.log' + str(base // segment_bytes))
            if case in ('partial-tail', 'footer-corrupt'):
                contents = log.read_bytes()
                segment_start = int(log.name[7:]) * segment_bytes
                seal_end = next((end for end in range(len(contents) // block_bytes * block_bytes,
                                                       0, -block_bytes)
                                 if int.from_bytes(contents[end-68:end-60], 'little') == 0x31474F4C32464350
                                 and int.from_bytes(contents[end-52:end-44], 'little') == segment_start + end),
                                None)
                if seal_end is None:
                    raise RuntimeError('fixture has no commit seal in the final segment')
            if case == 'partial-tail':
                with log.open('r+b') as f:
                    f.truncate(seal_end)
                    f.seek(seal_end)
                    f.write(b'abc')
            elif case in ('data-corrupt', 'footer-corrupt'):
                data = bytearray(log.read_bytes())
                offset = base % segment_bytes
                if case == 'data-corrupt' and len(data) < offset + 128:
                    raise RuntimeError('no committed suffix to corrupt')
                data[offset + 24 if case == 'data-corrupt' else seal_end - 16] ^= 1
                log.write_bytes(data)
            elif case == 'missing-data':
                log.rename(log.with_suffix('.preserved'))
            else:
                root = db / 'F2-SYNC-CURRENT'
                lines = root.read_text().splitlines()
                lines[-1] = str(int(lines[-1]) ^ 1)
                root.write_text('\n'.join(lines) + '\n')
            run(case, [probe, 'span-verify', db], expected=0 if case == 'partial-tail' else 1, env=env)
        for repeat in (1, 2):
            run('span-recover-' + str(repeat), [probe, 'span-verify', span], env=env)
        embedded = out / 'embedded-marker-db'
        run('embedded-marker-write', [probe, 'embedded-marker-write-kill', embedded], expected=-9)
        for repeat in (1, 2):
            run('embedded-marker-recover-' + str(repeat), [probe, 'embedded-marker-verify', embedded])
        collision = run('real-hash-collision', [probe, 'hash-collision', out / 'collision-db'], env=env)
        # One request per commit no longer stages multiple keys together. The
        # probe still verifies two actual colliding keys, 16 writers and reopen.
        if 'REAL_HASH_COLLISION PASS' not in collision.stdout:
            raise RuntimeError('real hash collision probe did not complete')
        def monitored(*a, **kw):
            kw.update(rss_reader=read_macos_rss, rss_source='macOS libproc; not Linux VmRSS')
            return ProcessRunner(*a, **kw)
        crash = out / 'original-sdk-crash'
        crash.mkdir()
        if sys.platform == 'darwin':
            crash_test.ProcessRunner = monitored
        with (crash / 'output.log').open('w') as log, contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
            crash_test.main(['--worker', str(build / 'crash_worker'), '--num-keys', '64', '--key-size', '32',
                             '--value-size', '256', '--mem-limit-bytes', str(4 << 30), '--trials', '5',
                             '--timeout', '120', '--rss-limit-bytes', str((4 << 30) * 11 // 10),
                             '--data-dir', str(crash), '--monitor-report', str(crash / 'processes.jsonl')])
        report['tests'].append(dict(test='original-sdk-crash', trials=5, status='PASS'))
        save()
        points = ('before-data-flush', 'before-data-sync', 'after-data-sync', 'after-wal',
                  'before-ack', 'before-root', 'after-root')
        for concurrent in (False, True):
            for point in points:
                boundary(point, concurrent)
        report['status'] = 'PASS'
    except BaseException as error:
        report.update(status='FAIL', reason=str(error))
        raise
    finally:
        report['finished_at'] = datetime.now(timezone.utc).isoformat()
        save()
    print('COMPLETE', len(report['tests']), 'checks', flush=True)


if __name__ == '__main__':
    main()
