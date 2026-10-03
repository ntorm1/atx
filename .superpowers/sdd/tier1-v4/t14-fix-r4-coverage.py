"""Guarded coverage remeasurement with exact before/after source identities."""
import json
from pathlib import Path
from atx_db.alpha_panel import common as C, fund_notes as F
a=Path('../.superpowers/sdd/tier1-v4')
paths={'universe_manifest':C.build_root()/'universe/manifest.json','identity_manifest':C.build_root()/'identity/manifest.json','calendar':C.calendar_path()}
def hashes():return {k:C.sha256_file(p) for k,p in paths.items() if p.exists()}
before=hashes();cov=F.remeasure();after=hashes()
evidence={'before':before,'after':after,'source_hashes_stable_during_measurement':before==after,'remeasured_at':C.read_json(C.stage_dir(F.STAGE)/'manifest.json').get('remeasured_at'),'final_task2_refresh_pending':True}
(a/'t14-fix-r4-coverage-inputs.json').write_text(json.dumps(evidence,indent=2),encoding='utf8')
print(json.dumps(evidence|{'gate_status':cov['gate_status']}))
