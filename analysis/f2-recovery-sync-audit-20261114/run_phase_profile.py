"""Matched fsync audit: real identical kernel fsync, optional native RocksDB group limit."""
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,os,re,shutil,subprocess,sys,threading,time
ROOT=Path(__file__).resolve().parents[2];BASE=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'taycan_participant_sdk/tests'));sys.path.insert(0,str(ROOT/'taycan_participant_sdk/benchmarks'))
from process_runner import ProcessRunner
from native_platform import read_macos_rss
OUT=BASE/'phase-profile';assert not OUT.exists();OUT.mkdir()
for line in subprocess.check_output(['ps','-Ao','pid=,command='],text=True).splitlines():
 if ' -lc ' in line:continue
 if any('/'+n+' ' in line for n in ('compaction_test','crash_worker','recovery_test','concurrent_test','taycan_kv_bench')):raise SystemExit('Other worker alive: '+line)
binaries={'f2':ROOT/'build/f2-phase-profile-20261114/taycan_kv_bench','rocksdb':ROOT/'build/rocks-recovery-sync-audit-20261114/taycan_kv_bench'}
plan=[('f2',1,False,True)]
report=dict(status='RUNNING',scope='Native macOS ARM64; per-call durable fsync; closed-loop threads; not physical power cut or formal score',parameters=dict(num_keys=1000000,key_size=32,value_size=256,mem_limit_bytes=4<<30,seed=20261021),binary_sha256={k:hashlib.sha256(v.read_bytes()).hexdigest() for k,v in binaries.items()},driver_sha256=hashlib.sha256((ROOT/'taycan_participant_sdk/benchmarks/kv_bench.cc').read_bytes()).hexdigest(),runs=[])
def save():(OUT/'results.json').write_text(json.dumps(report,indent=2)+'\n')
save();awake=subprocess.Popen(['/usr/bin/caffeinate','-i','-w',str(os.getpid())])
try:
 for engine,threads,control,interposed in plan:
  cell=f'{engine}-t{threads}-'+('group1' if control else 'default')+('-audit' if interposed else '-plain');folder=OUT/cell;folder.mkdir();db=folder/'db';db.mkdir()
  env={k:v for k,v in os.environ.items() if not k.startswith(('F2_','TAYCAN_','SYNC_AUDIT_','DYLD_INSERT_'))}
  if control:env['TAYCAN_ROCKSDB_MAX_GROUP_BYTES']='1'
  if interposed:env['DYLD_INSERT_LIBRARIES']=str(BASE/'sync_audit.dylib')
  command=[str(binaries[engine]),'--data-dir='+str(db),'--workload=load','--threads='+str(threads),'--duration-seconds=10','--warmup-seconds=2','--sample-every=100']+['--'+k.replace('_','-')+'='+str(v) for k,v in report['parameters'].items()]
  row=dict(cell=cell,engine=engine,threads=threads,group_limited=control,syscall_audit=interposed,status='RUNNING',command=command);report['runs'].append(row);save();stop=threading.Event();start=time.monotonic();print('START',cell,flush=True)
  try:
   with (folder/'output.log').open('w') as log:
    with ProcessRunner(command,log,(4<<30)*11//10,240,.01,folder/'processes.jsonl',rss_reader=read_macos_rss,rss_source='macOS libproc',env=env) as worker:
     def guard():
      while not stop.wait(.5):
       total=0
       for parent,_,names in os.walk(db):
        for name in names:
         try:total+=os.stat(os.path.join(parent,name)).st_size
         except FileNotFoundError:pass
       if total>=2<<30:worker._fail('local_database_cap_2GiB');return
       if shutil.disk_usage(BASE).free<20<<30:worker._fail('local_free_space_20GiB');return
     g=threading.Thread(target=guard,daemon=True);g.start()
     try:row['process']=worker.finish()
     finally:stop.set();g.join()
   lines=(folder/'output.log').read_text().splitlines();results=[]
   for line in lines:
    if line.startswith('{'):
     obj=json.loads(line)
     if obj.get('type')=='result':results.append(obj)
    if line.startswith('SYNC_AUDIT '):row['syscalls']={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)}
    if line.startswith('F2_DATA_PREFIX '):row['prefix']={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)}
   assert len(results)==1 and results[0]['status']=='PASS' and results[0]['puts']==1000000
   row['measurement']=results[0]
   if interposed:
    counts=row['syscalls'];n=counts['hot_calls'] if engine=='f2' else counts['wal_calls'];assert n>0
    row['operations_per_data_sync']=1000000/n;row['data_syncs_per_second']=n/results[0]['measured_seconds']
   row['status']='PASS';row['files']=[dict(path=str(p.relative_to(db)),bytes=p.stat().st_size) for p in db.rglob('*') if p.is_file()];shutil.rmtree(db)
  except Exception as e:row['status']='FAIL';row['reason']=repr(e)
  finally:row['wall_seconds']=time.monotonic()-start;save();print(row['status'],cell,row.get('measurement',{}).get('ops_per_second'),row.get('reason',''),flush=True)
  if row['status']!='PASS':break
 report['status']='PASS' if len(report['runs'])==len(plan) and all(c['status']=='PASS' for c in report['runs']) else 'FAIL';save()
finally:
 if awake.poll() is None:awake.terminate();awake.wait()
