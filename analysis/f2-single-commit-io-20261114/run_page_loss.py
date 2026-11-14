"""Both engines:74 successful operations newer than base, hold actual pending fsync.
Clone only while stopped. Lose bytes strictly after last acknowledged file end.
"""
from pathlib import Path
import json,os,select,signal,subprocess,time,shutil
ROOT=Path(__file__).resolve().parents[2];BASE=Path(__file__).resolve().parent
OUT=BASE/'same-page-loss';assert not OUT.exists();OUT.mkdir();report=dict(scope='Identical pending fsync page-image fault, not physical power cut',engines=[])
for name,probe in [('f2',BASE/'ack_suffix_probe')]:
 folder=OUT/name;folder.mkdir();db=folder/'source';arm=folder/'armed';nr,nw=os.pipe();rr,rw=os.pipe()
 env=dict(os.environ,DYLD_INSERT_LIBRARIES=str(ROOT/'analysis/f2-recovery-sync-audit-20261114/sync_audit.dylib'),SYNC_AUDIT_KIND=name,SYNC_AUDIT_MODE='hold',SYNC_AUDIT_NOTICE_FD=str(nw),SYNC_AUDIT_RELEASE_FD=str(rr),SYNC_AUDIT_ARM_PATH=str(arm))
 with (folder/'stderr.log').open('w') as err:
  p=subprocess.Popen([str(probe),'write',str(db)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,env=env,pass_fds=(nw,rr),start_new_session=True)
  os.close(nw);os.close(rr)
  try:
   assert select.select([p.stdout],[],[],30)[0];assert p.stdout.readline().decode().strip()=='ACK_PREFIX 74'
   paths=list((db/'hot').glob('log.log*')) if name=='f2' else list(db.glob('*.log'))
   before={str(f.relative_to(db)):f.stat().st_size for f in paths}
   before_data={str(f.relative_to(db)):f.read_bytes() for f in paths}
   p.stdin.write(b'G');p.stdin.flush();assert select.select([nr],[],[],30)[0] and os.read(nr,1)==b'S'
   os.killpg(p.pid,signal.SIGSTOP);os.waitpid(p.pid,os.WUNTRACED)
   frozen=folder/'frozen';shutil.copytree(db,frozen)
   os.killpg(p.pid,signal.SIGKILL);p.wait()
  finally:
   if p.poll() is None:os.killpg(p.pid,signal.SIGKILL);p.wait()
   p.stdout.close();p.stdin.close();os.close(nr);os.close(rw)
 changed=[rel for rel,size in before.items() if (frozen/rel).stat().st_size>size]
 assert len(changed)==1,changed
 rel=changed[0];stable=before[rel];data=(frozen/rel).read_bytes();assert data[:stable]==before_data[rel]
 boundary=(stable//4096+1)*4096;assert boundary<len(data)
 cases={'all-lost':data[:stable],'all-survives':data,'earlier-page-lost':data[:stable]+bytes(boundary-stable)+data[boundary:],'tail-page-lost':data[:boundary]+bytes(len(data)-boundary)}
 row=dict(engine=name,last_ack=74,stable_file_end=stable,log=rel,pending_bytes=len(data)-stable,cases=[]);report['engines'].append(row)
 for case,image in cases.items():
  assert image[:stable]==before_data[rel]
  target=folder/case;shutil.copytree(frozen,target);(target/rel).write_bytes(image);checks=[]
  for repeat in range(2):
   q=subprocess.run([str(probe),'verify',str(target)],capture_output=True,text=True,timeout=60)
   (folder/(case+f'-{repeat}.stdout')).write_text(q.stdout);(folder/(case+f'-{repeat}.stderr')).write_text(q.stderr)
   checks.append(dict(returncode=q.returncode,stdout=q.stdout));assert q.returncode==0,q.stderr
  row['cases'].append(dict(case=case,status='PASS',checks=checks));shutil.rmtree(target)
  (OUT/'results.json').write_text(json.dumps(report,indent=2)+'\n');print(name,case,'PASS',flush=True)
 shutil.rmtree(db);shutil.rmtree(frozen)
