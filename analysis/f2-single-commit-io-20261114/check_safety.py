"""Read-only check of the scoped live worker and retained database sizes."""
from pathlib import Path
import json
import os
import shutil
import subprocess

BASE = Path(__file__).resolve().parent
print('free_GiB', round(shutil.disk_usage(BASE).free / (1 << 30), 2))
assert shutil.disk_usage(BASE).free > 20 << 30
listing = subprocess.check_output(['ps', '-Ao', 'pid=,ppid=,etime=,%cpu=,comm='], text=True)
for line in listing.splitlines():
    if '/build/f2-io-' in line or '/usr/bin/caffeinate' in line:
        print(line.strip())
raw = BASE / 'confirm/results.json'
if raw.exists():
    report = json.loads(raw.read_text())
    active = [r for r in report['runs'] if r['status'] == 'RUNNING']
    for row in active:
        log = BASE / 'confirm' / row['cell'] / 'output.log'
        print(row['cell'])
        print('\n'.join(log.read_text().splitlines()[-2:]))
