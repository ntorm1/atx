import os
from pathlib import Path
from atx_db.alpha_panel import common as C
art=Path('../.superpowers/sdd/tier1-v4');root=C.build_root();directory=root/'universe'
manifest=C.read_json(directory/'manifest.json');actual=C.output_hashes(directory)
before=C.read_json(art/'t2r2-after-verification.json')['publications']['universe']
old=before['actual_files']
changed=[{'file':name,'before':old.get(name),'current':actual.get(name)} for name in sorted(set(old)|set(actual)) if old.get(name)!=actual.get(name)]
out={'guard_job':os.environ.get('ATX_GUARD_JOB'),'manifest_sha256':C.sha256_file(directory/'manifest.json'),'prior_manifest_sha256':before['manifest_sha256'],'manifest_files':manifest['files'],'actual_files':actual,'files_match_manifest':actual==manifest['files'],'changed_from_round2':changed,'partial_files':[str(p) for p in directory.rglob('*.partial')],'input_snapshot_stale':True,'acceptance':'HOLD: interrupted universe build; gates/coverage not remeasured'}
C.write_json_atomic(art/'t2r4-interrupted-universe-snapshot.json',out)
print('snapshot',out['manifest_sha256'],'files_match',out['files_match_manifest'],'changed',len(changed),'partials',len(out['partial_files']))
