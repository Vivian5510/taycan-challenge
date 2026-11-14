"""Matched native main/F2-GC-on and RocksDB reference, cells sequential."""
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,os,shutil,subprocess,sys,threading,time
ROOT=Path(__file__).resolve().parents[2];BASE=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'taycan_participant_sdk/tests'));sys.path.insert(0,str(ROOT/'taycan_participant_sdk/benchmarks'))
from process_runner import ProcessRunner
from native_platform import read_macos_rss
plan=json.loads((BASE/'comparison-plan.json').read_text())
OUT=BASE/'performance';assert not OUT.exists();OUT.mkdir()
binaries={'f2':ROOT/'build/f2-enabled-verdict-20261114/taycan_kv_bench','rocksdb':ROOT/'build/rocks-enabled-verdict-20261114/taycan_kv_bench'}
listing=subprocess.check_output(['ps','-Ao','pid=,command='],text=True)
for line in listing.splitlines():
 if any('/'+name in line for name in ('compaction_test','recovery_test','concurrent_test','crash_worker','memory_limit_test','sequential_test','random_test')) and ' -lc ' not in line:
  raise SystemExit('Refusing performance measurement while correctness worker exists: '+line)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def now():return datetime.now(timezone.utc).isoformat()
identities={k:json.loads(subprocess.check_output([str(v),'--describe'],text=True)) for k,v in binaries.items()}
report=dict(started_at=now(),status='RUNNING',plan=plan,identities=identities,binary_sha256={k:sha(v) for k,v in binaries.items()},shared_driver_sha256=sha(ROOT/'taycan_participant_sdk/benchmarks/kv_bench.cc'),runs=[])
report['linked_libraries']={k:subprocess.check_output(['otool','-L',str(v)],text=True) for k,v in binaries.items()}
report['rocksdb_static_library_sha256']=sha(ROOT/'build/deps/rocksdb-fsync-8.9.1-20261114/lib/librocksdb.a')
report['rocksdb_sync_build_cache']=[l for l in (ROOT/'build/rocksdb-fsync-8.9.1-20261114/CMakeCache.txt').read_text().splitlines() if 'HAVE_FULLFSYNC' in l]
def save():(OUT/'results.json').write_text(json.dumps(report,indent=2)+'\n')
save();awake=subprocess.Popen(['/usr/bin/caffeinate','-i','-w',str(os.getpid())])
failed=set()
try:
 for repeat,order in enumerate(plan['order'],1):
  for workload in plan['workloads']:
   for name in order:
    cell=f'{name}-{workload}-r{repeat}'
    if name in failed:
     report['runs'].append(dict(cell=cell,engine=name,workload=workload,repeat=repeat,status='NOT_RUN',reason='engine previously failed; no restart/retry to replace failure'));save();continue
    folder=OUT/cell;folder.mkdir();db=folder/'db';db.mkdir()
    command=[str(binaries[name]),'--data-dir='+str(db),'--workload='+workload]
    params={**plan['data'],'duration_seconds':plan['duration_seconds'],'warmup_seconds':plan['warmup_seconds'],'sample_every':100,'progress_interval_seconds':1}
    command+=['--'+k.replace('_','-')+'='+str(v) for k,v in params.items()]
    row=dict(cell=cell,engine=name,workload=workload,repeat=repeat,command=command,status='RUNNING');report['runs'].append(row);save()
    env={k:v for k,v in os.environ.items() if not k.startswith(('F2_','TAYCAN_'))};env['F2_GC_TRACE']=str(folder/'gc-trace.jsonl')
    print('START',cell,flush=True);began=time.monotonic();stop=threading.Event()
    try:
     with (folder/'output.log').open('w') as output:
      with ProcessRunner(command,output,plan['data']['mem_limit_bytes']*11//10,plan['guards']['timeout_seconds'],.01,folder/'processes.jsonl',rss_reader=read_macos_rss,rss_source='macOS libproc native development',env=env) as worker:
       def guard():
        last=0
        while not stop.wait(.5):
         if worker.proc.poll() is not None:return
         total=0
         for directory,_,names in os.walk(db):
          for filename in names:
           try:total+=os.stat(os.path.join(directory,filename),follow_symlinks=False).st_size
           except FileNotFoundError:pass
         if time.monotonic()-last>5:
          with (folder/'resources.jsonl').open('a') as f:f.write(json.dumps(dict(elapsed=time.monotonic()-began,directory_bytes=total,rss_peak_bytes=worker.peak))+'\n')
          last=time.monotonic()
         if total>=plan['guards']['single_database_bytes']:worker._fail('local_2GiB_database_guard');return
         if shutil.disk_usage(BASE).free<plan['guards']['free_bytes']:worker._fail('local_20GiB_free_guard');return
       monitor=threading.Thread(target=guard,daemon=True);monitor.start()
       try:row['process']=worker.finish()
       finally:stop.set();monitor.join()
     records=[]
     for line in (folder/'output.log').read_text().splitlines():
      try:records.append(json.loads(line))
      except ValueError:pass
     results=[r for r in records if r.get('type')=='result'];errors=[r for r in records if r.get('type')=='error']
     if len(results)!=1:raise RuntimeError(str(errors or 'no final measurement'))
     row['measurement']=results[0]
     if results[0]['status']!='PASS':raise RuntimeError(results[0].get('error','measurement failed'))
     if workload=='load' and results[0]['operations']!=plan['data']['num_keys']:raise RuntimeError('load incomplete')
     row['status']='PASS'
     row['database_files']=[dict(path=str(p.relative_to(db)),bytes=p.stat().st_size) for p in db.rglob('*') if p.is_file()]
     shutil.rmtree(db)
    except Exception as e:
     row['reason']=str(e);failed.add(name)
    finally:
     stop.set();row['wall_seconds']=time.monotonic()-began;save()
     print(row['status'],cell,row.get('reason',row.get('measurement',{}).get('ops_per_second')),flush=True)
 report['status']='PASS' if not failed else 'FAIL';report['finished_at']=now();save()
finally:
 if awake.poll() is None:awake.terminate();awake.wait()
