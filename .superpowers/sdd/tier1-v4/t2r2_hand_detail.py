"""Read selected original passages from persisted hand receipts; no HTTP."""
from pathlib import Path
import re
from atx_db.alpha_panel import common as C,issuer_history as I,issuer_classes as K
from atx_db import sec_http as H
art=Path('../.superpowers/sdd/tier1-v4');e=C.read_json(art/'t2r2-hand-source-evidence.json');stores=[H.FetchLedgerStore(Path(e['raw_root'])).load(),H.FetchLedgerStore(I.RAW).load()];out=[]
for r in e['rows']:
    if r['ticker'] not in ('FRES','SAFE'):continue
    for s in r['sources']:
        store=next(x for x in stores if x.lookup(s['url']) and x.lookup(s['url']).ok);text=K.plain_text(store.read(store.lookup(s['url']),maximum=K.PRIMARY_MAXIMUM))
        pattern=r'(?i)(?:foreign private issuer|fresh2 group|reit.{0,50}tax|tax.{0,70}reit|qualif.{0,70}reit)'
        passages=[text[max(0,m.start()-100):m.end()+350] for m in re.finditer(pattern,text)][:20]
        out.append({'ticker':r['ticker'],'source':s,'cover':text[:1800],'passages':passages})
C.write_json_atomic(art/'t2r2-hand-detail-evidence.json',{'rows':out,'limits':'Audit-only originals; no source events or HTTP.'})
print('persisted selected original detail rows',len(out))
