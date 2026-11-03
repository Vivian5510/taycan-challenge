#!/usr/bin/env python3
"""Contest-target correctness: 4 GiB, 16..32 B keys, 256 B generated values.

Public scenario sizes, fixed Basic cases, monitoring and crash protocol are kept.
Use run_tests.py directly to reproduce the original SDK argument defaults.
"""
import argparse
import json
from pathlib import Path

import run_tests

CONFIG = json.loads((Path(__file__).resolve().parents[1] / 'evaluation-config.json').read_text())


def main(argv=None):
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--key-size', type=int,
                        choices=range(CONFIG['key_size_bytes']['min'], CONFIG['key_size_bytes']['max'] + 1),
                        default=CONFIG['key_size_bytes']['default'])
    parser.add_argument('--value-size', type=int, choices=[CONFIG['value_size_bytes']],
                        default=CONFIG['value_size_bytes'])
    budget = CONFIG['memory_tiers'][CONFIG['correctness']['memory_tier']]['mem_limit_bytes']
    parser.add_argument('--mem-limit-bytes', type=int, choices=[budget], default=budget)
    args, rest = parser.parse_known_args(argv)
    if '--help' in rest or '-h' in rest:
        print(__doc__ + '\n')
    return run_tests.main(rest + ['--strict-rss-limit', '--key-size', str(args.key_size),
                                '--value-size', str(args.value_size),
                                '--mem-limit-bytes', str(args.mem_limit_bytes)])

if __name__ == '__main__':
    raise SystemExit(main())
