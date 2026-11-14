#!/usr/bin/env python3
"""SIGKILL recovery with an acknowledged prefix and at most one pending operation."""
import argparse
from collections import namedtuple
import os
from pathlib import Path
import random
import select
import signal
import subprocess
import tempfile
import time
import shutil
from process_runner import ProcessRunner


# Python 通过 stdin 发送 G（执行下一条操作）；worker 通过独立管道回复 READY/ACK。
# acknowledged 是已确认成功的最后一个操作编号，也等于已完成操作数。
ServingWorker = namedtuple(
    "ServingWorker", "monitor read_fd buffer acknowledged")


def worker_command(args):
    return [str(Path(args.worker).resolve()), '--seed=' + str(args.seed),
            '--num_keys=' + str(args.num_keys), '--key_size=' + str(args.key_size),
            '--key_mode=' + args.key_mode,
            '--value_size=' + str(args.value_size),
            '--mem_limit_bytes=' + str(args.mem_limit_bytes)]


def read_line(read_fd, buffer, deadline, proc, log):
    # 管道按字节传输，一次 read 可能只读到半行，也可能读到多行。
    # 按换行符提取一条消息，剩余字节保留到下一次读取或 kill 后处理。
    while b'\n' not in buffer:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([read_fd], [], [], remaining)[0]:
            raise TimeoutError('worker protocol timed out')
        data = os.read(read_fd, 4096)
        if not data:
            raise RuntimeError('worker exited before expected protocol message: exit={}; log={}'
                               .format(proc.wait(timeout=5), log.name))
        buffer.extend(data)
    line, _, rest = buffer.partition(b'\n')
    buffer[:] = rest
    return line.decode('ascii')


def run(args, mode, path, operations, log, possible=0):
    command = worker_command(args) + ['--mode=' + mode, '--db_path=' + str(path),
                      '--operations=' + str(operations),
                      '--possible_operation=' + str(possible)]
    # Keep protocol output on disk; each prepare/verify process is monitored.
    output_path = Path(str(log.name) + '.' + mode + '-' + str(time.monotonic_ns() if hasattr(time, 'monotonic_ns') else int(time.monotonic()*1e9)) + '.stdout')
    with output_path.open('w') as output:
        with ProcessRunner(command, output, args.rss_limit_bytes, args.timeout,
                           args.sample_interval_ms / 1000, args.monitor_report) as monitor:
            monitor.finish()
    return output_path.read_text()


def prepare(args, path, initial, log):
    """Create the initial database and close it normally."""
    # prepare 完成 initial 条操作并正常关闭；serve 会从 initial + 1 继续。
    run(args, 'prepare', path, initial, log)


def serve(args, path, initial, log, rng):
    """Open a worker and execute confirmed updates; keep it alive for kill."""
    # 此管道只传 READY/ACK，普通日志另写文件，避免引擎日志干扰协议。
    read_fd, write_fd = os.pipe()
    proc = None
    monitor = None
    buffer = bytearray()
    deadline = time.monotonic() + args.timeout

    try:
        monitor = ProcessRunner(
            worker_command(args) + [
                '--mode=serve', '--db_path=' + str(path),
                '--operations=' + str(initial),
                '--progress_fd=' + str(write_fd)],
            log, args.rss_limit_bytes, args.timeout,
            args.sample_interval_ms / 1000, args.monitor_report,
            stdin=subprocess.PIPE, pass_fds=(write_fd,))
        proc = monitor.proc
        # 父进程不写回复管道，立即关闭自己持有的写端。
        # 否则 worker 退出后管道仍有写端打开，读端无法收到 EOF。
        os.close(write_fd)
        write_fd = -1
        # READY 表示 worker 已成功打开引擎，可以开始接收操作。
        if read_line(read_fd, buffer, deadline, proc, log) != 'READY':
            raise RuntimeError('invalid worker readiness message')
        acknowledged = initial
        # G 不携带 key/value；worker 按操作编号推导目标 key 和写入版本。
        # Put/Delete 成功返回后才发送 ACK n，表示前 n 条操作已确定成功。
        # ACK 只是成功通知，不执行额外的 sync；持久化由引擎接口保证。
        # 收到完整 ACK 后才发送下一条，因此最多只有一条未确认操作。
        updates = rng.randint(1, args.num_keys)
        for _ in range(updates):
            proc.stdin.write(b'G')
            proc.stdin.flush()
            message = read_line(read_fd, buffer, deadline, proc, log)
            if message != 'ACK ' + str(acknowledged + 1):
                raise RuntimeError('invalid acknowledgement: ' + message)
            acknowledged += 1
        return ServingWorker(monitor, read_fd, buffer, acknowledged)
    except BaseException:
        # The caller cannot clean up until serve has returned a worker.
        if monitor is not None:
            monitor.close()
        if proc is not None:
            proc.stdin.close()
        os.close(read_fd)
        if write_fd >= 0:
            os.close(write_fd)
        raise


def close_worker(worker):
    """Release the serving process and protocol pipe, including on failure."""
    try:
        worker.monitor.close()
    finally:
        try:
            worker.monitor.proc.stdin.close()
        finally:
            os.close(worker.read_fd)


