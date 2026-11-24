"""Monitor regression checks, independent of the contestant implementation."""
import json
import errno
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

from process_runner import ProcessRunner, read_rss


@unittest.skipUnless(Path('/proc/self/status').exists(), 'Linux /proc required')
class MonitorTests(unittest.TestCase):
    def run_worker(self, source, limit=256 << 20, timeout=5, inject=False):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory)/'process.jsonl'
            with open(str(Path(directory)/'output'), 'w') as log:
                with ProcessRunner([sys.executable, '-c', source], log, limit,
                                   timeout, 0.005, report) as runner:
                    if inject:
                        time.sleep(0.1)
                        runner.inject_kill()
                    return runner.finish(expected_kill=inject)

    def test_success(self):
        result = self.run_worker('import time; time.sleep(.1)')
        self.assertEqual(result['status'], 'PASS')
        self.assertGreater(result['samples'], 0)

    def test_memory_limit(self):
        with self.assertRaisesRegex(RuntimeError, 'memory_limit'):
            self.run_worker('import time; x=bytearray(96*1024*1024); time.sleep(3)',
                            limit=64 << 20)

    def test_timeout(self):
        with self.assertRaisesRegex(RuntimeError, 'timeout'):
            self.run_worker('import time; time.sleep(3)', timeout=.1)

    def test_unexpected_exit(self):
        with self.assertRaisesRegex(RuntimeError, 'unexpected_exit'):
            self.run_worker('import time; time.sleep(.1); raise SystemExit(7)')

    def test_injected_kill(self):
        result = self.run_worker('import time; time.sleep(3)', inject=True)
        self.assertEqual(result['status'], 'EXPECTED_KILL')

    def test_read_error(self):
        with mock.patch('process_runner.read_rss', side_effect=PermissionError('denied')):
            with self.assertRaisesRegex(RuntimeError, 'monitor_error'):
                self.run_worker('import time; time.sleep(3)')

    def test_reaped_process_has_no_rss(self):
        for code in (errno.ENOENT, errno.ESRCH):
            with mock.patch('builtins.open', side_effect=OSError(code, 'process disappeared')):
                self.assertIsNone(read_rss(12345))

    def test_missing_proc_sample_after_valid_samples_is_not_failure(self):
        calls = [0]
        def sample(_):
            calls[0] += 1
            return 4096 if calls[0] == 1 else None
        with mock.patch('process_runner.read_rss', side_effect=sample):
            result = self.run_worker('import time; time.sleep(.1)')
        self.assertEqual(result['status'], 'PASS')
        self.assertEqual(result['rss_peak_bytes'], 4096)

    def test_permission_error_still_fails(self):
        with mock.patch('builtins.open', side_effect=PermissionError(errno.EACCES, 'denied')):
            with self.assertRaises(PermissionError):
                read_rss(12345)

    def test_no_sample(self):
        with mock.patch('process_runner.read_rss', return_value=None):
            with self.assertRaisesRegex(RuntimeError, 'no valid RSS sample'):
                self.run_worker('import time; time.sleep(.1)')

    def test_memory_failure_not_accepted_as_injection(self):
        # Exercise the race protocol deterministically. Sleeping 100 ms before
        # SIGKILL could kill a slow/emulated child before its allocation, testing
        # an expected injection rather than an already-observed memory failure.
        # test_memory_limit above separately checks real process allocations.
        with tempfile.TemporaryDirectory() as directory:
            with open(str(Path(directory)/'output'), 'w') as log:
                with mock.patch('process_runner.read_rss', return_value=96 << 20):
                    with ProcessRunner([sys.executable, '-c', 'import time; time.sleep(3)'],
                                       log, 64 << 20, 5, .005) as runner:
                        runner.thread.join(timeout=5)
                        self.assertFalse(runner.thread.is_alive())
                        self.assertEqual(runner.reason, 'memory_limit')
                        with self.assertRaisesRegex(RuntimeError, 'memory_limit'):
                            runner.inject_kill()
                        with self.assertRaisesRegex(RuntimeError, 'memory_limit'):
                            runner.finish(expected_kill=True)


if __name__ == '__main__':
    unittest.main()
