"""Verify frozen parameters, successful calls and sealed commits in each cell."""
from pathlib import Path
import hashlib
import json
import math
import re
import statistics

ROOT = Path(__file__).resolve().parents[2]
BASE = Path(__file__).resolve().parent
raw = json.loads((BASE / 'confirm/results.json').read_text())
summary = dict(status=raw['status'], completed=0, cells=[], workloads=[])
for row in raw['runs']:
    check = dict(cell=row['cell'], status=row['status'])
    summary['cells'].append(check)
    if row['status'] != 'PASS':
        continue
    build = ROOT / ('build/f2-io-' + row['variant'] + '-20261114')
    digest = hashlib.sha256((build / 'taycan_kv_bench').read_bytes()).hexdigest()
    assert digest == raw['binaries'][row['variant']]['sha256']
    assert row['process']['returncode'] == 0
    assert row['process']['rss_peak_bytes'] <= (4 << 30) * 11 // 10
    command = row['command']
    for flag in ('--num-keys=1000000', '--threads=32', '--mem-limit-bytes=4294967296',
                 '--key-size=32', '--value-size=256', '--seed=20261021',
                 '--duration-seconds=10', '--warmup-seconds=2'):
        assert flag in command
    result = row['measurement']
    assert result['status'] == 'PASS'
    assert result['operations'] == sum(result[k] for k in ('gets', 'puts', 'deletes'))
    assert math.isclose(result['ops_per_second'], result['operations'] / result['measured_seconds'], rel_tol=1e-8)
    if row['workload'] == 'load':
        assert result['operations'] == 1000000
    text = (BASE / 'confirm' / row['cell'] / 'output.log').read_text()
    counters = re.findall(r'F2_DATA_PREFIX commits=(\d+) data_syncs=(\d+)', text)
    assert len(counters) == 1
    commits, syncs = map(int, counters[0])
    assert syncs >= commits
    if row['workload'] == 'load':
        assert commits == 1000000
    check.update(binary_sha256=digest, commits=commits, data_syncs=syncs,
                 rss_peak_bytes=row['process']['rss_peak_bytes'])
    summary['completed'] += 1

for workload in ('load', 'update', 'contest_mix'):
    pair = dict(workload=workload)
    for name in ('aio-nocache', 'aio-batch2'):
        cells = [r for r in raw['runs'] if r['variant'] == name and
                 r['workload'] == workload and r['status'] == 'PASS']
        values = [c['measurement']['ops_per_second'] for c in cells]
        pair[name] = dict(completed=len(values), observations=values,
                          median=statistics.median(values) if len(values) == 2 else None,
                          rss_peak_bytes=max((c['process']['rss_peak_bytes'] for c in cells), default=0))
    if all(pair[n]['median'] for n in ('aio-nocache', 'aio-batch2')):
        pair['ratio'] = pair['aio-batch2']['median'] / pair['aio-nocache']['median']
    summary['workloads'].append(pair)

assert hashlib.sha256((ROOT / 'taycan_participant_sdk/benchmarks/kv_bench.cc').read_bytes()).hexdigest() == raw['driver_sha256']
if raw['status'] == 'PASS':
    assert summary['completed'] == 12
(BASE / 'confirmation-verification.json').write_text(json.dumps(summary, indent=2) + '\n')
print(raw['status'], summary['completed'], 'verified cells')
