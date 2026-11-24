#!/usr/bin/env python3
"""Interleave identical durable loads to measure F2-native apply candidates."""
from datetime import datetime, timezone
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'script/taycan/tests'))
sys.path.insert(0,str(ROOT/'script/taycan/benchmarks'))
from process_runner import ProcessRunner
from native_platform import read_macos_rss


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',type=Path,required=True)
    p.add_argument('--results',type=Path,required=True)
    p.add_argument('--collect-experiment',action='store_true')
    args=p.parse_args();binary=args.binary.resolve();out=args.results.resolve()
    out.mkdir(parents=True,exist_ok=False)
    report=dict(status='RUNNING',started_at=datetime.now(timezone.utc).isoformat(),
        classification='macOS ARM64 synchronous durable API experiment; not official scoring',
        binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
        mode_meaning={'decoded':'WAL batch decoded, owned contexts'},
        ordering='alternate 0/50 microsecond policy order; fresh database per cell',runs=[],summary=[])
    def save():(out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    def cell(mode,threads,repeat,count=1024,budget=4<<30,kind='hot',collect_us=0):
        variant=mode+('-wait'+str(collect_us) if collect_us else '')
        name=f'{kind}-{variant}-t{threads}-r{repeat}';folder=out/name;folder.mkdir()
        command=[str(binary),'--data-dir='+str(folder/'db'),'--workload=load',
            '--num-keys='+str(count),'--threads='+str(threads),'--mem-limit-bytes='+str(budget),
            '--key-size=32','--value-size=256','--duration-seconds=1','--warmup-seconds=0',
            '--sample-every=1','--progress-interval-seconds=1','--seed=20261021']
        row=dict(case=name,kind=kind,mode=mode,collect_us=collect_us,threads=threads,repeat=repeat,command=command,
                 environment={'F2_WAL_APPLY_MODE':mode,'F2_WAL_GROUP_WAIT_US':str(collect_us)},status='FAIL')
        print('START',name,flush=True)
        try:
            with (folder/'output.log').open('w') as log:
                with ProcessRunner(command,log,budget*11//10+(8<<20),120,.02,folder/'processes.jsonl',
                    rss_reader=read_macos_rss,rss_source='macOS libproc; development only',
                    env=dict(os.environ,F2_WAL_APPLY_MODE=mode,F2_WAL_GROUP_WAIT_US=str(collect_us))) as worker:
                    row['monitor']=worker.finish()
            output=(folder/'output.log').read_text()
            row['measurement']=next(json.loads(line) for line in output.splitlines() if line.startswith('{"type":"result"'))
            if row['measurement']['status']!='PASS' or row['measurement']['operations']!=count:
                raise RuntimeError('load did not complete full requested key count')
            for tag,key in [('F2_WAL_TIMING ','timing'),('F2_ROCKS_WAL ','wal')]:
                line=next((line for line in output.splitlines() if line.startswith(tag)),None)
                if line is not None:row[key]=dict((k,int(v)) for k,v in re.findall(r'(\w+)=(\d+)',line))
            row['group_histogram']=next((line for line in output.splitlines() if line.startswith('F2_WAL_GROUP_HIST')),None)
            row['status']='PASS'
        except Exception as error:row['reason']=str(error)
        report['runs'].append(row);save()
        print('COMPLETE',name,row['status'],row.get('measurement',{}).get('ops_per_second'),flush=True)
        return row['status']=='PASS'
    save()
    for threads in (1,4,16):
        for repeat in (1,2,3):
            for wait in ((0,50) if repeat%2 else (50,0)):
                if not cell('decoded',threads,repeat,collect_us=wait):report['status']='FAIL';save();return 1
    for wait in (0,50):
        if not cell('decoded',16,1,32768,32<<20,'spill',wait):report['status']='FAIL';save();return 1
    for threads in (1,4,16):
        variants=[('decoded',0),('decoded',50)]
        for mode,wait in variants:
            rows=[r for r in report['runs'] if r['kind']=='hot' and r['mode']==mode and r['threads']==threads and r['collect_us']==wait]
            report['summary'].append(dict(mode=mode,collect_us=wait,threads=threads,repeats=len(rows),
                ops_per_second=statistics.median(r['measurement']['ops_per_second'] for r in rows),
                min_ops_per_second=min(r['measurement']['ops_per_second'] for r in rows),
                max_ops_per_second=max(r['measurement']['ops_per_second'] for r in rows),
                put_p99_us=statistics.median(r['measurement']['put_latency']['p99_us'] for r in rows),
                apply_us_per_operation=statistics.median(r['timing']['apply_ns']/r['measurement']['operations']/1000 for r in rows) if all('timing' in r for r in rows) else None,
                sync_fraction=statistics.median(r['timing']['sync_ns']/r['timing']['leader_ns'] for r in rows) if all('timing' in r for r in rows) else None,
                average_group=statistics.median(r['measurement']['operations']/r['wal']['groups'] for r in rows)))
    report.update(status='PASS',finished_at=datetime.now(timezone.utc).isoformat());save()
    print(json.dumps(report['summary'],indent=2),flush=True)
    return 0


if __name__=='__main__':raise SystemExit(main())
