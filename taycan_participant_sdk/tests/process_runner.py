"""Linux process supervision shared by ordinary and crash tests (Python 3.6+)."""
import json
import math
import errno
import os
from pathlib import Path
import signal
import subprocess
import threading
import time


def read_rss(pid):
    try:
        with open('/proc/{}/status'.format(pid)) as stream:
            for line in stream:
                if line.startswith('VmRSS:'):
                    return int(line.split()[1]) * 1024
    except OSError as error:
        # wait() may reap the process concurrently with this sample. A vanished
        # /proc entry is an exit race, not proof of a monitoring failure.
        if error.errno not in (errno.ENOENT, errno.ESRCH):
            raise
        return None
    return None  # A zombie has no address space.


class ProcessRunner:
    def __init__(self, command, log, rss_limit, timeout=3600, interval=0.002,
                 report=None, rss_reader=None, rss_source='Linux /proc VmRSS', **kwargs):
        if rss_reader is None and not Path('/proc/self/status').exists():
            raise RuntimeError('RSS monitoring requires Linux /proc')
        if (rss_limit <= 0 or not math.isfinite(timeout) or timeout <= 0 or
                not math.isfinite(interval) or interval <= 0):
            raise ValueError('monitor limits and interval must be positive')
        self.command = [str(x) for x in command]
        self.limit, self.timeout, self.interval = rss_limit, timeout, interval
        self.report = report
        self.rss_reader, self.rss_source = rss_reader, rss_source
        self.peak = self.samples = 0
        self.max_sample_gap = 0.0
        self.last_sample = None
        self.reason = None
        self.injected = False
        self.started = time.monotonic()
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.closed = False
        self.proc = subprocess.Popen(self.command, stdout=log, stderr=log,
                                     start_new_session=True, **kwargs)
        self.thread = threading.Thread(target=self._monitor, daemon=True)
        self.thread.start()

    def _kill(self):
        try:
            os.killpg(self.proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    def _fail(self, reason):
        with self.lock:
            if self.reason is None:
                self.reason = reason
            self._kill()

    def _monitor(self):
        while not self.stop.is_set():
            try:
                rss = (self.rss_reader or read_rss)(self.proc.pid)
                if rss is not None:
                    now = time.monotonic()
                    if self.last_sample is not None:
                        self.max_sample_gap = max(self.max_sample_gap, now-self.last_sample)
                    self.last_sample = now
                    self.samples += 1
                    self.peak = max(self.peak, rss)
                    if rss > self.limit:
                        self._fail('memory_limit')
                        return
            except (OSError, ValueError) as error:
                if self.proc.poll() is not None:
                    return
                self._fail('monitor_error: ' + str(error))
                return
            if self.proc.poll() is not None:
                return
            if time.monotonic() - self.started >= self.timeout:
                self._fail('timeout')
                return
            self.stop.wait(self.interval)

    def inject_kill(self):
        with self.lock:
            if self.reason:
                raise RuntimeError(self.reason)
            if self.proc.poll() is not None:
                raise RuntimeError('worker exited before fault injection')
            self.injected = True
            self._kill()

    def finish(self, expected_kill=False):
        code = self.proc.wait()
        self.stop.set()
        self.thread.join()
        self.closed = True
        if not self.samples and not self.reason:
            self.reason = 'monitor_error: no valid RSS sample'
        if not self.reason:
            if expected_kill and self.injected and code == -signal.SIGKILL:
                status = 'EXPECTED_KILL'
            elif code == 0 and not expected_kill:
                status = 'PASS'
            else:
                self.reason = 'unexpected_exit: {}'.format(code)
        if self.reason:
            status = 'FAIL'
        result = dict(command=self.command, pid=self.proc.pid, status=status,
                      reason=self.reason, returncode=code, rss_peak_bytes=self.peak,
                      rss_limit_bytes=self.limit, samples=self.samples,
                      rss_source=self.rss_source,
                      sample_interval_seconds=self.interval,
                      max_sample_gap_seconds=self.max_sample_gap,
                      elapsed_seconds=time.monotonic() - self.started)
        if self.report:
            with open(str(self.report), 'a') as stream:
                stream.write(json.dumps(result) + '\n')
        if self.reason:
            raise RuntimeError(self.reason + ' (peak={}, limit={})'.format(self.peak, self.limit))
        return result

    def close(self):
        if not self.closed:
            self._fail('cancelled')
            try:
                self.finish()
            except RuntimeError:
                pass

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()
