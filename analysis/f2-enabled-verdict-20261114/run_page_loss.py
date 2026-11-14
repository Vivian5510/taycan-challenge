"""Model partial persistence of an unacknowledged 100KiB write; preserve all ACKs.
This is a disk-image transformation model, not a physical power-cut test.
"""
from pathlib import Path
import hashlib,json,os,shutil,subprocess
ROOT=Path(__file__).resolve().parents[2];BASE=Path(__file__).resolve().parent
OUT=BASE/'pending-page-loss';assert not OUT.exists();OUT.mkdir()
probe=ROOT/'build/f2-enabled-verdict-20261114/engine/f2_sync_api_probe'
source=OUT/'source-db'
env={k:v for k,v in os.environ.items() if not k.startswith(('F2_','TAYCAN_'))}
env.update(F2_SYNC_FAULT='before-data-sync',F2_SYNC_FAULT_MIN_SERIAL='68')
# write-kill performs 67 operations, with binary repeated values and deletion.
# Obtain a stable acknowledged prefix; its own SIGKILL has no graceful close.
a=subprocess.run([str(probe),'write-kill',str(source)],capture_output=True,text=True,env={k:v for k,v in env.items() if k not in ('F2_SYNC_FAULT','F2_SYNC_FAULT_MIN_SERIAL')},timeout=60)
(OUT/'prepare.stdout').write_text(a.stdout);(OUT/'prepare.stderr').write_text(a.stderr);assert a.returncode==-9
expected=next(s.split()[1] for s in a.stdout.splitlines() if s.startswith('EXPECTED '))
# Existing embedded-marker probe appends one 100KiB value, then is killed before sync.
b=subprocess.run([str(probe),'embedded-marker-write-kill',str(source)],capture_output=True,text=True,env=dict(env,F2_SYNC_FAULT_MIN_SERIAL='68'),timeout=60)
(OUT/'pending.stdout').write_text(b.stdout);(OUT/'pending.stderr').write_text(b.stderr);assert b.returncode==-9 and 'F2_SYNC_FAULT before-data-sync' in b.stderr
# Open published root may already cover all ACKs; obtain that base tail.
root_tail=int((source/'F2-SYNC-CURRENT').read_text().splitlines()[3])
log=source/'hot/log.log0';data=log.read_bytes();stable=root_tail
# The second Open checkpoints the recovered 67-operation prefix before new data.
assert stable and stable%512==0 and len(data)>stable+8192
boundary=(stable//4096+1)*4096
cases={'all-pending-lost':data[:stable],'all-pending-survives':data,'earlier-pending-page-lost':data[:stable]+bytes(boundary-stable)+data[boundary:],'last-pending-page-lost':data[:boundary]+bytes(len(data)-boundary)}
report=dict(classification='partial unsynced page disk-image model, not real power cut',budget=4<<30,acknowledged_operations=67,stable_tail=stable,source_bytes=len(data),binary_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),cases=[])
for name,image in cases.items():
 assert image[:stable]==data[:stable]
 db=OUT/name;shutil.copytree(source,db);(db/'hot/log.log0').write_bytes(image)
 row=dict(case=name,acknowledged_bytes_unchanged=True,checks=[]);report['cases'].append(row)
 for attempt in range(2):
  p=subprocess.run([str(probe),'verify',str(db),expected],capture_output=True,text=True,timeout=60,env={k:v for k,v in env.items() if not k.startswith('F2_SYNC_FAULT')})
  (OUT/(name+f'-verify{attempt}.stdout')).write_text(p.stdout);(OUT/(name+f'-verify{attempt}.stderr')).write_text(p.stderr)
  row['checks'].append(dict(returncode=p.returncode))
  if p.returncode:break
 row['status']='PASS' if all(c['returncode']==0 for c in row['checks']) else 'FAIL'
 if row['status']=='PASS':shutil.rmtree(db)
 (OUT/'results.json').write_text(json.dumps(report,indent=2)+'\n')
 print(name,row['status'],flush=True)
