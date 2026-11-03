#!/usr/bin/env python3
"""Exercise upstream RocksDB queue/WAL components without replacing F2."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', required=True, type=Path)
    p.add_argument('--results', required=True, type=Path)
    args = p.parse_args()
    binary = args.binary.resolve()
    out = args.results.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = ROOT/'research/rocksdb-v8.9.1'
    files = ['db/write_thread.cc', 'db/log_writer.cc', 'db/log_reader.cc',
             'db/write_batch.cc', 'file/writable_file_writer.cc', 'env/io_posix.cc']
    report = dict(status='RUNNING', started_at=datetime.now(timezone.utc).isoformat(),
                  classification='component validation; NOT F2 KVEngine performance',
                  platform=platform.platform(), binary_sha256=digest(binary),
                  upstream_commit=subprocess.check_output(['git','rev-parse','HEAD'], cwd=source, text=True).strip(),
                  upstream_source_hashes={name:digest(source/name) for name in files},
                  integration_source_hashes={x.name:digest(x) for x in Path(__file__).parent.iterdir()
                                             if x.is_file()}, tests=[])
    def save():
        (out/'results.json').write_text(json.dumps(report, indent=2)+'\n')
    def run(name, command, expected=0):
        folder=out/name
        folder.mkdir(exist_ok=False)
        start=time.monotonic()
        child=subprocess.run([str(binary)]+list(map(str,command)), capture_output=True, text=True, timeout=60)
        (folder/'stdout.log').write_text(child.stdout)
        (folder/'stderr.log').write_text(child.stderr)
        row=dict(name=name, command=[str(binary)]+list(map(str,command)), returncode=child.returncode,
                 expected_returncode=expected, seconds=time.monotonic()-start,
                 status='PASS' if child.returncode==expected else 'FAIL')
        try:
            row['measurement']=json.loads(child.stdout)
        except ValueError:
            pass
        report['tests'].append(row)
        save()
        print(name, row['status'], child.stdout.strip(), child.stderr.strip(), flush=True)
        return row
    save()
    # One synchronous API call per worker; no artificial asynchronous batches.
    for threads in (1,4):
        for repeat in (1,2):
            name=f'load-t{threads}-r{repeat}'
            db=out/(name+'-db')
            row=run(name,['load',db,threads,1024])
            if row['status']=='PASS':
                run(name+'-recover',['recover-benchmark',db,1024])
    crash_db=out/'crash-db'
    killed=run('write-kill',['write-kill',crash_db], expected=-9)
    if killed['status']=='PASS':
        run('recover-after-kill',['verify',crash_db,64])
        run('recover-twice',['verify',crash_db,64])
        # Incomplete final physical header may be discarded; existing records
        # must all survive. A CRC error in an earlier record must be rejected.
        partial=out/'partial-tail-db'
        shutil.copytree(crash_db, partial)
        with (partial/'000001.log').open('ab') as f:
            f.write(b'abc')
        run('partial-tail',['verify',partial,64])
        corrupt=out/'crc-corrupt-db'
        shutil.copytree(crash_db, corrupt)
        path=corrupt/'000001.log'
        data=bytearray(path.read_bytes());data[30]^=1;path.write_bytes(data)
        run('crc-rejected',['verify',corrupt,64], expected=1)
    for error in ('sync-error','append-error'):
        run(error,[error,out/(error+'-db'),4,64])
    report['status']='PASS' if all(x['status']=='PASS' for x in report['tests']) else 'FAIL'
    report['finished_at']=datetime.now(timezone.utc).isoformat()
    save()
    return int(report['status']!='PASS')


if __name__=='__main__':
    raise SystemExit(main())
