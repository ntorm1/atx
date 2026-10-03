from pathlib import Path
import re,html
from atx_db.alpha_panel import common as C,issuer_history as I
from atx_db import sec_http as H
art=Path('../.superpowers/sdd/tier1-v4');sample=C.read_json(art/'t2r2-independent-audit-sample.json');store=H.FetchLedgerStore(I.RAW).load();limiter=I.RoundLimiter(I.RAW/'http-attempts.jsonl',cap=3000);out=[]
for r in sample['samples']:
    if r['ticker'] not in ('HPP','AHH'):continue
    f=r['cached_primary'];urls=[f['url'],f"https://www.sec.gov/Archives/edgar/data/{r['cik_u']}/{f['accession'].replace('-','')}/{f['accession']}-index-headers.html"]
    records=[store.ensure(url,limiter=limiter,maximum=10_000_000)[0] for url in urls]
    text=html.unescape(re.sub('<[^>]+>',' ',store.read(records[0]).decode(errors='replace')));text=re.sub(r'\s+',' ',text)
    excerpts=[text[max(0,m.start()-180):m.end()+200] for m in re.finditer(r'real estate investment trust',text,re.I)][:12]
    row={'ticker':r['ticker'],'cik':r['cik_u'],'session_date':r['session_date'],'primary_url':urls[0],'primary_sha256':records[0].sha256,'header_url':urls[1],'header_sha256':records[1].sha256,'header':I.parse_header(store.read(records[1]),r['cik_u'],f['accession']),'excerpts':excerpts};out.append(row);print(row)
C.write_json_atomic(art/'t2r2-reit-confirmation.json',out);print('total round HTTP attempts',limiter.count)
