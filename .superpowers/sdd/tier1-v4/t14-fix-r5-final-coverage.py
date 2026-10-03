"""Run only after controller final universe producer/integrity release (R9)."""
import json
from pathlib import Path
from atx_db.alpha_panel import common as C,fund_notes as F
a=Path('../.superpowers/sdd/tier1-v4');root=C.build_root();universe=root/'universe';path=universe/'manifest.json'
before_manifest=C.sha256_file(path);manifest=C.read_json(path)
assert before_manifest=='90e9ca70cec64838a96b5f7ffc0de88a28e1faf529b48b480c56f4b9d85d0c0e','controller released a different final universe manifest'
required={f'year={y}/universe.parquet' for y in range(2020,2026)}
assert required<=set(manifest['files']),sorted(required-set(manifest['files']))
checks={name:{'actual_sha256':C.sha256_file(universe/name),'manifest_sha256':entry['sha256'],'actual_bytes':(universe/name).stat().st_size,'manifest_bytes':entry['bytes']} for name,entry in sorted(manifest['files'].items())}
assert all(r['actual_sha256']==r['manifest_sha256'] and r['actual_bytes']==r['manifest_bytes'] for r in checks.values())
cov=F.remeasure()
after_manifest=C.sha256_file(path)
after={name:C.sha256_file(universe/name) for name in sorted(checks)}
assert before_manifest==after_manifest and all(after[name]==r['actual_sha256'] for name,r in checks.items())
result={'controller_final_release_required':True,'universe_manifest_sha256':before_manifest,'universe_required_outputs':checks,'source_hashes_stable_during_measurement':True,'remeasured_at':C.read_json(C.stage_dir(F.STAGE)/'manifest.json')['remeasured_at'],'gate_status':cov['gate_status'],'segments':cov['segments'],'cells':cov['cells'],'gc3_final_pending':False}
(a/'t14-fix-r5-final-coverage.json').write_text(json.dumps(result,indent=2),encoding='utf8')
print(json.dumps({k:v for k,v in result.items() if k not in ('cells','segments')}))
