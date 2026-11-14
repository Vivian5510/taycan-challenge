from pathlib import Path
import json,os,sys,subprocess
ROOT=Path(__file__).resolve().parents[2];BASE=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'taycan_participant_sdk/tests'))
sys.path.insert(0,str(ROOT/'taycan_participant_sdk/benchmarks'))
import run_tests
from process_runner import ProcessRunner
from native_platform import read_macos_rss
out=BASE/'rocks-correctness';assert not out.exists();out.mkdir()
code=run_tests.main(['--native-development','--strict-rss-limit','--build-dir',str(ROOT/'build/rocks-recovery-sync-audit-20261114'),'--results-dir',str(out),'--tests','basic,recovery,concurrent,crash_recovery','--mem-limit-bytes',str(4<<30),'--timeout','300'])
raise SystemExit(code)
