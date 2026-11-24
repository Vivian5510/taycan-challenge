#!/usr/bin/env python3
"""Original single-in-flight ACK protocol with internal checkpoint/reclaim kill points."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'script/taycan/tests'))
sys.path.insert(0,str(ROOT/'script/taycan/benchmarks'))
from process_runner import ProcessRunner
from native_platform import read_macos_rss

def monitor(command,log,report,**kwargs):
    return ProcessRunner(command,log,int((4<<30)*1.1),120,.02,report,
                         rss_reader=read_macos_rss,rss_source='macOS libproc',**kwargs)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--results',required=True)
    p.add_argument('--hybridlog-only',action='store_true')
    p.add_argument('--rocksdb-wal-only',action='store_true')
    p.add_argument('--build-dir',type=Path,help='Explicit build for a rocksdb-wal boundary experiment')
    args=p.parse_args()
    root=Path(args.results).resolve();root.mkdir(exist_ok=False,parents=True)
    results=[]
    builds=[('hybridlog','f2-hybridlog-durable-20261030')] if args.hybridlog_only else [('checkpoint','f2-sync-checkpoint-20261030'),('commit-log','f2-sync-log-20261030')]
    if args.rocksdb_wal_only: builds=[('rocksdb-wal','f2-rocksdb-wal-20261030')]
    for strategy,build in builds:
        binary=str(args.build_dir.resolve()/'crash_worker') if args.build_dir else str(ROOT/'archive/build-history'/build/'crash_worker')
        common=[binary,'--num_keys=64','--key_size=32','--value_size=256',
                '--mem_limit_bytes='+str(4<<30),'--seed=20261021']
        points=('before-sync','after-sync','before-root','after-root') if args.hybridlog_only else ('before-root','after-root','after-reset')
        if args.rocksdb_wal_only: points=('after-wal','before-ack','after-new-wal','before-root','after-root','after-purge')
        for point in points:
            folder=root/(strategy+'-'+point);folder.mkdir();db=folder/'db'
            report=folder/'processes.jsonl'
            start=time.monotonic();record=dict(strategy=strategy,boundary=point,status='FAIL')
            with (folder/'prepare.log').open('w') as log:
                with monitor(common+['--mode=prepare','--db_path='+str(db),'--operations=64'],log,report) as worker:
                    worker.finish()
            read_fd,write_fd=os.pipe();confirmed=64;buffer=bytearray();returned=None
            env=dict(os.environ,F2_SYNC_FAULT=point,F2_SYNC_FAULT_MIN_SERIAL='65',F2_SYNC_RECLAIM_BYTES='65536')
            with (folder/'serve.log').open('w') as log:
                with monitor(common+['--mode=serve','--db_path='+str(db),'--operations=64',
                                      '--progress_fd='+str(write_fd)],log,report,
                             stdin=subprocess.PIPE,pass_fds=(write_fd,),env=env) as worker:
                    os.close(write_fd)
                    def line():
                        while b'\n' not in buffer:
                            if not select.select([read_fd],[],[],120)[0]:raise TimeoutError('ACK timeout')
                            data=os.read(read_fd,4096)
                            if not data:return None
                            buffer.extend(data)
                        value,_,rest=buffer.partition(b'\n');buffer[:]=rest
                        return value.decode()
                    assert line()=='READY'
                    while confirmed<640:
                        try:worker.proc.stdin.write(b'G');worker.proc.stdin.flush()
                        except BrokenPipeError:break
                        message=line()
                        if message is None:break
                        assert message=='ACK '+str(confirmed+1),message
                        confirmed+=1
                    returned=worker.proc.wait(timeout=5)
                    try:worker.finish()
                    except RuntimeError:pass  # own SIGKILL is expected at this injected boundary
                    worker.proc.stdin.close();os.close(read_fd)
            record.update(confirmed=confirmed,possible=confirmed+1,serve_returncode=returned)
            with (folder/'verify1.log').open('w') as log:
                with monitor(common+['--mode=verify','--db_path='+str(db),'--operations='+str(confirmed),
                             '--possible_operation='+str(confirmed+1)],log,report) as worker:
                    worker.finish()
            output=(folder/'verify1.log').read_text()
            prefix=int(next(line.split()[1] for line in output.splitlines() if line.startswith('RECOVERED_PREFIX ')))
            with (folder/'verify2.log').open('w') as log:
                with monitor(common+['--mode=verify','--db_path='+str(db),'--operations='+str(prefix)],log,report) as worker:
                    worker.finish()
            record.update(status='PASS' if returned==-9 else 'FAIL',recovered=prefix,seconds=time.monotonic()-start,
                          log_bytes=(db/'F2-COMMIT-0').stat().st_size if (db/'F2-COMMIT-0').exists() else None)
            if args.rocksdb_wal_only:
                record['wal_generations_after_reopen']=sorted(p.name for p in (db/'wal').glob('*.log'))
            results.append(record)
            (root/'results.json').write_text(json.dumps(results,indent=2)+'\n')
            print(record,flush=True)
    (root/'finished.json').write_text(json.dumps(dict(status='PASS' if all(r['status']=='PASS' for r in results) else 'FAIL',
        finished_at=datetime.now(timezone.utc).isoformat()),indent=2)+'\n')

if __name__=='__main__':main()
