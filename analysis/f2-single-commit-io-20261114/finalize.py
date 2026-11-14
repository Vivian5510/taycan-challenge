"""Final audit and cleanup only after all scoped workers finish."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BASE = Path(__file__).resolve().parent
for line in subprocess.check_output(['ps', '-Ao', 'pid=,command='], text=True).splitlines():
    if ' -lc ' in line:
        continue
    if '/build/f2-io-' in line:
        raise SystemExit('Wait for scoped worker: ' + line)
verification = json.loads((BASE / 'confirmation-verification.json').read_text())
assert verification['status'] == 'PASS' and verification['completed'] == 12
manifest = json.loads((BASE / 'source-manifest.json').read_text())
for relative, sha in manifest['source_sha256'].items():
    if relative.endswith('/README.md'):
        continue  # Documentation was updated after the tested binaries froze.
    assert hashlib.sha256((ROOT / relative).read_bytes()).hexdigest() == sha, relative
assert subprocess.check_output(['git', 'rev-parse', 'main'], text=True).strip().startswith('26e4466')
reentry = json.loads((BASE / 'reentry/results.json').read_text())
assert reentry[0]['status'] == 'PASS' and reentry[1]['status'] == 'FAIL'
assert (BASE / 'batch2-compaction/exit-code').read_text().strip() == '0'
assert sum(line.startswith('PASS trial=') for line in (BASE / 'batch2-crash-runner.log').read_text().splitlines()) == 5
checks = json.loads((BASE / 'validation-batch2/results.json').read_text())
assert checks['status'] == 'PASS' and len(checks['tests']) == 71

cleanup = []
# These overall test suites passed, including intentional EIO/corruption cases.
# Real public failures, before-timeout DBs and reentry failure are not deleted.
for name in ('validation', 'validation-batch2', 'syscall-boundaries',
             'syscall-boundaries-hot-inline', 'syscall-boundaries-batch2'):
    folder = BASE / name
    for lock in folder.rglob('LOCK'):
        db = lock.parent
        if not (db / 'hot').is_dir():
            continue
        files = [dict(path=str(p.relative_to(db)), bytes=p.stat().st_size)
                 for p in db.rglob('*') if p.is_file()]
        cleanup.append(dict(path=str(db.relative_to(ROOT)), files=files))
for record in cleanup:
    shutil.rmtree(ROOT / record['path'])
archive = BASE / 'successful-diagnostic-cleanup.json'
if archive.exists():
    cleanup = json.loads(archive.read_text()) + cleanup
archive.write_text(json.dumps(cleanup, indent=2) + '\n')

status = dict(main='26e4466', recovery_fix_merged=True, performance_cells=12,
              fixed_parameters_verified=True, binary_hashes_verified=True,
              callback_reentry_failure_reproduced=True, optimization_adopted=False,
              default_completion_batch=1, group_commit=False,
              clean_successful_databases=len(cleanup),
              formal_scale_complete=False)
(BASE / 'final-verification.json').write_text(json.dumps(status, indent=2) + '\n')
print(status)
