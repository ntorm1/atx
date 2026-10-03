import json
from pathlib import Path
from atx_db.alpha_panel import common as C,ratios as R
EXPECTED='90e9ca70cec64838a96b5f7ffc0de88a28e1faf529b48b480c56f4b9d85d0c0e'
root=C.build_root();manifest=root/'universe'/'manifest.json';source=root/'compustat_std'/'manifest.json';source_before=C.sha256_file(source)
def verify():
 sha=C.sha256_file(manifest);assert sha==EXPECTED,(sha,EXPECTED)
 m=C.read_json(manifest);files=[]
 for name,meta in m['files'].items():
  p=root/'universe'/name;size=p.stat().st_size;digest=C.sha256_file(p)
  assert size==meta['bytes'] and digest==meta['sha256'],name
  files.append({'path':str(p),'bytes':size,'sha256':digest})
 assert len(files)==9,len(files)
 return {'manifest_sha256':sha,'files':files}
before=verify();v=R.validate(checks=('coverage',));after=verify();assert before==after;assert source_before==C.sha256_file(source)
out={'source_before_after_sha256':source_before,'universe_before':before,'universe_after':after,'output_integrity_stable':True,'coverage':{k:x for k,x in v.items() if k.startswith('coverage')},'scope':'One final coverage refresh after R9 release; Task2 French10/60FAIL remains, stable snapshot does not imply universe parity'}
Path('../.superpowers/sdd/tier1-v4/t4-r2-final-coverage.json').write_text(json.dumps(out,indent=2,default=str));print(json.dumps({k:x['by_role'] for k,x in out['coverage'].items()}))
