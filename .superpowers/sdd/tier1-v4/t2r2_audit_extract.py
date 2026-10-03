"""Read original receipt-backed disclosures for the fixed independent audit; no network."""
from pathlib import Path
import re
from atx_db.alpha_panel import common as C, issuer_history as I, issuer_classes as K
from atx_db import sec_http as H

art=Path('../.superpowers/sdd/tier1-v4')
sample=C.read_json(art/'t2r2-independent-audit-sample.json')
plan=C.read_json(C.stage_dir(K.STAGE)/'plan.json')['rows']
store=H.FetchLedgerStore(I.RAW).load();out=[]
seen=set()
for s in sample['samples']:
    key=(s['security_id'],s['session_date'])
    if key in seen:continue
    seen.add(key)
    candidates=[r for r in plan if r['cik']==s['cik_u'] and r['filing_date']<str(s['session_date'])[:10]]
    periodic=[r for r in candidates if r['form'] in ('10-K','10-Q','20-F','40-F')]
    r=max(periodic or candidates,key=lambda r:(r['filing_date'],r['accession'])) if candidates else None
    item={k:s[k] for k in ('ticker','security_id','session_date','cik_u','stratum','sic_asof','inc','security_type')}
    if not r:out.append({**item,'reason':'no_selected_original_before_sample'});continue
    records=[store.lookup(r[k]) for k in ('header_url','primary_url')]
    item.update(source=r,receipt_status=[{'status':v.status,'error':v.error,'sha256':v.sha256} if v else None for v in records])
    if not all(v and v.ok for v in records):out.append({**item,'reason':'original_receipt_unavailable'});continue
    head=store.read(records[0]);raw=store.read(records[1]);clock=I.parse_header(head,s['cik_u'],r['accession'])
    text=K.plain_text(raw);name=K.issuer_name(head,s['cik_u'])
    patterns={
        'cover':r'(?i)(?:securities registered pursuant|title of each class|trading symbol|common stock|ordinary shares|class [ab] common)',
        'business':r'(?i)(?:we are|the company is|our business|business overview|principal business)',
        'class_status':r'(?i)(?:real estate investment trust|business development company|investment company act|blank.check|special purpose acquisition|no longer a reit|ceased.{0,30}reit)',
        'identity':r'(?i)(?:incorporated in|incorporated under|state of incorporation|jurisdiction of incorporation|cayman|bermuda|delaware|nevada|maryland)',
    }
    excerpts={k:[text[max(0,m.start()-120):m.end()+260] for m in list(re.finditer(p,text))[:10]] for k,p in patterns.items()}
    out.append({**item,'header':clock,'issuer_name':name,'reit_assertion':K.reit_assertion(raw,name,s['cik_u']) if name else None,'excerpts':excerpts})
    print(s['ticker'],s['session_date'],name,clock['inc_state_country'] if clock else None,flush=True)
C.write_json_atomic(art/'t2r2-independent-audit-evidence.json',{'sample_sha256':C.sha256_file(art/'t2r2-independent-audit-sample.json'),'rows':out,'limits':'Original primary disclosures and bound original headers; issuer assertions audited at the sampled cutoff. No benchmark identity matching implied.'})
