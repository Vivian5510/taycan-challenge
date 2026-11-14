from pathlib import Path
import subprocess,json
r=Path(__file__).resolve().parents[2];b=Path(__file__).resolve().parent
selected=json.loads((b/'selected.json').read_text())['variant']
build=r/('build/f2-io-'+selected+'-20261114')
command=['c++','-std=c++17','-O2','-I'+str(r/'taycan_participant_sdk/include'),str(r/'analysis/f2-recovery-sync-audit-20261114/ack_suffix_probe.cc'),str(build/'engine/libcontestant_engine.a'),str(build/'engine/libfaster_cpp.a'),'/opt/homebrew/lib/libtbb.12.20.dylib',str(r/'build/deps/rocksdb-8.9.1-native-arm64/lib/librocksdb.a'),'-Wl,-rpath,/opt/homebrew/lib','-o',str(b/'ack_suffix_probe')]
subprocess.run(command,check=True)