def kill(worker, trial, rng):
    """Send one unconfirmed update, kill the worker, then drain its ACKs."""
    monitor, read_fd, buffer, acknowledged = worker
    proc = monitor.proc
    # 第一轮先暂停 worker，再发送最后一条命令，确保该命令尚未执行就被 kill。
    # 后续轮次不暂停，覆盖写入与 kill 竞争的情况，不依赖引擎内部观察点。
    if trial == 0:
        os.killpg(proc.pid, signal.SIGSTOP)
        _, status = os.waitpid(proc.pid, os.WUNTRACED)
        if not os.WIFSTOPPED(status):
            raise RuntimeError('worker did not stop')
    # 最后再发送一条，但不等待 ACK；只允许这一条的执行结果未知。
    proc.stdin.write(b'G')
    proc.stdin.flush()
    # 前两轮立即 kill，后续轮次随机等待 0～10 ms。
    delay = 0 if trial < 2 else rng.random() * 0.01
    if delay:
        time.sleep(delay)
    monitor.inject_kill()
    monitor.finish(expected_kill=True)
    # kill 不会清空管道：worker 可能已经成功写入并发出 ACK，Python 尚未读取。
    # 等待进程退出后读到 EOF，并与 read_line 留下的 buffer 合并，避免漏掉成功通知。
    while True:
        data = os.read(read_fd, 4096)
        if not data:
            break
        buffer.extend(data)
    # 未收到最后一条的完整 ACK 时，保留 pending=n+1；它可能未执行、执行中，
    # 也可能已成功但来不及发完 ACK。恢复时只对它涉及的 key 允许新旧两种状态。
    # 例如已确认 6、pending=7：只允许第 7 条涉及的 key 有两种结果。
    pending = acknowledged + 1
    # 只认可换行结束的完整消息；末尾被 kill 截断的半条 ACK 不算确认。
    complete_lines = bytes(buffer).split(b'\n')[:-1]
    if complete_lines:
        if complete_lines != [('ACK ' + str(pending)).encode('ascii')]:
            raise RuntimeError('unexpected protocol after kill')
        # 找到完整 ACK，最后一条也必须恢复；pending=0 表示不再有未知操作。
        acknowledged = pending
        pending = 0
    return acknowledged, pending, delay


def verify(args, path, acknowledged, pending, log):
    """Resolve the recovered prefix, then require it to survive reopening."""
    # 将确认进度和唯一的未知操作交给 worker，全量校验每个 key。
    # worker 用操作编号推导预期版本，并返回实际恢复到的进度 RECOVERED_PREFIX。
    output = run(args, 'verify', path, acknowledged, log, pending)
    prefixes = [line.split()[1] for line in output.splitlines()
                if line.startswith('RECOVERED_PREFIX ')]
    if len(prefixes) != 1:
        raise RuntimeError('missing recovered state')
    recovered = int(prefixes[0])
    if recovered not in (acknowledged, pending):
        raise RuntimeError('invalid recovered prefix')
    # 第一次若确认恢复到 7，第二次就必须仍是 7，不能再退回 6。
    # 此处不传 pending，要求再次打开后状态完全一致。
    run(args, 'verify', path, recovered, log)
    return recovered


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', required=True)
    parser.add_argument('--seed', type=int, default=20261021)
    parser.add_argument('--num-keys', type=int, default=100000)
    parser.add_argument('--key-size', type=int, default=16)
    parser.add_argument('--key-mode', choices=['random', 'sequential'], default='random')
    parser.add_argument('--value-size', type=int, default=128)
    parser.add_argument('--mem-limit-bytes', type=int, default=256 << 20)
    parser.add_argument('--prepare-rounds', type=int, default=1)
    parser.add_argument('--trials', type=int, default=5)
    parser.add_argument('--timeout', type=float, default=3600)
    parser.add_argument('--data-dir', default=None)
    parser.add_argument('--rss-limit-bytes', type=int)
    parser.add_argument('--sample-interval-ms', type=float, default=2)
    parser.add_argument('--monitor-report')
    args = parser.parse_args(argv)
    rss_limit = args.rss_limit_bytes or (args.mem_limit_bytes * 11 // 10 + (8 << 20))
    if rss_limit <= 0 or args.sample_interval_ms <= 0:
        parser.error('invalid RSS monitor settings')
    if min(args.num_keys, args.prepare_rounds, args.trials, args.timeout) <= 0:
        parser.error('counts and timeout must be positive')
    args.rss_limit_bytes = rss_limit
    base = Path(tempfile.mkdtemp(prefix='taycan-crash-', dir=args.data_dir))
    if args.monitor_report is None:
        args.monitor_report = str(base / 'processes.jsonl')
    success = False
    rng = random.Random(args.seed)

    try:
        for trial in range(args.trials):
            # 每轮使用新数据库；同一轮的 prepare/serve/两次 verify 共用该目录。
            path = base / ('db-' + str(trial))
            initial = args.num_keys * args.prepare_rounds
            with (base / ('trial-{}.log'.format(trial))).open('w') as log:
                prepare(args, path, initial, log)
                worker = serve(args, path, initial, log, rng)
                try:
                    acknowledged, pending, delay = kill(worker, trial, rng)
                finally:
                    close_worker(worker)
                recovered = verify(args, path, acknowledged, pending, log)
                print('PASS trial={} seed={} confirmed={} pending={} recovered={} kill_delay={:.6f}'
                      .format(trial, args.seed, acknowledged, pending, recovered, delay), flush=True)
            shutil.rmtree(str(path))
        success = True
    finally:
        if success:
            print("Completed crash logs: " + str(base), flush=True)
        else:
            print('Preserved failure logs/database: ' + str(base), flush=True)


if __name__ == '__main__':
    main()
