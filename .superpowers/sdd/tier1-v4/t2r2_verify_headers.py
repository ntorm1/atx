from atx_db.alpha_panel import issuer_history as I,common as C
from atx_db import sec_http as H
plan=C.read_json(C.stage_dir(I.STAGE)/'plan.json');store=H.FetchLedgerStore(I.RAW).load()
for r in plan['rows'][:3]:
    rec=store.lookup(r['url'])
    print(r['cik'],rec.status if rec else None,I.parse_header(store.read(rec),r['cik'],r['accession']) if rec and rec.ok else None)
    if rec and rec.ok:print(store.read(rec)[:1000].decode(errors='replace'))
