#!/usr/bin/env python3
"""Run the same resolved workload through the F2-native adapter and RocksDB."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import sys

import run_performance as performance


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, help='F2-native CMake build')
    parser.add_argument('--baseline-build-dir', required=True, help='RocksDB adapter CMake build')
    parser.add_argument('--results-dir', required=True, help='New guest-local root for both engines')
    args, common = parser.parse_known_args(argv)
    root = Path(args.results_dir).resolve()
    if '--dry-run' in common:
        # Resolve once through the same parser; do not create anything.
        return performance.main(['--build-dir', args.build_dir] + common)
    sharing = performance.competitors()
    if sharing and '--allow-shared-host' not in common:
        parser.error('another evaluation is running; comparison must wait for an idle guest')
    root.mkdir(parents=True, exist_ok=False)
    results = []
    try:
        for engine, build in [('f2_native_completion_baseline', args.build_dir), ('rocksdb', args.baseline_build_dir)]:
            folder = root/engine
            code = performance.main(['--build-dir', build, '--results-dir', str(folder),
                                     '--expect-engine', engine] + common)
            report = json.loads((folder/'results.json').read_text())
            results.append(report)
            if code and '--keep-going' not in common:
                break
    finally:
        comparison = dict(schema_version=1, cross_engine_comparison=True,
                          status='INCOMPLETE', engines=[r['tested_engine'] for r in results],
                          completed_at_utc=datetime.now(timezone.utc).isoformat(),
                          shared_host_processes=sharing, reports=[str(root/r['tested_engine']/'results.json') for r in results])
        if len(results) == 2:
            comparable = results[0]['parameters'] == results[1]['parameters']
            comparison['matched_parameters'] = comparable
            comparison['status'] = 'PASS' if comparable and all(r['status']=='PASS' for r in results) else 'FAIL'
            comparison['parameters'] = results[0]['parameters']
        (root/'comparison.json').write_text(json.dumps(comparison, indent=2)+'\n')
        (root/'README.md').write_text(
            '# 磁盘 KV 对比运行\n\n状态：{}。同一负载程序分别链接两个引擎，逐个运行。\n\n'.format(comparison['status']) +
            ('本轮显式允许共享机器，仅用于工具验证，不能用于性能排名。\n\n' if sharing else '') +
            '\n'.join('- [{}]({}/README.md)'.format(r['tested_engine'], r['tested_engine']) for r in results)+'\n')
    return 0 if comparison['status']=='PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
