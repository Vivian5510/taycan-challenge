"""Invoke the existing exact-phase GC oracle on the frozen selected binary."""
from pathlib import Path
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BASE = Path(__file__).resolve().parent
selected = json.loads((BASE / 'selected.json').read_text())['variant']
assert selected == 'aio-batch2'
listing = subprocess.check_output(['ps', '-Ao', 'pid=,command='], text=True)
for line in listing.splitlines():
    if ' -lc ' in line:
        continue
    if '/taycan_kv_bench ' in line and '/build/' in line:
        raise SystemExit('Performance worker is still active: ' + line)
command = [sys.executable,
           str(ROOT / 'experiments/faster-native-adapter/validate_compaction_crash.py'),
           '--build-dir', str(ROOT / ('build/f2-io-' + selected + '-20261114')),
           '--results-dir', str(BASE / 'gc-crash-batch2')]
with (BASE / 'gc-crash-batch2-runner.log').open('w') as log:
    result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
raise SystemExit(result.returncode)
