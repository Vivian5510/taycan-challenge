#!/usr/bin/env python3
"""Compare the same synchronous SDK API; preserve original tests and crash protocol."""
import argparse
import contextlib
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from unittest import mock

ROOT=Path(__file__).resolve().parents[2]
SDK=ROOT/'taycan_participant_sdk'
sys.path.insert(0,str(SDK/'tests'))
sys.path.insert(0,str(SDK/'benchmarks'))
from process_runner import ProcessRunner
from native_platform import read_macos_rss
import crash_test

def monitored(*a,**k):
    k.update(rss_reader=read_macos_rss,rss_source='macOS libproc; development only')
    return ProcessRunner(*a,**k)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results',required=True)
    args=parser.parse_args()
    root=Path(args.results).resolve();root.mkdir(parents=True,exist_ok=False)
    rows=[]
    for mode,build in [('checkpoint','f2-sync-checkpoint-20261030'),('commit-log','f2-sync-log-20261030')]:
        build=ROOT/'build'/build
        folder=root/mode;folder.mkdir()
        metadata=dict(mode=mode,binary_sha256=hashlib.sha256((build/'taycan_kv_bench').read_bytes()).hexdigest(),
            started_at=datetime.now(timezone.utc).isoformat(),classification='macOS synchronous API experiment; not contest scale')
        (folder/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
        fault=folder/'crash';fault.mkdir()
        try:
            with (fault/'output.log').open('w') as log:
                with contextlib.redirect_stdout(log),contextlib.redirect_stderr(log):
                    with mock.patch.object(crash_test,'ProcessRunner',monitored):
                        crash_test.main(['--worker',str(build/'crash_worker'),'--num-keys','8',
                            '--key-size','32','--value-size','256','--mem-limit-bytes',str(4<<30),
                            '--trials','3','--timeout','120','--data-dir',str(fault),
                            '--monitor-report',str(fault/'processes.jsonl')])
            status='PASS'
        except Exception as error:
            status='FAIL: '+str(error)
        rows.append(dict(mode=mode,test='crash_recovery',status=status))
        print(mode,'crash',status,flush=True)
        (root/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
        perf=folder/'performance'
        command=[sys.executable,str(SDK/'benchmarks/run_performance.py'),
            '--native-development','--build-dir',str(build),'--profile','dev',
            '--num-keys','64','--mem-limit-bytes',str(4<<30),'--key-size','32','--value-size','256',
            '--threads','1,4','--workloads','load,read,contest_mix','--duration-seconds','3',
            '--warmup-seconds','1','--repeats','2','--mode','latency','--timeout-seconds','120',
            '--results-dir',str(perf)]
        with (folder/'performance-driver.log').open('w') as log:
            child=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,timeout=900)
        rows.append(dict(mode=mode,test='same_api_performance',returncode=child.returncode,
                         status='PASS' if child.returncode==0 else 'FAIL',command=command))
        (root/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
        print(mode,'performance',child.returncode,flush=True)
    (root/'finished.json').write_text(json.dumps(dict(status='PASS' if all(r['status']=='PASS' for r in rows) else 'FAIL',
        finished_at=datetime.now(timezone.utc).isoformat()),indent=2)+'\n')
    return int(any(r['status']!='PASS' for r in rows))

if __name__=='__main__':
    raise SystemExit(main())
