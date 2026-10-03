"""Guarded source semantics of the prior ADES 2020 automatic cut."""
import gzip,hashlib,json
from pathlib import Path
from atx_db.alpha_panel import fund_notes_semantic as P
root=Path('data/raw/sec_notes_t14_pilot');adsh='0001515156-21-000010'
r=next(r for r in map(json.loads,(root/'receipts.jsonl').read_text(encoding='utf8').splitlines()) if r['adsh']==adsh and r['kind']=='primary' and r['http_status']==200)
body=gzip.decompress((root/r['path']).read_bytes());assert hashlib.sha256(body).hexdigest()==r['sha256']
claims=P.reportable_lists(P.Primary(body.decode('utf8','replace')),'2020-12-31',{'AdvancedPurificationTechnologies','RefinedCoal'})
Path('../.superpowers/sdd/tier1-v4/t14-fix-r5-ades-claims.json').write_text(json.dumps(claims,indent=2),encoding='utf8')
print(json.dumps(claims,indent=2))
