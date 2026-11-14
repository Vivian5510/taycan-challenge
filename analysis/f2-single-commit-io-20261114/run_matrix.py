"""Sequential fixed-condition IO candidates; no compiling or tests during measurement."""
from pathlib import Path
from datetime import datetime,timezone
import argparse,hashlib,json,os,shutil,subprocess,sys,threading,time
ROOT=Path(__file__).resolve().parents[2];BASE=Path(__file__).resolve().parent
sys.path[:0]=[str(ROOT/'taycan_participant_sdk/tests'),str(ROOT/'taycan_participant_sdk/benchmarks')]
from process_runner import ProcessRunner
from native_platform import read_macos_rss
parser=argparse.ArgumentParser();parser.add_argument('--stage',choices=['preflight','screen','confirm'],required=True);args=parser.parse_args()
plan=json.loads((BASE/'plan.json').read_text());variants={r['name']:r for r in plan['variants']}
def build(name):return ROOT/('build/f2-io-'+name+'-20261114')
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
OUT=BASE/args.stage;assert not OUT.exists();OUT.mkdir()
listing=subprocess.check_output(['ps','-Ao','pid=,command='],text=True)
for line in listing.splitlines():
 if ' -lc ' in line:continue
 if any('/'+n+' ' in line for n in ['taycan_kv_bench','compaction_test','recovery_test','crash_worker','basic_test','concurrent_test']) or 'cmake --build ' in line or '/clang ' in line or '/clang++ ' in line:
  raise SystemExit('Other workload/build alive: '+line)
report=dict(stage=args.stage,status='RUNNING',started_at=datetime.now(timezone.utc).isoformat(),plan=plan,scope='macOS ARM64 local development; sync-before-success; not official score',source_sha256={str(p.relative_to(ROOT)):digest(p) for p in (ROOT/'experiments/faster-native-adapter').iterdir() if p.is_file() and p.suffix in ['.cc','.h','.inc','.py','.txt']},driver_sha256=digest(ROOT/'taycan_participant_sdk/benchmarks/kv_bench.cc'),binaries={name:dict(sha256=digest(build(name)/'taycan_kv_bench'),identity=json.loads(subprocess.check_output([str(build(name)/'taycan_kv_bench'),'--describe'],text=True))) for name in variants},runs=[])
def save():(OUT/'results.json').write_text(json.dumps(report,indent=2)+'\n')
save();awake=subprocess.Popen(['/usr/bin/caffeinate','-i','-w',str(os.getpid())])
if args.stage=='preflight':cells=[(name,test,1) for name in variants for test in ['basic','recovery','concurrent']]
elif args.stage=='screen':cells=[(name,'load',1) for name in variants]
else:
 selected=json.loads((BASE/'selected.json').read_text())['variant'];names=['aio-nocache',selected]
 cells=[(n,w,r) for r in (1,2) for w in ['load','update','contest_mix'] for n in (names if r==1 else list(reversed(names)))]
try:
 for name,workload,repeat in cells:
  cell=f'{name}-{workload}-r{repeat}';folder=OUT/cell;folder.mkdir();db=folder/'db';db.mkdir()
  if args.stage=='preflight':
   command=[str(build(name)/(workload+'_test')),'--num_keys=1000','--key_size=32','--value_size=256','--mem_limit_bytes=4294967296','--data_dir='+str(db)]
  else:
   command=[str(build(name)/'taycan_kv_bench'),'--data-dir='+str(db),'--workload='+workload,'--num-keys=1000000','--threads=32','--mem-limit-bytes=4294967296','--key-size=32','--value-size=256','--seed=20261021','--duration-seconds=10','--warmup-seconds=2','--sample-every=100','--progress-interval-seconds=1']
  env={k:v for k,v in os.environ.items() if not k.startswith(('F2_','TAYCAN_','SYNC_AUDIT_','DYLD_INSERT_'))}
  env['F2_GC_TRACE']=str(folder/'gc.jsonl')
  row=dict(cell=cell,variant=name,workload=workload,repeat=repeat,status='RUNNING',command=command);report['runs'].append(row);save();print('START',cell,flush=True);start=time.monotonic();stop=threading.Event()
  try:
   with (folder/'output.log').open('w') as log:
    with ProcessRunner(command,log,(4<<30)*11//10,240,.01,folder/'processes.jsonl',rss_reader=read_macos_rss,rss_source='macOS libproc',env=env) as worker:
     def guard():
      last=0
      while not stop.wait(.5):
       size=0
       for parent,_,files in os.walk(db):
        for filename in files:
         try:size+=os.stat(os.path.join(parent,filename)).st_size
         except FileNotFoundError:pass
       free=shutil.disk_usage(BASE).free
       if time.monotonic()-last>=5:
        with (folder/'resources.jsonl').open('a') as f:f.write(json.dumps(dict(elapsed=time.monotonic()-start,directory_bytes=size,rss_peak_bytes=worker.peak,free_bytes=free))+'\n')
        last=time.monotonic()
       if size>=2<<30:worker._fail('local_database_2GiB_guard');return
       if free<20<<30:worker._fail('local_free_20GiB_guard');return
     g=threading.Thread(target=guard,daemon=True);g.start()
     try:row['process']=worker.finish()
     finally:stop.set();g.join()
   if args.stage!='preflight':
    measurements=[]
    for line in (folder/'output.log').read_text().splitlines():
     if line.startswith('{'):
      o=json.loads(line)
      if o.get('type')=='result':measurements.append(o)
    assert len(measurements)==1 and measurements[0]['status']=='PASS';m=measurements[0]
    if workload=='load':assert m['puts']==1000000
    assert abs(m['ops_per_second']-m['operations']/m['measured_seconds'])<=m['ops_per_second']*1e-8
    row['measurement']=m
   row['status']='PASS';row['database_files']=[dict(path=str(p.relative_to(db)),bytes=p.stat().st_size) for p in db.rglob('*') if p.is_file()];shutil.rmtree(db)
  except Exception as e:row['status']='FAIL';row['reason']=repr(e)
  finally:row['wall_seconds']=time.monotonic()-start;save();print(row['status'],cell,row.get('measurement',{}).get('ops_per_second'),row.get('reason',''),flush=True)
  if row['status']!='PASS':break
 report['status']='PASS' if len(report['runs'])==len(cells) and all(c['status']=='PASS' for c in report['runs']) else 'FAIL';report['finished_at']=datetime.now(timezone.utc).isoformat();save()
finally:
 if awake.poll() is None:awake.terminate();awake.wait()
