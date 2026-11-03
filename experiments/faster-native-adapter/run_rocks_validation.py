#!/usr/bin/env python3
"""F2 + native RocksDB WAL: SDK correctness and real crash/recovery probes."""
import argparse
import contextlib
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'taycan_participant_sdk/tests'))
sys.path.insert(0,str(ROOT/'taycan_participant_sdk/benchmarks'))
from process_runner import ProcessRunner
from native_platform import read_macos_rss
import crash_test


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir',required=True,type=Path)
    p.add_argument('--results',required=True,type=Path)
    args=p.parse_args()
    out=args.results.resolve();out.mkdir(exist_ok=False,parents=True)
    build=args.build_dir.resolve();probe=build/'engine/f2_sync_api_probe'
    rows=[]
    report=dict(status='RUNNING',started_at=datetime.now(timezone.utc).isoformat(),
        classification='macOS ARM64 development correctness; not full contest acceptance',
        binary_sha256={str(binary.relative_to(build)):hashlib.sha256(binary.read_bytes()).hexdigest()
            for binary in [build/n for n in ('basic_test','recovery_test','concurrent_test','crash_worker','taycan_kv_bench','engine/f2_sync_api_probe')]},tests=rows)
    def save():
        (out/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    def run(name,cmd,expected=0,env=None):
        started=time.monotonic()
        try:
            child=subprocess.run(list(map(str,cmd)),capture_output=True,text=True,timeout=120,env=env)
            row=dict(test=name,status='PASS' if child.returncode==expected else 'FAIL',
                     returncode=child.returncode,expected_returncode=expected,
                     command=list(map(str,cmd)),seconds=time.monotonic()-started)
            (out/(name+'.stdout.log')).write_text(child.stdout)
            (out/(name+'.stderr.log')).write_text(child.stderr)
        except subprocess.TimeoutExpired as error:
            row=dict(test=name,status='FAIL',reason=str(error),seconds=time.monotonic()-started)
            rows.append(row);save();raise
        rows.append(row);save();print(name,row['status'],flush=True)
        if row['status']!='PASS': raise RuntimeError('check failed: '+name)
        return child
    save()
    common=['--num_keys=64','--key_size=32','--value_size=256','--mem_limit_bytes=4294967296','--seed=20261021']
    for name in ('basic','recovery','concurrent'):
        run(name,[build/(name+'_test')]+common+['--gtest_color=no'])
    for name in ('sync-error','io-error','apply-error'):
        run(name,[probe,name,out/(name+'-db')])
    same=out/'same-key-db'
    child=run('same-key-write',[probe,'write-kill',same],expected=-9)
    expected=next(x.split()[1] for x in child.stdout.splitlines() if x.startswith('EXPECTED '))
    for repeat in (1,2):run('same-key-recover-'+str(repeat),[probe,'verify',same,expected])

    # Tiny memory buffers force the hot log to reach disk; 64 KiB values also
    # span multiple RocksDB physical WAL blocks. Several full bases rotate.
    span=out/'span-db';env=dict(os.environ,F2_PROBE_BUDGET_BYTES=str(32<<20))
    child=run('span-write',[probe,'span-write-kill',span],expected=-9,env=env)
    if 'SPANNED_PREFIX_ACK 649' not in child.stdout:raise RuntimeError('incorrect acknowledged count')
    token=(span/'F2-SYNC-CURRENT').read_text().splitlines()[0]
    for case in ('partial-tail','crc-corrupt','missing-wal','root-corrupt'):
        db=out/(case+'-db');shutil.copytree(span,db)
        wal=db/'wal'/(token+'.log')
        if case=='partial-tail':
            with wal.open('ab') as f:f.write(b'abc')
        elif case=='crc-corrupt':
            data=bytearray(wal.read_bytes())
            if len(data)<64:raise RuntimeError('not enough WAL bytes to inject corruption')
            data[30]^=1;wal.write_bytes(data)
        elif case=='missing-wal':wal.rename(wal.with_suffix('.preserved'))
        else:
            root=db/'F2-SYNC-CURRENT';lines=root.read_text().splitlines();lines[-1]=str(int(lines[-1])^1);root.write_text('\n'.join(lines)+'\n')
        run(case,[probe,'span-verify',db],expected=0 if case=='partial-tail' else 1,env=env)
    for repeat in (1,2):run('span-recover-'+str(repeat),[probe,'span-verify',span],env=env)

    def monitored(*a,**kw):
        kw.update(rss_reader=read_macos_rss,rss_source='macOS libproc; not Linux VmRSS')
        return ProcessRunner(*a,**kw)
    crash=out/'crash';crash.mkdir()
    crash_test.ProcessRunner=monitored
    with (crash/'output.log').open('w') as log,contextlib.redirect_stdout(log),contextlib.redirect_stderr(log):
        crash_test.main(['--worker',str(build/'crash_worker'),'--num-keys','64',
            '--key-size','32','--value-size','256','--mem-limit-bytes',str(4<<30),
            '--trials','5','--timeout','120','--rss-limit-bytes',str((4<<30)*11//10),
            '--data-dir',str(crash),'--monitor-report',str(crash/'processes.jsonl')])
    rows.append(dict(test='original-sdk-crash-protocol',trials=5,status='PASS'));save()
    report.update(status='PASS',finished_at=datetime.now(timezone.utc).isoformat());save()
    print('completed',len(rows),'checks',flush=True)
    return 0


if __name__=='__main__':
    raise SystemExit(main())
