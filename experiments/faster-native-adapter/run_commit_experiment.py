#!/usr/bin/env python3
"""Bounded F2 internal durability experiment; preserves every FAIL/timeout log."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'taycan_participant_sdk/benchmarks'))
from native_platform import read_macos_rss


def run(command, folder, expected=0, timeout=90):
    folder.mkdir(parents=True, exist_ok=False)
    begin = time.monotonic()
    with (folder/'output.log').open('w') as log:
        child = subprocess.Popen(command, stdout=log, stderr=log, start_new_session=True)
        peak = 0
        timed_out = False
        while child.poll() is None:
            rss = read_macos_rss(child.pid)
            peak = max(peak, rss or 0)
            if time.monotonic()-begin > timeout:
                os.killpg(child.pid, 9)
                timed_out = True
            time.sleep(.02)
        code = child.wait()
    output = (folder/'output.log').read_text()
    events = []
    for line in output.splitlines():
        try:
            events.append(json.loads(line))
        except ValueError:
            pass
    engine_errors = [line for line in output.splitlines() if '::ERROR::' in line]
    result = dict(command=command, returncode=code, expected_returncode=expected,
                  status='PASS' if code==expected and not timed_out and not engine_errors else 'FAIL',
                  engine_errors=engine_errors,
                  timeout=timed_out, rss_peak_bytes=peak, wall_seconds=time.monotonic()-begin,
                  events=events, log=str(folder/'output.log'))
    (folder/'process.json').write_text(json.dumps(result, indent=2)+'\n')
    print('[{}] {} {:.2f}s RSS {:.1f} MiB'.format(result['status'], folder.name,
          result['wall_seconds'], peak/(1<<20)), flush=True)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', required=True)
    p.add_argument('--results', required=True)
    args = p.parse_args()
    if platform.system()!='Darwin':
        p.error('this diagnostic runner uses macOS RSS; use Linux process_runner for Linux')
    root = Path(args.results).resolve()
    root.mkdir(exist_ok=False, parents=True)
    binary = str(Path(args.binary).resolve())
    metadata = dict(platform=platform.platform(), architecture=platform.machine(),
        started_at=datetime.now(timezone.utc).isoformat(),
        binary_sha256=hashlib.sha256(Path(binary).read_bytes()).hexdigest(),
        source_hashes={str(path.relative_to(ROOT)):hashlib.sha256(path.read_bytes()).hexdigest()
                       for path in Path(__file__).parent.glob('*') if path.is_file()},
        limitations=['asynchronous batch protocol, not synchronous KVEngine throughput',
                     'fixed 4096 buckets, three 12 MiB rings; NOT 4 GiB/2e8 acceptance',
                     'macOS diagnostic backend synchronizes every write chunk',
                     'sessions use disjoint keys; no cross-session same-key replay ordering',
                     'no log reclamation; explicit final checkpoint reported separately'])
    (root/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
    results = []

    def cell(name, mode, keys, rounds, batch, threads=1, wakeup=1, fault=''):
        folder = root/name; folder.mkdir()
        db = folder/'db'
        common = [binary,'--directory='+str(db),'--mode='+mode,'--keys='+str(keys),
                  '--rounds='+str(rounds),'--batch='+str(batch),'--threads='+str(threads),
                  '--wakeup='+str(wakeup)]
        record = dict(name=name, mode=mode, keys=keys, rounds=rounds, batch=batch, threads=threads,
                      wakeup=wakeup, fault=fault)
        write = run(common+['--phase=write']+(['--fault='+fault] if fault else []),
                    folder/'write', expected=-9 if fault else 0)
        record['write'] = write
        record['status'] = write['status']
        record['disk_bytes'] = sum(f.stat().st_size for f in db.rglob('*') if f.is_file())
        if write['status']=='PASS':
            if fault:
                # Only the first batch was ACKed. The next async batch is in flight.
                verify = [binary,'--phase=recover','--directory='+str(db),'--mode='+mode,
                          '--keys='+str(batch),'--rounds=1','--batch='+str(batch),'--threads=1']
            else:
                verify = common+['--phase=recover']
            record['recover'] = run(verify,folder/'recover')
            record['reopen'] = run(verify,folder/'reopen')
            if record['recover']['status']!='PASS' or record['reopen']['status']!='PASS':
                record['status']='FAIL'
        results.append(record)
        (root/'results.json').write_text(json.dumps(results,indent=2)+'\n')

    # Fixed small scope: true single pending write, then controlled asynchronous groups.
    for mode in ('checkpoint','commit-log'):
        cell(mode+'-batch1', mode, 8, 1, 1)
        cell(mode+'-batch64', mode, 256, 3, 64)
        cell(mode+'-threads4', mode, 256, 3, 64, threads=4)
    # Attribute the scheduling effect instead of labelling all wait time as disk cost.
    cell('checkpoint-polling-control','checkpoint',128,1,64,wakeup=0)
    for mode in ('checkpoint','commit-log'):
        for fault in ('after-ack','before-commit'):
            cell(mode+'-'+fault,mode,128,1,64,fault=fault)
    cell('checkpoint-before-publish','checkpoint',128,1,64,fault='before-publish')
    cell('checkpoint-final-delete','checkpoint',64,3,64)
    cell('commit-log-final-delete','commit-log',64,3,64)
    # Tail damage must preserve the already synchronized first frame. Full CRC
    # corruption must be reported instead of silently accepting partial state.
    source = root/'commit-log-before-commit/db'
    for name, corruption in [('incomplete-tail',False),('crc-corruption',True)]:
        folder=root/name;folder.mkdir(); db=folder/'db';shutil.copytree(source,db)
        log=db/'F2-COMMIT-0'
        if corruption:
            data=bytearray(log.read_bytes());data[60]^=1;log.write_bytes(data)
        else:
            with log.open('ab') as stream: stream.write(b'partial-frame')
        command=[binary,'--phase=recover','--mode=commit-log','--directory='+str(db),
                 '--keys=64','--batch=64','--rounds=1']
        outcome=run(command,folder/'recover',expected=1 if corruption else 0)
        results.append(dict(name=name,status=outcome['status'],recover=outcome))
        (root/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print('Complete; original failures remain in '+str(root),flush=True)
    (root/'finished.json').write_text(json.dumps(dict(
        status='PASS' if all(r['status']=='PASS' for r in results) else 'FAIL',
        cells=len(results), finished_at=datetime.now(timezone.utc).isoformat()),indent=2)+'\n')
    return int(any(r['status']!='PASS' for r in results))


if __name__=='__main__':
    raise SystemExit(main())
