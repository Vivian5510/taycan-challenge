#!/usr/bin/env python3
"""Build with CMake, run every public test here with Linux VmRSS supervision."""
import argparse
import contextlib
import json
from pathlib import Path
import tempfile
import time
import sys

from process_runner import ProcessRunner
import crash_test

MIB = 1 << 20
# These defaults are the public workloads, not performance scoring data.
CASES = {
    'basic': {},
    'sequential': dict(num_keys=1000000, key_mode='sequential'),
    'random': dict(num_keys=5000000),
    'overwrite': dict(num_keys=100000, rounds=10),
    'delete': dict(num_keys=100000),
    'mixed': dict(num_keys=1000000),
    'recovery': dict(num_keys=100000),
    'checkpoint': dict(num_keys=131072, key_size=64, value_size=4096),
    'concurrent': dict(num_keys=10000),
    'demand_load': dict(num_keys=16000000, key_size=64, value_size=128),
    'memory_limit': dict(num_keys=20000000),
    'compaction': dict(num_keys=100000, key_size=16, value_size=4096, rounds=10),
    'crash_recovery': dict(num_keys=100000, key_size=16, value_size=128),
    'compaction_crash': dict(num_keys=100000, key_size=16, value_size=4096),
}


def case_parameters(args, name):
    params = dict(num_keys=100000, key_size=64, value_size=128, rounds=10,
                  mem_limit_bytes=256*MIB, seed=args.seed)
    params.update(CASES[name])
    for key in ('num_keys', 'key_size', 'value_size', 'rounds', 'mem_limit_bytes'):
        if getattr(args, key) is not None:
            params[key] = getattr(args, key)
    return params


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--dry-run', action='store_true',
                        help='Print all resolved scenario parameters without starting any worker')
    parser.add_argument('--tests', default=','.join(CASES))
    parser.add_argument('--build-dir', default='build/submission/linux-x86_64-release')
    parser.add_argument('--results-dir')
    parser.add_argument('--seed', type=int, default=20261021)
    parser.add_argument('--timeout', type=float, default=86400)
    parser.add_argument('--sample-interval-ms', type=float, default=2)
    parser.add_argument('--trials', type=int, default=5)
    parser.add_argument('--rss-slack-bytes', type=int, default=8*MIB,
                        help='Provisional harness allowance; requires native Linux calibration')
    parser.add_argument('--strict-rss-limit', action='store_true',
                        help='Enforce floor(1.1*budget), without harness or reference allowances')
    parser.add_argument('--native-development', action='store_true',
                        help='Run identical public cases on macOS with libproc RSS; not Linux VmRSS acceptance')
    for flag in ('num-keys', 'key-size', 'value-size', 'rounds', 'mem-limit-bytes'):
        parser.add_argument('--' + flag, type=int)
    args = parser.parse_args(argv)
    if args.list:
        for name in CASES:
            print(name)
        return 0
    names = args.tests.split(',')
    if any(name not in CASES for name in names) or len(set(names)) != len(names):
        parser.error('unknown or duplicate test name')
    if (min(args.timeout, args.sample_interval_ms, args.trials) <= 0 or
            args.rss_slack_bytes < 0):
        parser.error('invalid monitor parameters')
    if args.dry_run:
        print(json.dumps(dict(tests=[dict(test=name, parameters=case_parameters(args, name))
                                   for name in names], trials=args.trials,
                              strict_rss_limit=args.strict_rss_limit,
                              native_development=args.native_development,
                              rss_slack_bytes=0 if args.strict_rss_limit else args.rss_slack_bytes,
                              reference_allowance=('none' if args.strict_rss_limit else
                                                   '4*num_keys bytes for mixed/compaction only')), indent=2))
        return 0
    native_rss = args.native_development and sys.platform == 'darwin'
    if not Path('/proc/self/status').exists() and not native_rss:
        parser.error('the monitored test entry point requires Linux /proc')
    def monitored(*a, **kw):
        if native_rss:
            sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'benchmarks'))
            from native_platform import read_macos_rss
            kw.update(rss_reader=read_macos_rss, rss_source='macOS libproc; native development, not Linux VmRSS')
        return ProcessRunner(*a, **kw)
    root = Path(args.results_dir).resolve() if args.results_dir else Path(
        tempfile.mkdtemp(prefix='taycan-results-')).resolve()
    root.mkdir(parents=True, exist_ok=True)
    build = Path(args.build_dir).resolve()
    results = []
    print('Results: ' + str(root), flush=True)
    for name in names:
        folder = Path(tempfile.mkdtemp(prefix=name + '-', dir=str(root)))
        params = case_parameters(args, name)
        # Explicit reference storage allowance, independent of engine allocations.
        reference_bytes = (4*params['num_keys'] if not args.strict_rss_limit and
                           name in ('mixed', 'compaction') else 0)
        allowance = 0 if args.strict_rss_limit else args.rss_slack_bytes + reference_bytes
        limit = params['mem_limit_bytes'] * 11 // 10 + allowance
        report, log_path = folder/'processes.jsonl', folder/'output.log'
        record = dict(test=name, parameters=params, rss_limit_bytes=limit,
                      harness_allowance_bytes=allowance,
                      validation_scope='macOS native development' if native_rss else 'Linux VmRSS supervised',
                      log=str(log_path), status='FAIL')
        start = time.monotonic()
        try:
            if name in ('crash_recovery', 'compaction_crash'):
                argv = ['--worker', str(build/'crash_worker'), '--data-dir', str(folder),
                        '--prepare-rounds', str(params['rounds'] if name == 'compaction_crash' else 1),
                        '--trials', str(args.trials), '--timeout', str(args.timeout),
                        '--rss-limit-bytes', str(limit), '--sample-interval-ms', str(args.sample_interval_ms),
                        '--monitor-report', str(report)]
                for key in ('seed', 'num_keys', 'key_size', 'value_size', 'mem_limit_bytes'):
                    argv += ['--'+key.replace('_', '-'), str(params[key])]
                # The controller runs here; each actual engine worker is monitored.
                with log_path.open('w') as log:
                    with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
                        original_runner = crash_test.ProcessRunner
                        crash_test.ProcessRunner = monitored
                        try:
                            crash_test.main(argv)
                        finally:
                            crash_test.ProcessRunner = original_runner
                record['status'] = 'PASS'
            else:
                command = [str(build/(name+'_test'))]
                command += ['--{}={}'.format(k, v) for k, v in params.items()]
                command += ['--data_dir='+str(folder)]
                with log_path.open('w') as log:
                    with monitored(command, log, limit, args.timeout,
                                       args.sample_interval_ms/1000, report) as process:
                        process.finish()
                record['status'] = 'SKIP' if '[  SKIPPED ]' in log_path.read_text() else 'PASS'
        except Exception as error:
            record['reason'] = str(error)
        record['elapsed_seconds'] = time.monotonic()-start
        if report.exists():
            processes = [json.loads(line) for line in report.read_text().splitlines()]
            record['rss_peak_bytes'] = max((p['rss_peak_bytes'] for p in processes), default=0)
            failures = [p['reason'] for p in processes if p['status'] == 'FAIL']
            if failures:
                record['status'] = 'FAIL'
                record['reason'] = '; '.join(failures)
        results.append(record)
        (root/'results.json').write_text(json.dumps(results, indent=2))
        print('{} {} peak={} bytes {:.2f}s{}'.format(record['status'], name,
              record.get('rss_peak_bytes', 'unavailable'), record['elapsed_seconds'],
              ': '+record['reason'] if 'reason' in record else ''), flush=True)
    return int(any(item['status']=='FAIL' for item in results))


if __name__ == '__main__':
    raise SystemExit(main())
