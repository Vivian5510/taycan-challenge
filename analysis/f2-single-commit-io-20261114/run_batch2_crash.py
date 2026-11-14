from pathlib import Path
import sys
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'taycan_participant_sdk/tests'))
sys.path.insert(0, str(root / 'taycan_participant_sdk/benchmarks'))
import crash_test
from process_runner import ProcessRunner
from native_platform import read_macos_rss
def monitored(*args, **kwargs):
    kwargs.update(rss_reader=read_macos_rss, rss_source='macOS libproc')
    return ProcessRunner(*args, **kwargs)
crash_test.ProcessRunner = monitored
args = ['--worker', str(root / 'build/f2-io-aio-batch2-20261114/crash_worker'),
        '--num-keys', '100000', '--key-size', '16', '--value-size', '128',
        '--mem-limit-bytes', str(4 << 30), '--trials', '5', '--timeout', '600',
        '--rss-limit-bytes', str((4 << 30) * 11 // 10),
        '--data-dir', str(root / 'analysis/f2-single-commit-io-20261114/batch2-crash')]
raise SystemExit(crash_test.main(args))
