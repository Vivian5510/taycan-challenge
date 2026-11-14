"""Frozen main, all necessary GC/index migration options enabled. No source fixes."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib, json, os, shutil, subprocess, sys, threading, time
ROOT=Path(__file__).resolve().parents[2]
BASE=Path(__file__).resolve().parent
BUILD=ROOT/'build/f2-enabled-verdict-20261114'
sys.path.insert(0,str(ROOT/'taycan_participant_sdk/tests'))
import run_tests
from process_runner import ProcessRunner
NAMES=os.environ.get('F2_VERDICT_TESTS','basic,recovery,concurrent,compaction,crash_recovery,compaction_crash').split(',')
RUN_NAME=os.environ.get('F2_VERDICT_RUN','public-enabled')
OUT=BASE/RUN_NAME
assert not OUT.exists()
OUT.mkdir()
# Do not start a workload whose final live raw KV bytes alone exceed the
# repository's 2GiB-per-database diagnostic cap. Keep original Key counts.
if NAMES == ['memory_limit']:
    count=20000000
    live=count-count//3-1
    minimum=live*(64+128)
    assert minimum>2<<30
    (OUT/'results.json').write_text(json.dumps([dict(test='memory_limit',status='NOT_RUN',parameters=dict(num_keys=count,key_size=64,value_size=128,rounds=10,mem_limit_bytes=4<<30),reason='Preflight: final live uncompressed KV bytes alone exceed local 2GiB database cap; no smaller dataset substituted',minimum_live_logical_bytes=minimum)],indent=2)+'\n')
    print('NOT_RUN memory_limit: original live dataset exceeds local database cap',flush=True)
    raise SystemExit(1)
class GuardedRunner(ProcessRunner):
    def __init__(self,*a,**kw):
        report=kw.get('report',a[5] if len(a)>5 else None)
        self.folder=Path(report).parent
        env={k:v for k,v in os.environ.items() if not k.startswith(('F2_','TAYCAN_'))}
        env['F2_GC_TRACE']=str(self.folder/'gc-trace.jsonl')
        kw['env']=env
        super().__init__(*a,**kw)
        self.guard_stop=threading.Event();self.guard=threading.Thread(target=self.check,daemon=True);self.guard.start()
    def check(self):
        last=0
        while not self.guard_stop.wait(.5):
            if self.proc.poll() is not None:return
            total=0;largest=0;db_sizes=[]
            for lock in self.folder.rglob('LOCK'):
                db=lock.parent
                size=0
                for directory,_,names in os.walk(db):
                    for name in names:
                        try:size+=os.stat(os.path.join(directory,name),follow_symlinks=False).st_size
                        except FileNotFoundError:pass
                db_sizes.append(dict(path=str(db),bytes=size));total+=size;largest=max(largest,size)
            free=shutil.disk_usage(BASE).free
            if time.monotonic()-last>5:
                with (self.folder/'resources.jsonl').open('a') as f:f.write(json.dumps(dict(pid=self.proc.pid,elapsed=time.monotonic()-self.started,directory_bytes=total,largest_database_bytes=largest,databases=db_sizes,rss_peak_bytes=self.peak,free_bytes=free))+'\n')
                last=time.monotonic()
            if largest>=2<<30:self._fail('local_disk_guard_2GiB_not_an_engine_assertion');return
            if free<20<<30:self._fail('local_free_space_guard_20GiB');return
    def finish(self,*a,**kw):
        try:return super().finish(*a,**kw)
        finally:self.guard_stop.set();self.guard.join()
args=['--native-development','--strict-rss-limit','--build-dir',str(BUILD),'--results-dir',str(OUT),'--tests',','.join(NAMES),'--timeout',os.environ.get('F2_VERDICT_TIMEOUT','600'),'--mem-limit-bytes',str(4<<30)]
manifest=dict(started_at=datetime.now(timezone.utc).isoformat(),revision=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),argv=args,scope='Original public assertions and scenario key counts, user-chosen 4GiB, macOS libproc; explicit local deadline and 2GiB per-database guard, not official timeout',config=dict(durable_compaction=True,cold_migration=True,data_segment_bits=23,sync_strategy='data-log',io='posix-aio',sync='fsync',prefix_block_bytes=512,group_commit=False),binary_sha256={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [BUILD/(n+'_test') for n in NAMES if n not in ('crash_recovery','compaction_crash')]+[BUILD/'crash_worker']})
(BASE/(RUN_NAME+'-manifest.json')).write_text(json.dumps(manifest,indent=2)+'\n')
(BASE/(RUN_NAME+'-resolved-plan.json')).write_text(subprocess.check_output([sys.executable,str(ROOT/'taycan_participant_sdk/tests/run_tests.py')]+args+['--dry-run'],text=True))
awake=subprocess.Popen(['/usr/bin/caffeinate','-i','-w',str(os.getpid())])
run_tests.ProcessRunner=GuardedRunner
try:code=run_tests.main(args)
finally:
    manifest['finished_at']=datetime.now(timezone.utc).isoformat()
    (BASE/(RUN_NAME+'-manifest.json')).write_text(json.dumps(manifest,indent=2)+'\n')
    if awake.poll() is None:awake.terminate();awake.wait()
raise SystemExit(code)
