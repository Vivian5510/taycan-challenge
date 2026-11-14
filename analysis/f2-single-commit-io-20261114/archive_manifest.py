"""Archive bounded evidence paths only, leaving failed databases in place."""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[2]
BASE = Path(__file__).resolve().parent
dbs = {path.parent for path in BASE.rglob('LOCK')}
evidence = []
for path in BASE.rglob('*'):
    if not path.is_file() or '__pycache__' in path.parts:
        continue
    if path.name == 'evidence-files.json':
        continue
    if any(db == path.parent or db in path.parents for db in dbs):
        continue
    if path.suffix not in ('.py', '.md', '.json', '.jsonl', '.txt', '.log', '.html', '.cc', '.stdout', '.stderr'):
        continue
    if path.stat().st_size > 4 << 20:
        continue
    evidence.append(dict(path=str(path.relative_to(ROOT)), bytes=path.stat().st_size,
                         sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
(BASE / 'evidence-files.json').write_text(json.dumps(evidence, indent=2) + '\n')
print(len(evidence), 'bounded evidence files; databases excluded')
