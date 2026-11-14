from pathlib import Path
import hashlib
import json
import os
import shutil
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
BASE = Path(__file__).resolve().parent
OUT = BASE / 'batch4'
assert not OUT.exists()
OUT.mkdir()
sys.path[:0] = [str(ROOT / 'taycan_participant_sdk/tests'),
               str(ROOT / 'taycan_participant_sdk/benchmarks')]
from process_runner import ProcessRunner
from native_platform import read_macos_rss

binary = ROOT / 'build/f2-io-aio-batch4-20261114/taycan_kv_bench'
report = dict(status='RUNNING', variant='aio-batch4',
              binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), runs=[])

def save():
    (OUT / 'results.json').write_text(json.dumps(report, indent=2) + '\n')

for workload in ('load', 'update', 'contest_mix'):
    folder = OUT / workload
    folder.mkdir()
    db = folder / 'db'
    db.mkdir()
    command = [str(binary), '--data-dir=' + str(db), '--workload=' + workload,
               '--num-keys=1000000', '--threads=32', '--mem-limit-bytes=4294967296',
               '--key-size=32', '--value-size=256', '--seed=20261021',
               '--duration-seconds=10', '--warmup-seconds=2', '--sample-every=100',
               '--progress-interval-seconds=1']
    row = dict(workload=workload, status='FAIL', command=command)
    report['runs'].append(row)
    save()
    started = time.monotonic()
    try:
        with (folder / 'output.log').open('w') as log:
            with ProcessRunner(command, log, (4 << 30) * 11 // 10, 240, .01,
                               folder / 'processes.jsonl', rss_reader=read_macos_rss,
                               rss_source='macOS libproc', env=dict(os.environ)) as worker:
                row['process'] = worker.finish()
        records = [json.loads(line) for line in (folder / 'output.log').read_text().splitlines()
                   if line.startswith('{')]
        measurement = next(item for item in records if item.get('type') == 'result')
        assert measurement['status'] == 'PASS'
        row['measurement'] = measurement
        row['status'] = 'PASS'
        shutil.rmtree(db)
    except Exception as error:
        row['reason'] = repr(error)
    row['wall_seconds'] = time.monotonic() - started
    save()
    print(workload, row['status'], row.get('measurement', {}).get('ops_per_second'), flush=True)

report['status'] = 'PASS' if all(item['status'] == 'PASS' for item in report['runs']) else 'FAIL'
save()
