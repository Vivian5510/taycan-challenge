#!/usr/bin/env python3
"""Standalone performance matrix. Public correctness tests use tests/run_correctness.py."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time

PROJECT = Path(__file__).resolve().parents[3]
SDK = PROJECT / 'code' / 'taycan_participant_sdk'
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests'))
from process_runner import ProcessRunner

MIB = 1 << 20
GIB = 1 << 30
EVALUATION = json.loads((SDK / 'evaluation-config.json').read_text())
WORKLOADS = ('load', 'read', 'update', 'ycsb_a', 'contest_mix', 'latency_80_20', 'recovery')
RATIOS = {
    'load': [0, 100, 0], 'read': [100, 0, 0], 'update': [0, 100, 0],
    'ycsb_a': [50, 50, 0], 'contest_mix': [60, 30, 10], 'latency_80_20': [80, 20, 0],
}
PROFILES = {
    'smoke': dict(num_keys=1000, duration_seconds=1, warmup_seconds=0, repeats=1,
                  threads=[1], mem_limit_bytes=256*MIB,
                  workloads=['load', 'read', 'update', 'ycsb_a', 'contest_mix', 'latency_80_20']),
    'dev': dict(num_keys=10000, duration_seconds=15, warmup_seconds=2, repeats=1,
                threads=[1, 2, 4, 8], mem_limit_bytes=256*MIB,
                workloads=['load', 'read', 'ycsb_a', 'contest_mix']),
    'reference': dict(num_keys=1000000, duration_seconds=30, warmup_seconds=5, repeats=3,
                      threads=[1, 2, 4, 8], mem_limit_bytes=256*MIB,
                      workloads=['load', 'read', 'update', 'ycsb_a', 'contest_mix']),
    'pressure': dict(num_keys=16000000, duration_seconds=600, warmup_seconds=10, repeats=3,
                     threads=[1, 2, 4, 8], mem_limit_bytes=256*MIB,
                     workloads=['load', 'read', 'ycsb_a', 'contest_mix']),
    'contest-load': dict(num_keys=EVALUATION['num_keys'], duration_seconds=1, warmup_seconds=0, repeats=1,
                         threads=[32], mem_limit_bytes=64*GIB, workloads=['load']),
    'contest-4g': dict(num_keys=EVALUATION['num_keys'], duration_seconds=600, warmup_seconds=10, repeats=1,
                       threads=[32], mem_limit_bytes=EVALUATION['memory_tiers']['4g']['mem_limit_bytes'],
                       workloads=['contest_mix']),
    'contest-16g': dict(num_keys=EVALUATION['num_keys'], duration_seconds=600, warmup_seconds=10, repeats=1,
                        threads=[32], mem_limit_bytes=EVALUATION['memory_tiers']['16g']['mem_limit_bytes'],
                        workloads=['contest_mix']),
    'contest-recovery-4g': dict(num_keys=EVALUATION['num_keys'], duration_seconds=1, warmup_seconds=0, repeats=1,
                               threads=[1], mem_limit_bytes=EVALUATION['memory_tiers']['4g']['mem_limit_bytes'],
                               workloads=['recovery']),
    'contest-recovery-16g': dict(num_keys=EVALUATION['num_keys'], duration_seconds=1, warmup_seconds=0, repeats=1,
                                threads=[1], mem_limit_bytes=EVALUATION['memory_tiers']['16g']['mem_limit_bytes'],
                                workloads=['recovery']),
}


def csv_numbers(text):
    try:
        numbers = [int(part) for part in text.split(',')]
    except ValueError:
        raise argparse.ArgumentTypeError('expected comma-separated integers')
    if not numbers or len(set(numbers)) != len(numbers) or any(n < 1 or n > 128 for n in numbers):
        raise argparse.ArgumentTypeError('thread counts must be unique integers in 1..128')
    return numbers


def csv_workloads(text):
    items = text.split(',')
    if not items or len(set(items)) != len(items) or any(item not in WORKLOADS for item in items):
        raise argparse.ArgumentTypeError('unknown or duplicate workload')
    return items


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def linked_libraries(binary):
    command = ['otool', '-L', str(binary)] if platform.system() == 'Darwin' else ['ldd', str(binary)]
    listing = subprocess.run(command, capture_output=True, text=True, timeout=30, check=True)
    libraries = []
    for line in listing.stdout.splitlines():
        words = line.split()
        paths = [Path(word) for word in words if word.startswith('/')]
        if paths and paths[0].is_file():
            libraries.append(dict(path=str(paths[0].resolve()), sha256=sha256(paths[0])))
    return libraries


def save(path, report):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    temporary.replace(path)


def competitors():
    """Detect other Taycan suites before measuring, without stopping them."""
    found = []
    if platform.system() == 'Darwin':
        from native_platform import native_processes
        return native_processes(('run_tests.py', 'run_correctness.py', 'run_performance.py',
                                 'taycan_kv_bench',
                                 'taycan_selftest', 'taycan_collision_selftest', 'crash_worker',
                                 'basic_test', 'sequential_test', 'random_test', 'overwrite_test',
                                 'delete_test', 'mixed_test', 'recovery_test', 'concurrent_test',
                                 'memory_limit_test', 'demand_load_test', 'checkpoint_test', 'compaction_test'))
    for process in Path('/proc').iterdir():
        if not process.name.isdigit() or int(process.name) == os.getpid():
            continue
        try:
            parts = (process/'cmdline').read_bytes().decode(errors='replace').split('\0')
        except OSError:
            continue
        if any(Path(arg).name in ('run_tests.py', 'run_correctness.py', 'run_performance.py',
                                 'taycan_kv_bench') or
               (Path(arg).name.endswith('_test') and 'taycan' in arg) for arg in parts if arg):
            found.append(dict(pid=int(process.name), command=parts))
    return found


def records(log):
    result = []
    if log.exists():
        for line in log.read_text(errors='replace').splitlines():
            try:
                item = json.loads(line)
                if isinstance(item, dict):
                    result.append(item)
            except ValueError:
                pass
    return result


def directory_bytes(path):
    total = 0
    try:
        for item in path.rglob('*'):
            if item.is_file():
                total += item.stat().st_size
    except OSError:
        pass
    return total


def print_live_progress(folder, db, worker, cell, limit, started, stop, interval, output_path=None):
    """Tail completed-operation events; keep presentation outside engine workers."""
    stages = dict(starting='启动', open='打开', prepare='装载', warmup='预热',
                  measure='测量', close='关闭')
    stage, snapshot = 'starting', None
    last_print, last_disk, disk, received = 0.0, 0.0, 0, 0.0
    partial = ''
    with (output_path or folder / 'output.log').open(errors='replace') as source:
        while True:
            chunk = source.read()
            partial += chunk
            lines = partial.split('\n')
            partial = lines.pop()
            changed = False
            final = False
            for line in lines:
                try:
                    event = json.loads(line)
                except ValueError:
                    continue
                if not isinstance(event, dict):
                    continue
                if event.get('type') == 'stage':
                    next_stage = event.get('stage', stage)
                    if next_stage != stage:
                        stage, snapshot, changed = next_stage, None, True
                elif event.get('type') == 'progress' and event.get('stage') == stage:
                    snapshot = event
                    received = time.monotonic()
                    final = event.get('final', False)
                    if final:
                        count = '{:,}'.format(event['completed'])
                        if event.get('total'):
                            count += '/{:,}'.format(event['total'])
                        print('[阶段完成] {} | {} | 完成 {} | {:.3f}s'.format(
                            cell, stages.get(stage, stage), count, event['elapsed_seconds']), flush=True)
                elif event.get('type') == 'error':
                    print('[引擎错误] {} {}'.format(cell, event.get('error')), flush=True)
            now = time.monotonic()
            if changed or final or (not stop.is_set() and now-last_print >= interval):
                if changed or now-last_disk >= 5.0:
                    disk, last_disk = directory_bytes(db), now
                try:
                    from process_runner import read_rss
                    rss = (worker.rss_reader or read_rss)(worker.proc.pid)
                except (OSError, ValueError):
                    rss = None
                rate = '—'
                completed = '—'
                if snapshot:
                    rate = '{:,.0f}'.format(snapshot['ops_per_second'])
                    completed = '{:,}'.format(snapshot['completed'])
                    if snapshot.get('total'):
                        total = snapshot['total']
                        completed += '/{:,} ({:.1%})'.format(total, snapshot['completed']/total)
                    if now-received > interval*2:
                        rate += '（最近采样）'
                print('[运行中] {} | {} | 总耗时 {:.1f}s | 完成 {} | 区间 {} ops/s | RSS {} / 峰值 {:.1f} / 上限 {:.1f} MiB | 文件 {:.1f} MiB'.format(
                    cell, stages.get(stage, stage), now-started, completed, rate,
                    '{:.1f}'.format(rss/MIB) if rss is not None else '—',
                    worker.peak/MIB, limit/MIB, disk/MIB), flush=True)
                last_print = now
            if stop.is_set():
                break
            stop.wait(min(.2, interval))


def validate(measurement, workload, count):
    if measurement.get('status') != 'PASS':
        raise RuntimeError(measurement.get('error', 'benchmark reported failure'))
    operations = measurement.get('operations', 0)
    elapsed = measurement.get('measured_seconds', 0)
    throughput = measurement.get('ops_per_second', 0)
    if operations <= 0 or elapsed <= 0 or not math.isfinite(throughput):
        raise RuntimeError('empty or invalid measurement')
    if not math.isclose(throughput, operations/elapsed, rel_tol=1e-6):
        raise RuntimeError('throughput does not match completed operations / measured time')
    if sum(measurement[op] for op in ('gets', 'puts', 'deletes')) != operations:
        raise RuntimeError('operation counters do not match the completed count')
    if workload == 'recovery':
        if operations != 300 or measurement.get('gets') != 200 or measurement.get('puts') != 100:
            raise RuntimeError('recovery measurement did not complete 100 reads, 100 writes and 100 readbacks')
        recovery_seconds = measurement.get('recovery_seconds')
        if not recovery_seconds or not math.isfinite(recovery_seconds):
            raise RuntimeError('recovery duration is missing or invalid')
        if recovery_seconds < measurement.get('open_seconds', 0):
            raise RuntimeError('recovery duration excludes Open')
    if workload == 'load' and operations != count:
        raise RuntimeError('load did not complete the requested distinct keys')
    if workload in ('read', 'ycsb_a', 'latency_80_20') and measurement.get('read_misses'):
        raise RuntimeError('unexpected misses in a workload without deletes')


def run_monitored_process(command, output_path, process_report, folder, db, cell,
                          rss_limit, started, args, monitor_kwargs):
    progress_stop = threading.Event()
    progress = None
    try:
        with output_path.open('w') as log:
            with ProcessRunner(command, log, rss_limit, args.timeout_seconds,
                               args.sample_interval_ms/1000, process_report,
                               **monitor_kwargs) as worker:
                if args.progress_interval_seconds:
                    progress = threading.Thread(
                        target=print_live_progress,
                        args=(folder, db, worker, cell, rss_limit, started, progress_stop,
                              args.progress_interval_seconds, output_path), daemon=True)
                    progress.start()
                worker.finish()
    finally:
        progress_stop.set()
        if progress is not None:
            progress.join()
    return records(output_path), records(process_report)


def clear_page_cache():
    if platform.system() != 'Linux':
        raise RuntimeError('page-cache clearing is supported only on Linux')
    try:
        subprocess.run(['sync'], check=True, timeout=300)
        Path('/proc/sys/vm/drop_caches').write_text('3\n')
    except (OSError, subprocess.SubprocessError) as error:
        raise RuntimeError('cannot clear Linux page cache; run with the required privileges') from error


def evaluation_gate(profile, runs, repeats, mode, page_cache_cleared):
    requirements = {
        'contest-load': ('data_preparation', EVALUATION['performance']['data_preparation']),
        'contest-4g': ('steady_state_4g', EVALUATION['performance']['steady_state']),
        'contest-16g': ('steady_state_16g', EVALUATION['performance']['steady_state']),
    }
    if profile not in requirements:
        return dict(status='NOT_APPLICABLE')
    if mode != 'throughput':
        return dict(status='NOT_APPLICABLE', reason='latency runs do not validate throughput gates')
    phase, requirement = requirements[profile]
    valid = [run['measurement']['ops_per_second'] for run in runs
             if run.get('status') == 'PASS' and run.get('measurement')]
    result = dict(phase=phase, required_average_ops_per_second=requirement['minimum_average_ops_per_second'],
                  completed_repeats=len(valid), requested_repeats=repeats,
                  measured_average_ops_per_second=sum(valid)/len(valid) if valid else None,
                  page_cache_cleared_before_each_cell=page_cache_cleared)
    if not page_cache_cleared:
        result['status'] = 'INCOMPLETE'
        result['reason'] = 'page-cache policy does not match the published performance procedure'
    elif len(valid) != repeats:
        result['status'] = 'INCOMPLETE'
    else:
        result['status'] = ('PASS' if result['measured_average_ops_per_second'] >=
                            result['required_average_ops_per_second'] else 'FAIL')
    return result


def render_report(report, destination):
    lines = ['# KV 性能测量', '',
             '被测引擎：{}。'.format(report.get('tested_engine', 'contestant_engine')), '',
             '状态：{}；配置：{}；模式：{}。'.format(report['status'], report['profile'], report['mode']), '',
             '仅为本地性能基准，不是出题方正式评分。装载、预热、Open、关闭独立报告。', '',
             '| 工作负载 | 线程 | 重复 | 状态 | ops/s | P99 µs | RSS MiB | 装载 s | 恢复 s |',
             '|---|---:|---:|---|---:|---:|---:|---:|---:|']
    for run in report['runs']:
        m = run.get('measurement', {})
        p99 = m.get('latency', {}).get('p99_us')
        recovery_seconds = m.get('recovery_seconds')
        lines.append('| {} | {} | {} | {} | {} | {} | {:.2f} | {} | {} |'.format(
            run['workload'], run['threads'], run['repeat'], run['status'],
            '{:.2f}'.format(m['ops_per_second']) if m else '—',
            '{:.2f}'.format(p99) if p99 is not None else '未采样', run.get('rss_peak_bytes', 0)/MIB,
            '{:.3f}'.format(m['setup_seconds']) if m else '—',
            '{:.6f}'.format(recovery_seconds) if recovery_seconds is not None else '—'))
        if run.get('preparation_status'):
            prep = run.get('preparation_measurement', {})
            lines.append('| 数据准备 | {} | 1 | {} | {} | 未采样 | {:.2f} | — | — |'.format(
                run.get('preparation_threads', 1),
                run['preparation_status'],
                '{:.2f}'.format(prep['ops_per_second']) if prep else '—',
                run.get('preparation_rss_peak_bytes', 0)/MIB))
        if run.get('reason'):
            lines.extend(['', '{}：{}'.format(run['cell'], run['reason']), ''])
    if report.get('shared_host_processes'):
        lines.extend(['', '本轮与其他 Taycan 进程共享 CPU/磁盘；仅用于验证测量工具，不能用于版本性能对比。'])
    gate = report.get('evaluation_gate', {})
    if gate.get('status') != 'NOT_APPLICABLE' and 'required_average_ops_per_second' in gate:
        measured = gate.get('measured_average_ops_per_second')
        lines.extend(['', '题面性能门槛：{}；要求平均 {:,.0f} ops/s；本轮平均 {}。'.format(
            gate['status'], gate['required_average_ops_per_second'],
            '{:,.2f} ops/s'.format(measured) if measured is not None else '未完成')])
        if gate.get('reason'):
            lines.append('门槛状态说明：{}。'.format(gate['reason']))
    lines.extend(['', '持久化口径：{}。'.format(report.get('durability_status', '未标注')),
                  '完整参数、各操作延迟分位数、文件大小、I/O 计数、命令、哈希及原始日志见 results.json。'])
    destination.write_text('\n'.join(lines) + '\n')


def durability_status(identity):
    """Describe the linked adapter policy; throughput validation is not a crash proof."""
    engine = identity.get('engine', '')
    policy = identity.get('policy', '').lower()
    if engine == 'f2_native_completion_baseline':
        return 'NOT_COMPLIANT: native F2 operation completion does not guarantee per-operation durable acknowledgement'
    if engine in ('f2_sync_checkpoint', 'f2_sync_commit-log', 'f2_sync_hybridlog', 'f2_sync_rocksdb-wal', 'f2_sync_data-log'):
        return 'CONFIGURED: F2 internal synchronous commit; crash evidence is required separately for this build'
    if engine == 'rocksdb' and 'sync=true' in policy and 'disablewal=false' in policy:
        return 'CONFIGURED: RocksDB sync=true and WAL enabled; still require crash tests on the tested build'
    if 'durable' in policy or 'fdatasync' in policy or 'fullfsync' in policy:
        return 'IMPLEMENTATION_CLAIM: linked engine policy advertises synchronous durability; inspect and run the crash suite'
    return 'UNVERIFIED: benchmark throughput does not establish per-operation durable acknowledgement'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=PROFILES, default='contest-4g')
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--native-development', action='store_true',
                        help='Enable macOS diagnostic metrics; never an official Linux acceptance result')
    parser.add_argument('--expect-engine', help='Fail if the linked binary identifies a different engine')
    parser.add_argument('--results-dir', help='Use a new directory on the guest local disk')
    parser.add_argument('--data-root', help='Guest local directory for temporary databases; default: results directory')
    parser.add_argument('--workloads', type=csv_workloads)
    parser.add_argument('--threads', type=csv_numbers)
    for name in ('num-keys', 'duration-seconds', 'warmup-seconds', 'repeats', 'mem-limit-bytes'):
        parser.add_argument('--'+name, type=int)
    parser.add_argument('--key-size', type=int, default=EVALUATION['key_size_bytes']['default'])
    parser.add_argument('--value-size', type=int, default=EVALUATION['value_size_bytes'])
    parser.add_argument('--seed', type=int, default=20261021)
    parser.add_argument('--mode', choices=('throughput', 'latency'), default='throughput')
    parser.add_argument('--sample-every', type=int, help='Latency mode: one sample per N calls on average; default 1')
    parser.add_argument('--distribution', choices=('uniform', 'hotset'), default='uniform')
    parser.add_argument('--timeout-seconds', type=float, default=86400)
    parser.add_argument('--sample-interval-ms', type=float, default=20,
                        help='External VmRSS sampling; public correctness runner retains its original 2 ms')
    parser.add_argument('--progress-interval-seconds', type=int, default=1,
                        help='Print live completed-operation counts and resources every N seconds; 0 disables live output')
    parser.add_argument('--keep-data', action='store_true')
    parser.add_argument('--keep-going', action='store_true')
    parser.add_argument('--allow-shared-host', action='store_true', help='Harness checks only: label results as contended')
    parser.add_argument('--clear-page-cache', action='store_true',
                        help='Clear the Linux OS page cache before each measured cell; requires system-level privileges')
    parser.add_argument('--dry-run', action='store_true', help='Print resolved parameters and number of cells; run nothing')
    args = parser.parse_args(argv)
    if args.profile == 'contest-load' and args.mode != 'throughput':
        parser.error('contest-load evaluates the 60,000 ops/s throughput gate; latency mode is not applicable')
    if args.profile.startswith('contest-recovery-') and args.mode != 'throughput':
        parser.error('contest recovery is a timed Open-and-operations scenario; latency sampling is not applicable')
    if args.clear_page_cache and platform.system() != 'Linux' and not args.dry_run:
        parser.error('--clear-page-cache requires Linux')
    params = dict(PROFILES[args.profile])
    for key in params:
        value = getattr(args, key, None)
        if value is not None:
            params[key] = value
    params.update(key_size=args.key_size, value_size=args.value_size, seed=args.seed,
                  distribution=args.distribution, sample_every=0 if args.mode == 'throughput' else (
                      args.sample_every if args.sample_every is not None else 1))
    if args.mode == 'throughput' and args.sample_every is not None:
        parser.error('--sample-every applies only to --mode latency')
    if not (1 <= params['num_keys'] <= 200000000 and 1 <= params['duration_seconds'] <= 86400 and
            0 <= params['warmup_seconds'] <= 3600 and 1 <= params['repeats'] <= 100 and
            8 <= args.key_size <= MIB and 16 <= args.value_size <= 16*MIB and
            params['mem_limit_bytes'] >= 16*MIB and params['sample_every'] >= 0 and
            0 <= args.seed <= (1 << 64)-1 and math.isfinite(args.timeout_seconds) and args.timeout_seconds > 0 and
            math.isfinite(args.sample_interval_ms) and args.sample_interval_ms > 0 and
            0 <= args.progress_interval_seconds <= 3600):
        parser.error('invalid count, size, memory, sampling, or timeout parameter')
    contest_profile = args.profile.startswith('contest-')
    if contest_profile:
        recovery_profile = args.profile.startswith('contest-recovery-')
        tier_name = args.profile[len('contest-recovery-'):] if recovery_profile else args.profile[len('contest-'):]
        expected_mem = (64*GIB if args.profile == 'contest-load' else
                        EVALUATION['memory_tiers'][tier_name]['mem_limit_bytes'])
        expected_workload = (['load'] if args.profile == 'contest-load' else
                             ['recovery'] if recovery_profile else ['contest_mix'])
        expected_threads = [1] if recovery_profile else [EVALUATION['performance']['threads']]
        expected_duration = 1 if args.profile == 'contest-load' or recovery_profile else 600
        if (params['num_keys'] != EVALUATION['num_keys'] or
                params['mem_limit_bytes'] != expected_mem or
                params['threads'] != expected_threads or
                params['workloads'] != expected_workload or
                params['duration_seconds'] != expected_duration or
                ((args.profile == 'contest-load' or recovery_profile) and params['warmup_seconds'] != 0) or
                not EVALUATION['key_size_bytes']['min'] <= params['key_size'] <= EVALUATION['key_size_bytes']['max'] or
                params['value_size'] != EVALUATION['value_size_bytes']):
            parser.error('contest profiles fix official key count, thread count, workload, memory tier, duration, key range and value size; use an explicit diagnostic profile for other parameters')
    if args.mode == 'latency' and params['sample_every'] == 0:
        parser.error('latency mode requires a positive --sample-every')
    if args.dry_run:
        print(json.dumps(dict(profile=args.profile, mode=args.mode, parameters=params,
                              clear_page_cache_before_each_cell=args.clear_page_cache,
                              cells=len(params['threads'])*len(params['workloads'])*params['repeats']), indent=2))
        return 0
    if args.profile.startswith('contest-') and (platform.system() != 'Linux' or platform.machine() != 'x86_64'):
        parser.error('contest profiles require Linux x86_64; use dev/reference/pressure for local diagnostics')
    if args.profile.startswith('contest-') and not args.clear_page_cache and not args.dry_run:
        parser.error('contest profiles require --clear-page-cache to reproduce the published per-round cache policy')
    if not Path('/proc/self/status').exists():
        if not args.native_development or platform.system() != 'Darwin':
            parser.error('Linux /proc required, or explicitly use --native-development on macOS')
    monitor_kwargs = {}
    if platform.system() == 'Darwin':
        from native_platform import read_macos_rss
        if not read_macos_rss(os.getpid()):
            parser.error('native RSS reader returned no sample for the controller')
        monitor_kwargs = dict(rss_reader=read_macos_rss,
                              rss_source='macOS libproc PROC_PIDTASKINFO resident bytes; not Linux VmRSS')
    binary = Path(args.build_dir).resolve()/'taycan_kv_bench'
    if not binary.is_file():
        parser.error('build taycan_kv_bench with -DTAYCAN_BUILD_PERF_BENCHMARKS=ON')
    identity = json.loads(subprocess.check_output([str(binary), '--describe'], text=True, timeout=30))
    if args.expect_engine and identity['engine'] != args.expect_engine:
        parser.error('binary engine identity differs from --expect-engine')
    sharing = competitors()
    if sharing and not args.allow_shared_host:
        parser.error('another Taycan evaluation is running; wait for completion before collecting performance data')
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    root = Path(args.results_dir).resolve() if args.results_dir else Path.cwd()/('performance-'+stamp)
    root.mkdir(parents=True, exist_ok=False)
    data_root = Path(args.data_root).resolve() if args.data_root else root
    data_root.mkdir(parents=True, exist_ok=True)
    minimum = params['num_keys']*(params['key_size']+params['value_size'])
    if shutil.disk_usage(data_root).free < minimum + 256*MIB:
        parser.error('insufficient local disk even for logical Key/Value bytes plus 256 MiB; parameters were not reduced')
    local_engines = {
        'f2_native_completion_baseline': PROJECT/'code/faster-native-adapter',
        'f2_sync_checkpoint': PROJECT/'code/faster-native-adapter',
        'f2_sync_commit-log': PROJECT/'code/faster-native-adapter',
        'f2_sync_hybridlog': PROJECT/'code/faster-native-adapter',
        'f2_sync_rocksdb-wal': PROJECT/'code/faster-native-adapter',
        'f2_sync_data-log': PROJECT/'code/faster-native-adapter',
    }
    engine_source = (SDK/'engine' if identity['engine'] == 'contestant_engine' else
                     local_engines.get(identity['engine'], SDK/'benchmarks'/'baselines'/identity['engine']))
    if not engine_source.is_dir():
        parser.error('engine source is unavailable or retired: ' + identity['engine'])
    source_root = PROJECT if identity['engine'] in local_engines else SDK
    source_hashes = {str(path.relative_to(source_root)): sha256(path) for path in sorted(engine_source.rglob('*'))
                     if path.is_file() and path.suffix in ('.cc', '.h', '.inc', '.py', '.txt')}
    if identity['engine'] in ('f2_sync_rocksdb-wal','f2_sync_data-log'):
        for path in sorted((PROJECT/'code/rocksdb-commit-reference').glob('*')):
            if path.is_file():
                source_hashes[str(path.relative_to(PROJECT))] = sha256(path)
    source_hashes['benchmarks/kv_bench.cc'] = sha256(Path(__file__).with_name('kv_bench.cc'))
    source_hashes['evaluation-config.json'] = sha256(SDK / 'evaluation-config.json')
    report = dict(schema_version=2, status='RUNNING', classification='local performance; not official scoring',
                  native_development=args.native_development,
                  acceptance_status='NOT_RUN: official acceptance requires Linux x86_64',
                  tested_engine=identity['engine'], engine_identity=identity, cross_engine_comparison=False,
                  comparison_policy=json.loads(Path(__file__).with_name('baseline-policy.json').read_text()),
                  profile=args.profile, mode=args.mode, parameters=params, binary_sha256=sha256(binary),
                  linked_libraries=linked_libraries(binary),
                  monitor_parameters=dict(sample_interval_ms=args.sample_interval_ms,
                                          progress_interval_seconds=args.progress_interval_seconds,
                                          rss_limit_rule=('floor(1.1 * budget); sampled' if contest_profile
                                                          else 'floor(1.1 * budget) + 8 MiB; sampled'),
                                          platform='macOS libproc resident bytes' if monitor_kwargs else 'Linux /proc VmRSS'),
                  source_hashes=source_hashes, shared_host_processes=sharing, started_at_utc=stamp,
                  environment=dict(platform=platform.platform(), machine=platform.machine(),
                                   cpu_count=os.cpu_count(), python=platform.python_version()),
                  durability_requirement='KVEngine contract: successful Put/Delete must already be durable; no relaxed mode',
                  durability_status=durability_status(identity),
                  arrival_model='closed loop; at most one synchronous API call per worker; no coordinated-omission correction',
                  cache_policy=('Linux OS page cache cleared before every cell; no clearing during measurement'
                                if args.clear_page_cache else
                                'ordinary OS page cache retained; warmup per cell; evaluator page-cache policy not reproduced'),
                  latency_method='bounded histogram, 64 sub-buckets per power of two; percentile upper bounds, <1.563% quantization',
                  evaluation_gate=evaluation_gate(args.profile, [], params['repeats'], args.mode,
                                                  args.clear_page_cache),
                  runs=[], summary=[])
    save(root/'results.json', report)
    print('结果目录: '+str(root), flush=True)
    print('引擎: {} | 持久化口径: {}'.format(identity['engine'], report['durability_status']), flush=True)
    print('评测参数: profile={} mode={} keys={} key={}B value={}B budget={}MiB threads={} workloads={} repeats={}'.format(
        args.profile, args.mode, params['num_keys'], params['key_size'], params['value_size'],
        params['mem_limit_bytes'] / MIB, ','.join(map(str, params['threads'])),
        ','.join(params['workloads']), params['repeats']), flush=True)
    failure = False
    total_cells = len(params['workloads'])*len(params['threads'])*params['repeats']
    for workload in params['workloads']:
        for threads in params['threads']:
            for repeat in range(1, params['repeats']+1):
                cell = '{}-t{}-r{}'.format(workload, threads, repeat)
                folder = root/cell
                folder.mkdir()
                db = Path(tempfile.mkdtemp(prefix=cell+'-', dir=str(data_root)))
                command = [str(binary), '--data-dir='+str(db), '--workload='+workload]
                for key in ('num_keys', 'duration_seconds', 'warmup_seconds', 'key_size', 'value_size',
                            'mem_limit_bytes', 'seed', 'sample_every', 'distribution'):
                    command.append('--{}={}'.format(key.replace('_', '-'), params[key]))
                command.append('--threads='+str(threads))
                command.append('--progress-interval-seconds='+str(args.progress_interval_seconds))
                limit = params['mem_limit_bytes']*11//10 + (0 if contest_profile else 8*MIB)
                run = dict(cell=cell, workload=workload, ratio_get_put_delete=RATIOS.get(workload), threads=threads,
                           repeat=repeat, command=command, status='FAIL', data_dir=str(db),
                           log=str(folder/'output.log'), rss_limit_bytes=limit)
                recovery_workload = workload == 'recovery'
                separate_preparation = recovery_workload
                preparation_budget = params['mem_limit_bytes']
                preparation_threads = (EVALUATION['performance']['threads']
                                       if args.profile.startswith('contest-recovery-')
                                       else threads)
                preparation_command = None
                preparation_log = folder/'prepare.log'
                preparation_processes = folder/'prepare-processes.jsonl'
                preparation_limit = preparation_budget*11//10 + (0 if contest_profile else 8*MIB)
                if separate_preparation:
                    preparation_command = [str(binary), '--data-dir='+str(db), '--workload=load',
                                           '--num-keys='+str(params['num_keys']), '--duration-seconds=1',
                                           '--warmup-seconds=0', '--key-size='+str(params['key_size']),
                                           '--value-size='+str(params['value_size']),
                                           '--mem-limit-bytes='+str(preparation_budget), '--seed='+str(params['seed']),
                                           '--sample-every=0', '--distribution=uniform',
                                           '--threads='+str(preparation_threads),
                                           '--progress-interval-seconds='+str(args.progress_interval_seconds)]
                    run.update(preparation_command=preparation_command,
                               preparation_log=str(preparation_log),
                               preparation_rss_limit_bytes=preparation_limit,
                               preparation_threads=preparation_threads,
                               preparation_mem_limit_bytes=preparation_budget,
                               recovery_thread_count=threads)
                    command.append('--prepared-dataset')
                report['active_cell'] = cell
                save(root/'results.json', report)
                started = time.monotonic()
                print('\n[开始 {}/{}] {} 线程={} 重复={} workload={} 数据目录={}'.format(
                    len(report['runs'])+1, total_cells, cell, threads, repeat, workload, db), flush=True)
                try:
                    if args.clear_page_cache and not separate_preparation:
                        clear_page_cache()
                        run['page_cache_cleared_before_cell'] = True
                    if separate_preparation:
                        preparation_started = time.monotonic()
                        prepared, preparation_monitor = run_monitored_process(
                            preparation_command, preparation_log, preparation_processes, folder, db,
                            cell+'-data-preparation', preparation_limit, preparation_started, args, monitor_kwargs)
                        preparation_results = [item for item in prepared if item.get('type') == 'result']
                        if len(preparation_results) != 1:
                            raise RuntimeError('expected exactly one data-preparation result')
                        run['preparation_measurement'] = preparation_results[0]
                        validate(preparation_results[0], 'load', params['num_keys'])
                        run['preparation_status'] = 'PASS'
                        run['preparation_wall_seconds'] = time.monotonic()-preparation_started
                        run['preparation_rss_peak_bytes'] = (max(item['rss_peak_bytes'] for item in preparation_monitor)
                                                             if preparation_monitor else 0)
                        if args.clear_page_cache:
                            clear_page_cache()
                            run['page_cache_cleared_before_measurement'] = True
                    measured_records, monitor_records = run_monitored_process(
                        command, folder/'output.log', folder/'processes.jsonl', folder, db, cell,
                        limit, started, args, monitor_kwargs)
                    measured = [item for item in measured_records if item.get('type') == 'result']
                    if len(measured) != 1:
                        raise RuntimeError('expected exactly one completed measurement')
                    run['measurement'] = measured[0]
                    run['measurement_rss_peak_bytes'] = (max(item['rss_peak_bytes'] for item in monitor_records)
                                                         if monitor_records else 0)
                    validate(measured[0], workload, params['num_keys'])
                    run['status'] = 'PASS'
                except (Exception, KeyboardInterrupt) as error:
                    run['reason'] = str(error) or 'interrupted'
                    failure = True
                run['wall_seconds'] = time.monotonic()-started
                parsed = records(folder/'output.log')
                if not run.get('measurement'):
                    candidates = [item for item in parsed if item.get('type') == 'result']
                    if candidates:
                        run['measurement'] = candidates[-1]
                errors = [item['error'] for item in parsed if item.get('type') == 'error']
                if errors:
                    run['worker_errors'] = errors
                    if run['status'] != 'PASS':
                        run['reason'] = '; '.join([run.get('reason', '')] + errors).strip('; ')
                monitor = records(folder/'processes.jsonl')
                preparation_monitor = records(preparation_processes) if recovery_workload else []
                all_monitor_records = preparation_monitor + monitor
                if all_monitor_records:
                    run['rss_peak_bytes'] = max(item['rss_peak_bytes'] for item in all_monitor_records)
                    run['monitor'] = all_monitor_records[-1]
                if recovery_workload:
                    prepare_output = records(preparation_log)
                    if 'preparation_measurement' not in run:
                        preparation_results = [item for item in prepare_output if item.get('type') == 'result']
                        if preparation_results:
                            run['preparation_measurement'] = preparation_results[-1]
                    if 'preparation_status' not in run:
                        prep_errors = [item.get('error') for item in prepare_output if item.get('type') == 'error']
                        run['preparation_status'] = 'FAIL' if prep_errors else 'INCOMPLETE'
                        if prep_errors:
                            run['preparation_errors'] = prep_errors
                report['runs'].append(run)
                save(root/'results.json', report)
                render_report(report, root/'README.md')
                m = run.get('measurement', {})
                print('[完成] {:<20} status={} ops/s={} setup={}s measure={}s recovery={}s close={}s RSS峰值={}MiB 文件(测前/测后/关闭)={}{}\n'.format(
                    cell, run['status'], m.get('ops_per_second', 'unavailable'),
                    m.get('setup_seconds', '—'), m.get('measured_seconds', '—'),
                    m.get('recovery_seconds', '—'), m.get('close_seconds', '—'),
                    '{:.1f}'.format(run.get('rss_peak_bytes', 0) / MIB) if run.get('rss_peak_bytes') else '—',
                    '/'.join('{:.1f}'.format(m[k]/MIB) if m.get(k) is not None else '—'
                             for k in ('data_bytes_before_measure', 'data_bytes_after_measure', 'data_bytes_after_close'))+' MiB',
                    ' reason='+run['reason'] if run.get('reason') else ''), flush=True)
                if run['status'] == 'PASS' and not args.keep_data:
                    shutil.rmtree(db)
                if failure and not args.keep_going:
                    break
            if failure and not args.keep_going:
                break
        if failure and not args.keep_going:
            break
    report.pop('active_cell', None)
    report['status'] = 'FAIL' if failure else 'PASS'
    report['evaluation_gate'] = evaluation_gate(args.profile, report['runs'], params['repeats'], args.mode,
                                                args.clear_page_cache)
    report['completed_at_utc'] = datetime.now(timezone.utc).isoformat()
    for workload in params['workloads']:
        for threads in params['threads']:
            valid = [r['measurement']['ops_per_second'] for r in report['runs']
                     if r['workload'] == workload and r['threads'] == threads and r['status'] == 'PASS']
            report['summary'].append(dict(workload=workload, threads=threads, completed_repeats=len(valid),
                                         requested_repeats=params['repeats'],
                                         median_ops_per_second=statistics.median(valid) if valid else None,
                                         min_ops_per_second=min(valid) if valid else None,
                                         max_ops_per_second=max(valid) if valid else None,
                                         median_recovery_seconds=(statistics.median([
                                             r['measurement']['recovery_seconds'] for r in report['runs']
                                             if r['workload'] == workload and r['threads'] == threads and
                                             r['status'] == 'PASS' and 'recovery_seconds' in r['measurement']])
                                             if workload == 'recovery' and valid else None)))
    save(root/'results.json', report)
    render_report(report, root/'README.md')
    print('\n[汇总] {} | 已执行 {}/{} 单元 | PASS={} FAIL={} 未执行={}'.format(
        report['status'], len(report['runs']), total_cells,
        sum(r['status']=='PASS' for r in report['runs']), sum(r['status']=='FAIL' for r in report['runs']),
        total_cells-len(report['runs'])), flush=True)
    print('{:<18} {:>6} {:>12} {:>18}'.format('workload', '线程', '有效重复', 'median ops/s'), flush=True)
    for item in report['summary']:
        print('{:<18} {:>6} {:>12} {:>18}'.format(
            item['workload'], item['threads'], '{}/{}'.format(item['completed_repeats'], item['requested_repeats']),
            '{:,.0f}'.format(item['median_ops_per_second']) if item['median_ops_per_second'] is not None else '—'), flush=True)
    print('报告: '+str(root/'README.md'), flush=True)
    if report['evaluation_gate']['status'] != 'NOT_APPLICABLE':
        gate = report['evaluation_gate']
        print('题面性能门槛: {} | 平均值={} ops/s | 要求>={:,.0f} ops/s'.format(
            gate['status'],
            '{:,.2f}'.format(gate['measured_average_ops_per_second'])
            if gate['measured_average_ops_per_second'] is not None else '未完成',
            gate['required_average_ops_per_second']), flush=True)
    gate_failed = report['evaluation_gate']['status'] == 'FAIL'
    return int(failure or gate_failed)


if __name__ == '__main__':
    raise SystemExit(main())
