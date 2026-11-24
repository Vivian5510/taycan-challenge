"""Exercise measurement failure modes with an intentionally faulty engine fixture."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_performance


@unittest.skipUnless(os.environ.get('TAYCAN_BENCH_TEST_BINARY'), 'set TAYCAN_BENCH_TEST_BINARY to the test fixture')
class MeasurementTests(unittest.TestCase):
    def run_case(self, fault='', workload='read', extra=()):
        with tempfile.TemporaryDirectory() as directory:
            command = [os.environ['TAYCAN_BENCH_TEST_BINARY'], '--data-dir='+directory,
                       '--num-keys=64', '--threads=1', '--duration-seconds=1', '--warmup-seconds=0',
                       '--sample-every=1', '--workload='+workload] + list(extra)
            result = subprocess.run(command, capture_output=True, text=True,
                                    env=dict(os.environ, TAYCAN_BENCH_FAULT=fault), timeout=30)
        rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        return result, rows

    def test_distinct_load_count_and_counters(self):
        result, rows = self.run_case(workload='load', extra=['--threads=4'])
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        run_performance.validate(rows[-1], 'load', 64)
        self.assertEqual(rows[-1]['puts'], 64)
        self.assertEqual(rows[-1]['gets'], 0)

    def test_missing_reads_cannot_inflate_ycsb_throughput(self):
        result, rows = self.run_case('missing', 'ycsb_a')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(rows[-1]['status'], 'FAIL')

    def test_full_value_corruption_is_a_failure(self):
        result, rows = self.run_case('corruption')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('corrupt', rows[-1]['error'])

    def test_worker_exception_becomes_failure_report(self):
        result, rows = self.run_case('throw')
        self.assertEqual(result.returncode, 1)
        self.assertIn('injected worker exception', rows[-1]['error'])

    def test_prepare_failure_never_reports_performance_success(self):
        result, rows = self.run_case('put_error')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(rows[-1]['type'], 'error')
        self.assertIn('prepare failed', rows[-1]['error'])

    def test_latency_units_and_per_operation_histograms(self):
        result, rows = self.run_case('slow_get')
        self.assertEqual(result.returncode, 0, result.stdout)
        m = rows[-1]
        run_performance.validate(m, 'read', 64)
        self.assertGreaterEqual(m['get_latency']['p50_us'], 10000)
        self.assertEqual(m['get_latency']['samples'], m['gets'])
        self.assertEqual(m['put_latency']['samples'], 0)
        self.assertIsNone(m['put_latency']['p99_us'])

    def test_prepare_excluded_from_measured_time(self):
        result, rows = self.run_case('slow_prepare')
        self.assertEqual(result.returncode, 0, result.stdout)
        m = rows[-1]
        self.assertGreaterEqual(m['setup_seconds'], .32)
        self.assertGreaterEqual(m['measured_seconds'], 1)
        self.assertAlmostEqual(m['ops_per_second'], m['operations']/m['measured_seconds'], delta=.01)

    def test_throughput_mode_does_not_invent_latency(self):
        result, rows = self.run_case(workload='load', extra=['--sample-every=0'])
        self.assertEqual(result.returncode, 0)
        self.assertEqual(rows[-1]['latency']['samples'], 0)
        self.assertIsNone(rows[-1]['latency']['p99_us'])

    def test_numeric_overflow_cannot_wrap_thread_count(self):
        result, rows = self.run_case(extra=['--threads=4294967297'])
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(rows[-1]['type'], 'error')

    def test_live_counters_report_only_completed_operations(self):
        result, rows = self.run_case('slow_prepare', 'load',
                                     ['--threads=2', '--num-keys=800', '--progress-interval-seconds=1'])
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        progress = [r for r in rows if r.get('type') == 'progress']
        self.assertTrue(any(not r['final'] for r in progress), result.stdout)
        self.assertTrue(progress[-1]['final'])
        self.assertEqual(progress[-1]['completed'], 800)
        self.assertEqual(progress[-1]['total'], 800)
        self.assertEqual(progress[-1]['completed'], rows[-1]['operations'])
        self.assertEqual([r['completed'] for r in progress], sorted(r['completed'] for r in progress))
        self.assertTrue(all(0 <= r['completed'] <= 800 and r['ops_per_second'] >= 0 for r in progress))
        self.assertEqual(progress[-1]['elapsed_seconds'], rows[-1]['measured_seconds'])

    def test_live_counters_can_be_disabled(self):
        result, rows = self.run_case(workload='load', extra=['--progress-interval-seconds=0'])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertFalse(any(r.get('type') == 'progress' for r in rows))
        run_performance.validate(rows[-1], 'load', 64)


if __name__ == '__main__':
    unittest.main()
