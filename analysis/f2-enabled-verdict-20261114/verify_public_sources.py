from pathlib import Path
import tarfile,hashlib,json
ROOT=Path(__file__).resolve().parents[2];BASE=Path(__file__).resolve().parent
archives=list(ROOT.glob('**/taycan_participant_sdk.tar.gz'))
archives=[p for p in archives if 'research' not in p.parts and 'build' not in p.parts]
if not archives:
 print('Original archive not found; record current source hashes only')
 report=dict(original_archive=None,source_sha256={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (ROOT/'taycan_participant_sdk/tests').glob('*') if p.is_file() and p.suffix in ('.cc','.h')})
else:
 archive=archives[0];report=dict(original_archive=str(archive),checks=[])
 with tarfile.open(archive) as tar:
  for p in (ROOT/'taycan_participant_sdk/tests').glob('*'):
   if not p.is_file() or p.suffix not in ('.cc','.h'):continue
   candidates=[x for x in tar.getmembers() if x.isfile() and x.name.endswith('/tests/'+p.name)]
   if not candidates:continue
   data=tar.extractfile(candidates[0]).read();report['checks'].append(dict(path=p.name,matches=data==p.read_bytes()))
(BASE/'public-source-integrity.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report)[:2000])
