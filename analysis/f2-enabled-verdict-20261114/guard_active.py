"""Supplement the already-loaded controller's local disk guard; never restart it."""
from pathlib import Path
import json,os,signal,subprocess,time
base=Path(__file__).resolve().parent
while True:
    listing=subprocess.check_output(['ps','-Ao','pid=,command='],text=True)
    workers=[]
    for line in listing.splitlines():
        fields=line.strip().split(maxsplit=1)
        if len(fields)==2 and fields[1].startswith(str(base.parents[1]/'build/f2-enabled-verdict-20261114')+'/'):
            workers.append(int(fields[0]))
    alive=any('python3 analysis/f2-enabled-verdict-20261114/run_correctness.py' in line and ' -lc ' not in line for line in listing.splitlines())
    if not alive and not workers:break
    for folder in (base/'public-enabled').iterdir():
        if not folder.is_dir():continue
        total=0
        for parent,_,names in os.walk(folder):
            for name in names:
                try:total+=os.stat(os.path.join(parent,name),follow_symlinks=False).st_size
                except FileNotFoundError:pass
        if total>=2<<30:
            resource=folder/'resources.jsonl'
            if not resource.exists():continue
            pid=json.loads(resource.read_text().splitlines()[-1])['pid']
            if pid in workers:
                (folder/'external-guard.json').write_text(json.dumps(dict(reason='local_disk_guard_2GiB_not_engine_assertion',pid=pid,directory_bytes=total)))
                os.killpg(pid,signal.SIGKILL)
    time.sleep(.5)
