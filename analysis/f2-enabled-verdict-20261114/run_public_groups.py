"""Run remaining independent correctness groups concurrently, after performance.
Correctness time/RSS not used as a performance comparison.
"""
from pathlib import Path
import concurrent.futures,json,os,subprocess
ROOT=Path(__file__).resolve().parents[2];BASE=Path(__file__).resolve().parent
listing=subprocess.check_output(['ps','-Ao','pid=,command='],text=True)
for line in listing.splitlines():
    if ' -lc ' in line:continue
    if ' analysis/f2-enabled-verdict-20261114/run_comparison.py' in line or ('/taycan_kv_bench ' in line and '/build/' in line):raise SystemExit('Performance worker still active: '+line)
groups={'public-small':'overwrite,delete,mixed,checkpoint','public-random':'random','public-demand':'demand_load','public-memory':'memory_limit'}
rows=[]
def run(name,tests):
 env=dict(os.environ,F2_VERDICT_TESTS=tests,F2_VERDICT_RUN=name,F2_VERDICT_TIMEOUT='600')
 with (BASE/(name+'-runner.log')).open('w') as log:
  p=subprocess.run(['python3',str(BASE/'run_correctness.py')],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
 return dict(group=name,tests=tests,returncode=p.returncode)
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
 futures=[pool.submit(run,name,tests) for name,tests in groups.items()]
 for f in concurrent.futures.as_completed(futures):
  row=f.result();rows.append(row);print(row,flush=True)
  (BASE/'public-groups.json').write_text(json.dumps(rows,indent=2)+'\n')
