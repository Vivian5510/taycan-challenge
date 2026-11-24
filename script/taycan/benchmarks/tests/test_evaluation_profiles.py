"""Regression checks for the published contest workload profiles and gates."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_performance


class EvaluationProfileTests(unittest.TestCase):
    def test_official_load_profile(self):
        profile = run_performance.PROFILES['contest-load']
        self.assertEqual(profile['num_keys'], 200_000_000)
        self.assertEqual(profile['mem_limit_bytes'], 64 << 30)
        self.assertEqual(profile['threads'], [32])
        self.assertEqual(profile['workloads'], ['load'])

    def test_official_mixed_profiles(self):
        for name, budget in (('contest-4g', 4 << 30), ('contest-16g', 16 << 30)):
            with self.subTest(name=name):
                profile = run_performance.PROFILES[name]
                self.assertEqual(profile['num_keys'], 200_000_000)
                self.assertEqual(profile['mem_limit_bytes'], budget)
                self.assertEqual(profile['duration_seconds'], 600)
                self.assertEqual(profile['threads'], [32])
                self.assertEqual(profile['workloads'], ['contest_mix'])

    def test_recovery_profiles_use_full_dataset_and_one_local_probe_thread(self):
        for name, budget in (('contest-recovery-4g', 4 << 30),
                             ('contest-recovery-16g', 16 << 30)):
            with self.subTest(name=name):
                profile = run_performance.PROFILES[name]
                self.assertEqual(profile['num_keys'], 200_000_000)
                self.assertEqual(profile['mem_limit_bytes'], budget)
                self.assertEqual(profile['threads'], [1])
                self.assertEqual(profile['workloads'], ['recovery'])

    def test_gate_uses_average_and_published_minimum(self):
        runs = [
            {'status': 'PASS', 'measurement': {'ops_per_second': 62000.0}},
            {'status': 'PASS', 'measurement': {'ops_per_second': 58000.0}},
        ]
        gate = run_performance.evaluation_gate('contest-load', runs, 2, 'throughput', True)
        self.assertEqual(gate['status'], 'PASS')
        self.assertEqual(gate['required_average_ops_per_second'], 60000)
        self.assertEqual(gate['measured_average_ops_per_second'], 60000.0)

    def test_gate_distinguishes_failure_incomplete_and_latency_runs(self):
        low = [{'status': 'PASS', 'measurement': {'ops_per_second': 49999.0}}]
        self.assertEqual(run_performance.evaluation_gate('contest-4g', low, 1, 'throughput', True)['status'], 'FAIL')
        self.assertEqual(run_performance.evaluation_gate('contest-16g', [], 1, 'throughput', True)['status'],
                         'INCOMPLETE')
        self.assertEqual(run_performance.evaluation_gate('contest-4g', [], 1, 'latency', True)['status'],
                         'NOT_APPLICABLE')
        self.assertEqual(run_performance.evaluation_gate('contest-4g', low, 1, 'throughput', False)['status'],
                         'INCOMPLETE')


if __name__ == '__main__':
    unittest.main()
