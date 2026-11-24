"""Native metric checks: actual resident allocation, timeout and fail-closed behavior."""
import errno
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'tests'))
from native_platform import read_macos_rss
from process_runner import ProcessRunner


@unittest.skipUnless(platform.system() == 'Darwin', 'macOS libproc required')
class NativeMonitorTests(unittest.TestCase):
    def test_real_rss_units_and_process_exit(self):
        child = subprocess.Popen([sys.executable, '-c',
                                  'import sys,time; x=bytearray(32*1024*1024); print("READY",flush=True);time.sleep(5)'],
                                 stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(child.stdout.readline().strip(), 'READY')
            rss = read_macos_rss(child.pid)
            self.assertGreater(rss, 32 << 20)
            self.assertLess(rss, 256 << 20)
        finally:
            child.terminate()
            child.wait()
            child.stdout.close()
        self.assertIsNone(read_macos_rss(child.pid))

    def run_child(self, source, limit=256 << 20, timeout=3, reader=read_macos_rss):
        with tempfile.TemporaryDirectory() as directory:
            with (Path(directory)/'output.log').open('w') as log:
                with ProcessRunner([sys.executable, '-c', source], log, limit, timeout, .01,
                                   rss_reader=reader, rss_source='native regression fixture') as runner:
                    return runner.finish()

    def test_actual_memory_limit(self):
        with self.assertRaisesRegex(RuntimeError, 'memory_limit'):
            self.run_child('import time; x=bytearray(96*1024*1024);time.sleep(3)', limit=64 << 20)

    def test_timeout(self):
        with self.assertRaisesRegex(RuntimeError, 'timeout'):
            self.run_child('import time;time.sleep(3)', timeout=.1)

    def test_no_valid_sample_is_failure(self):
        with self.assertRaisesRegex(RuntimeError, 'no valid RSS sample'):
            self.run_child('import time;time.sleep(.1)', reader=lambda _: None)

    def test_permission_error_is_failure(self):
        with self.assertRaisesRegex(RuntimeError, 'monitor_error'):
            self.run_child('import time;time.sleep(3)', reader=mock.Mock(side_effect=PermissionError(errno.EPERM, 'denied')))

    def test_default_monitor_still_requires_linux(self):
        with self.assertRaisesRegex(RuntimeError, 'Linux /proc'):
            self.run_child('pass', reader=None)


if __name__ == '__main__':
    unittest.main()
