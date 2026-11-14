"""Same real fsync boundary checks for F2 and RocksDB, no fsync bypass."""
from pathlib import Path
import json,os,select,signal,subprocess,time,shutil
ROOT=Path(__file__).resolve().parents[2];BASE=Path(__file__).resolve().parent
OUT=BASE/'syscall-boundaries-hot-inline';assert not OUT.exists();OUT.mkdir()
selected=json.loads((BASE/'selected.json').read_text())['variant']
workers={'f2':ROOT/('build/f2-io-'+selected+'-20261114')/'crash_worker'}
report=dict(scope='Actual macOS fsync interposition, process crash; not physical power cut',rows=[])
def read_line(fd,buffer,seconds=30):
 deadline=time.monotonic()+seconds
 while b'\n' not in buffer:
  if not select.select([fd],[],[],max(0,deadline-time.monotonic()))[0]:raise TimeoutError('protocol')
  piece=os.read(fd,4096)
  if not piece:return None,buffer
  buffer+=piece
 line,_,rest=buffer.partition(b'\n');return line.decode(),rest
for name,worker in workers.items():
 for mode in ['hold','fail','kill-before-sync']:
  folder=OUT/(name+'-'+mode);folder.mkdir();db=folder/'db'
  common=[str(worker),'--num_keys=64','--key_size=32','--value_size=256','--mem_limit_bytes=4294967296','--seed=20261021','--db_path='+str(db)]
  init=subprocess.run(common+['--mode=prepare','--operations=64'],capture_output=True,text=True,timeout=60)
  (folder/'prepare.stdout').write_text(init.stdout);(folder/'prepare.stderr').write_text(init.stderr);assert init.returncode==0
  ack_r,ack_w=os.pipe();notice_r,notice_w=os.pipe();release_r,release_w=os.pipe();arm=folder/'armed'
  env=dict(os.environ,DYLD_INSERT_LIBRARIES=str(ROOT/'analysis/f2-recovery-sync-audit-20261114/sync_audit.dylib'),SYNC_AUDIT_KIND=name,SYNC_AUDIT_MODE='fail' if mode=='fail' else 'hold',SYNC_AUDIT_NOTICE_FD=str(notice_w),SYNC_AUDIT_RELEASE_FD=str(release_r),SYNC_AUDIT_ARM_PATH=str(arm))
  row=dict(engine=name,mode=mode,status='FAIL');report['rows'].append(row)
  with (folder/'serve.log').open('w') as log:
   p=subprocess.Popen(common+['--mode=serve','--operations=64','--progress_fd='+str(ack_w)],stdin=subprocess.PIPE,stdout=log,stderr=log,env=env,pass_fds=(ack_w,notice_w,release_r),start_new_session=True)
   os.close(ack_w);os.close(notice_w);os.close(release_r)
   try:
    message,buffer=read_line(ack_r,b'');assert message=='READY'
    arm.touch();p.stdin.write(b'G');p.stdin.flush()
    assert select.select([notice_r],[],[],30)[0] and os.read(notice_r,1)==b'S'
    assert not select.select([ack_r],[],[],.05)[0], 'ACK before actual fsync completed'
    row['no_ack_while_fsync_held']=True
    if mode=='kill-before-sync':
     os.killpg(p.pid,signal.SIGKILL);p.wait(timeout=10)
    else:
     os.write(release_w,b'R')
     message,buffer=read_line(ack_r,buffer)
     if mode=='fail':
      assert message is None and p.wait(timeout=10)!=0
      row['sync_error_not_acknowledged']=True
     else:
      assert message=='ACK 65';row['ack_after_sync']=True
      os.killpg(p.pid,signal.SIGKILL);p.wait(timeout=10)
    if mode!='fail':
     confirmed=65 if mode=='hold' else 64
     for attempt in range(2):
      args=common+['--mode=verify','--operations='+str(confirmed)]
      if mode=='kill-before-sync' and attempt==0:args+=['--possible_operation=65']
      q=subprocess.run(args,capture_output=True,text=True,timeout=60)
      (folder/f'verify{attempt}.stdout').write_text(q.stdout);(folder/f'verify{attempt}.stderr').write_text(q.stderr);assert q.returncode==0
      confirmed=int(next(l.split()[1] for l in q.stdout.splitlines() if l.startswith('RECOVERED_PREFIX ')))
     row['recovered_prefix']=confirmed
    row['status']='PASS'
   finally:
    if p.poll() is None:os.killpg(p.pid,signal.SIGKILL);p.wait()
    p.stdin.close()
    for fd in (ack_r,notice_r,release_w):os.close(fd)
  if row['status']=='PASS' and mode!='fail':shutil.rmtree(db)
  (OUT/'results.json').write_text(json.dumps(report,indent=2)+'\n')
  print(name,mode,row['status'],flush=True)
