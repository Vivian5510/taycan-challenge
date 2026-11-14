from pathlib import Path
import os,subprocess,time
ROOT=Path(__file__).resolve().parents[2];BASE=Path(__file__).resolve().parent
# Start remaining public scenarios only after the fixed performance matrix exits.
# Uses each original Key count; never reduce it to work around the disk guard.
while True:
    listing=subprocess.check_output(['ps','-Ao','pid=,command='],text=True)
    if not any((' analysis/f2-enabled-verdict-20261114/run_comparison.py' in line and ' -lc ' not in line) or ('/taycan_kv_bench ' in line and '/build/' in line and ' -lc ' not in line) for line in listing.splitlines()):break
    time.sleep(1)
env=dict(os.environ,F2_VERDICT_TESTS='sequential,overwrite,delete,mixed,random,demand_load,memory_limit,checkpoint',F2_VERDICT_RUN='public-remaining',F2_VERDICT_TIMEOUT='600')
with (BASE/'remaining-runner.log').open('w') as out:
 code=subprocess.call(['python3',str(BASE/'run_correctness.py')],cwd=ROOT,env=env,stdout=out,stderr=subprocess.STDOUT)
raise SystemExit(code)
