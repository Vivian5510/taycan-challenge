#!/usr/bin/env python3
"""Package code/submission-engine with the unchanged official SDK packager."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=ROOT / 'taycan_participant_sdk_2026_11_17.tar.gz')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/engine-centos7-20261127.tar.gz')
    args = parser.parse_args()
    source = ROOT / 'code/submission-engine'
    for line in (source / 'vendor/SOURCES.sha256').read_text().splitlines():
        digest, name = line.split('  ', 1)
        if sha(source / 'vendor' / name) != digest:
            raise RuntimeError('Vendor checksum mismatch: ' + name)
    for path in source.rglob('*'):
        if path.is_symlink():
            raise RuntimeError('Submission cannot contain symlinks: ' + str(path))
        if path.is_file() and path.suffix in ('.so', '.a', '.o', '.dylib', '.pyc'):
            raise RuntimeError('Unexpected prebuilt artifact: ' + str(path))
    with tempfile.TemporaryDirectory(prefix='centos7-engine-package-') as directory:
        temporary = Path(directory)
        with tarfile.open(str(args.sdk)) as archive:
            for member in archive.getmembers():
                if member.name.startswith('/') or '..' in Path(member.name).parts:
                    raise RuntimeError('Unexpected SDK path: ' + member.name)
                if not (member.isfile() or member.isdir()):
                    raise RuntimeError('Unexpected SDK entry: ' + member.name)
            archive.extractall(str(temporary))
        sdk = temporary / 'taycan_participant_sdk'
        shutil.rmtree(sdk / 'engine')
        shutil.copytree(source, sdk / 'engine')
        subprocess.run(['bash', str(sdk / 'package.sh'), 'engine'], check=True)
        output = args.output.resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(sdk / 'dist/engine.tar.gz', output)
    print('archive=' + str(output))
    print('sha256=' + sha(output))

if __name__ == '__main__':
    main()
