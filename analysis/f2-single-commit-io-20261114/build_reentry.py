"""Build the same small callback probe against the frozen baseline/candidate."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BASE = Path(__file__).resolve().parent
for line in subprocess.check_output(['ps', '-Ao', 'pid=,command='], text=True).splitlines():
    if ' -lc ' not in line and '/taycan_kv_bench ' in line and '/build/' in line:
        raise SystemExit('Do not compile during performance measurement: ' + line)
for name, limit in [('aio-nocache', 1), ('aio-batch2', 2)]:
    build = ROOT / ('build/f2-io-' + name + '-20261114')
    command = ['c++', '-std=c++17', '-O2', '-DF2_NATIVE_POSIX_AIO=1',
               '-DF2_AIO_COMPLETION_BATCH=' + str(limit),
               '-I' + str(build / 'engine/faster-src'), '-I/opt/homebrew/include',
               str(BASE / 'completion_reentry_probe.cc'),
               str(build / 'engine/libfaster_cpp.a'),
               '/opt/homebrew/lib/libtbb.12.20.dylib',
               str(ROOT / 'build/deps/rocksdb-8.9.1-native-arm64/lib/librocksdb.a'),
               '-Wl,-rpath,/opt/homebrew/lib', '-o', str(BASE / ('reentry-' + name))]
    subprocess.run(command, check=True)
