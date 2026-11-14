from pathlib import Path
import subprocess
r=Path(__file__).resolve().parents[2];b=Path(__file__).resolve().parent
command=['c++','-std=c++17','-O2','-I'+str(r/'taycan_participant_sdk/include'),str(b/'ack_suffix_probe.cc'),str(r/'build/f2-recovery-sync-audit-20261114/engine/libcontestant_engine.a'),str(r/'build/f2-recovery-sync-audit-20261114/engine/libfaster_cpp.a'),'/opt/homebrew/lib/libtbb.12.20.dylib',str(r/'build/deps/rocksdb-8.9.1-native-arm64/lib/librocksdb.a'),'-Wl,-rpath,/opt/homebrew/lib','-o',str(b/'ack_suffix_probe')]
subprocess.run(command,check=True)

command=['c++','-std=c++17','-O2','-I'+str(r/'taycan_participant_sdk/include'),str(b/'ack_suffix_probe.cc'),str(r/'build/rocks-recovery-sync-audit-20261114/engine/libcontestant_engine.a'),str(r/'build/deps/rocksdb-fsync-8.9.1-20261114/lib/librocksdb.a'),'-o',str(b/'rocks_ack_suffix_probe')]
subprocess.run(command,check=True)
