"""Original latest-2022-periodic evidence for every positive REIT issuer, no network."""
from pathlib import Path
import re
from atx_db.alpha_panel import common as C,issuer_history as I,issuer_classes as K
from atx_db import sec_http as H
plan=C.read_json(C.stage_dir(K.STAGE)/'plan.json');store=H.FetchLedgerStore(I.RAW).load();out=[]
for r in plan['status_followup']['rows']:
    rec=store.lookup(r['primary_url']);head=store.lookup(r['header_url'])
    text=K.plain_text(store.read(rec,maximum=K.PRIMARY_MAXIMUM)) if rec and rec.ok else ''
    excerpts=[text[max(0,m.start()-200):m.end()+500] for m in re.finditer(r'(?i)(?:real estate investment trust|no longer.{0,50}reit|reit election|taxable.{0,30}corporation|revok.{0,50}reit|conversion|c corporation)',text)][:18]
    item={**r,'raw_sha256':rec.sha256 if rec else None,'header_sha256':head.sha256 if head else None,'excerpts':excerpts};out.append(item)
    print(r['cik'],r['filing_date'],len(excerpts),flush=True)
C.write_json_atomic(Path('../.superpowers/sdd/tier1-v4/t2r2-status-followup-evidence.json'),{'rule':plan['status_followup']['rule'],'rows':out,'limits':'Entire positive source cohort; original dated availability, never historical backdating. Nonasserting text remains unknown.'})
