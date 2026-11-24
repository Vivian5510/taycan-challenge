#!/usr/bin/env python3
"""Run the current SDK correctness scenarios with the contest's 4 GiB budget.

Each public scenario keeps its own default key/value sizes and data volume.
"""
import json
from pathlib import Path
import sys

PROJECT = Path(__file__).resolve().parents[3]
SDK = PROJECT / 'code' / 'taycan_participant_sdk'
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_tests

CONFIG = json.loads((SDK / 'evaluation-config.json').read_text())


def main(argv=None):
    budget = CONFIG['memory_tiers'][CONFIG['correctness']['memory_tier']]['mem_limit_bytes']
    arguments = list(sys.argv[1:] if argv is None else argv)
    return run_tests.main(arguments + ['--mem-limit-bytes', str(budget)])

if __name__ == '__main__':
    raise SystemExit(main())
