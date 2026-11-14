from pathlib import Path
import json
import shutil
import subprocess
import sys

BASE = Path(__file__).resolve().parent
subprocess.run([sys.executable, str(BASE / 'build_reentry.py')], check=True)
OUT = BASE / 'reentry'
OUT.mkdir(exist_ok=False)
rows = []
for variant in ('aio-nocache', 'aio-batch2'):
    db = OUT / (variant + '-db')
    process = subprocess.run([str(BASE / ('reentry-' + variant)), str(db)],
                             text=True, capture_output=True, timeout=15)
    (OUT / (variant + '.stdout.log')).write_text(process.stdout)
    (OUT / (variant + '.stderr.log')).write_text(process.stderr)
    rows.append(dict(variant=variant, returncode=process.returncode,
                     status='PASS' if process.returncode == 0 else 'FAIL',
                     stdout=process.stdout))
    if process.returncode == 0:
        shutil.rmtree(db)
    (OUT / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
    print(variant, process.returncode, process.stdout.strip(), flush=True)
