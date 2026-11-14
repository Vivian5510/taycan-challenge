#!/bin/zsh
# Run only after the performance process and all workers have exited.
F2_VERDICT_TESTS=overwrite,delete,mixed,random,demand_load,memory_limit,checkpoint F2_VERDICT_RUN=public-final F2_VERDICT_TIMEOUT=600 python3 analysis/f2-enabled-verdict-20261114/run_correctness.py
