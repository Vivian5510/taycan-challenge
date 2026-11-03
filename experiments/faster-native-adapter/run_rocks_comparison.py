#!/usr/bin/env python3
"""Run the identical synchronous SDK workload on F2 and RocksDB, sequentially."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--results',required=True,type=Path)
    args=p.parse_args()
    out=args.results.resolve();out.mkdir(exist_ok=False,parents=True)
    library=ROOT/'build/deps/rocksdb-8.9.1-native-arm64/lib/librocksdb.a'
    dependency=dict(rocksdb_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT/'research/rocksdb-v8.9.1',text=True).strip(),
                    library=str(library),library_sha256=sha(library),
                    shared_workload_sha256=sha(ROOT/'taycan_participant_sdk/benchmarks/kv_bench.cc'))
    report=dict(status='RUNNING',started_at=datetime.now(timezone.utc).isoformat(),
                classification='macOS ARM64 small diagnostic; both synchronous durable ACK',
                dependency=dependency,engines=[],summary=[])
    def save():(out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n')
    save()
    profiles=[('f2','f2-rocksdb-wal-20261030','f2_sync_rocksdb-wal'),
              ('rocksdb','native-rocksdb-8.9.1-arm64','rocksdb')]
    data={}
    for name,build,identity in profiles:
        cmd=[sys.executable,str(ROOT/'taycan_participant_sdk/benchmarks/run_performance.py'),
             '--native-development','--build-dir',str(ROOT/'build'/build),'--expect-engine',identity,
             '--profile','dev','--num-keys','1024','--mem-limit-bytes',str(4<<30),
             '--key-size','32','--value-size','256','--threads','1,4',
             '--workloads','load,read,contest_mix','--duration-seconds','5','--warmup-seconds','1',
             '--repeats','2','--mode','latency','--timeout-seconds','120','--results-dir',str(out/name)]
        print('START',name,flush=True)
        with (out/(name+'-driver.log')).open('w') as log:
            child=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,timeout=360)
        row=dict(engine=name,returncode=child.returncode,status='PASS' if child.returncode==0 else 'FAIL',command=cmd)
        report['engines'].append(row);save()
        if child.returncode:
            report['status']='FAIL';save();return 1
        data[name]=json.loads((out/name/'results.json').read_text())
        if data[name]['status']!='PASS':raise RuntimeError('incomplete report')
        print('COMPLETE',name,len(data[name]['runs']),'cells',flush=True)
    if data['f2']['parameters']!=data['rocksdb']['parameters']:raise RuntimeError('parameter mismatch')
    for path in ('benchmarks/kv_bench.cc','evaluation-config.json'):
        if data['f2']['source_hashes'][path]!=data['rocksdb']['source_hashes'][path]:raise RuntimeError('driver source mismatch')
    if sha(library)!=dependency['library_sha256']:raise RuntimeError('library changed while measuring')
    report['parameters']=data['f2']['parameters']
    for workload in ('load','read','contest_mix'):
        for threads in (1,4):
            row=dict(workload=workload,threads=threads)
            for engine in ('f2','rocksdb'):
                cells=[r for r in data[engine]['runs'] if r['workload']==workload and r['threads']==threads]
                if len(cells)!=2 or any(r['status']!='PASS' for r in cells):raise RuntimeError('missing/failed cell')
                row[engine]=dict(ops_per_second=statistics.median(r['measurement']['ops_per_second'] for r in cells),
                    rss_peak_bytes=max(r['rss_peak_bytes'] for r in cells),
                    put_p99_us=statistics.median(r['measurement']['put_latency']['p99_us'] for r in cells)
                        if workload!='read' else None,
                    close_seconds=statistics.median(r['measurement']['close_seconds'] for r in cells),
                    data_bytes_after_close=statistics.median(r['measurement']['data_bytes_after_close'] for r in cells))
            row['f2_over_rocksdb']=row['f2']['ops_per_second']/row['rocksdb']['ops_per_second']
            report['summary'].append(row)
            print(json.dumps(row),flush=True)
    report.update(status='PASS',finished_at=datetime.now(timezone.utc).isoformat());save()
    return 0


if __name__=='__main__':raise SystemExit(main())
