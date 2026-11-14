from pathlib import Path
import json,hashlib,subprocess
ROOT=Path(__file__).resolve().parents[2];BASE=Path(__file__).resolve().parent
r=json.loads((BASE/'performance/results.json').read_text());excluded=set(json.loads((BASE/'contention.json').read_text())['affected_cells'])
assert r['plan']['data']==dict(num_keys=1000000,key_size=32,value_size=256,mem_limit_bytes=4<<30,threads=32,seed=20261021,distribution='uniform')
for name,path in [('f2',ROOT/'build/f2-enabled-verdict-20261114/taycan_kv_bench'),('rocksdb',ROOT/'build/rocks-enabled-verdict-20261114/taycan_kv_bench')]:
 assert hashlib.sha256(path.read_bytes()).hexdigest()==r['binary_sha256'][name]
assert hashlib.sha256((ROOT/'taycan_participant_sdk/benchmarks/kv_bench.cc').read_bytes()).hexdigest()==r['shared_driver_sha256']
for row in r['runs']:
 if row['status']!='PASS':continue
 m=row['measurement'];assert m['status']=='PASS'
 assert abs(m['ops_per_second']-m['operations']/m['measured_seconds'])<=m['ops_per_second']*1e-8
 assert m['operations']==m['gets']+m['puts']+m['deletes']
 if row['workload']=='load':assert m['puts']==1000000
 assert row['process']['rss_peak_bytes']<=4*(1<<30)*11//10
folder=BASE/'performance'
rows=[]
for row in r['runs']:
 records=[];p=folder/row['cell']/'output.log'
 if p.exists():
  for line in p.read_text().splitlines():
   if line.startswith('F2_DATA_PREFIX ') or line.startswith('F2_COMMIT_COUNTS '):records.append(line)
 rows.append(dict(cell=row['cell'],status=row['status'],included_in_comparison=row['status']=='PASS' and row['cell'] not in excluded,excluded_due_contention=row['cell'] in excluded,counters=records))
(BASE/'performance-verification.json').write_text(json.dumps(dict(binary_hashes_match=True,shared_driver_matches=True,operation_counters_match=True,rss_limit_checked=True,excluded_cells=sorted(excluded),cells=rows),indent=2)+'\n')
print('Verified',len(r['runs']),'cells; exclusions:',excluded)
