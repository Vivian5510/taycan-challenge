from pathlib import Path
import json
base=Path(__file__).resolve().parent
for results in base.glob('public-*/results.json'):
 print(results.parent.name)
 for row in json.loads(results.read_text()): print(row['test'],row['status'],round(row['elapsed_seconds'],2),row.get('reason',''))
