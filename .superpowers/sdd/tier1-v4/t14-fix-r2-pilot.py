"""Authorized deterministic <=100-filing SEC pilot; <=600 attempts, <=1 attempt/s, <=750 MiB raw response bytes."""
import datetime as dt
import gzip
import hashlib
import html
import json
import re
import shutil
import time
from collections import Counter,defaultdict
from pathlib import Path

from atx_db import sec_http
from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import filing_text as FT

artifact=Path('../.superpowers/sdd/tier1-v4')
rawroot=Path('data/raw/sec_notes_t14_pilot')
rawroot.mkdir(parents=True,exist_ok=True)
receipt_path=rawroot/'receipts.jsonl'
receipts=[json.loads(s) for s in receipt_path.read_text(encoding='utf-8').splitlines()] if receipt_path.exists() else []
landed=sum(r.get('bytes',0) for r in receipts if r.get('http_status')==200)
prior_requests=sum(r.get('requests',0) for r in receipts)
budget=600-prior_requests
class PilotLimiter(FT.CountingLimiter):
    def __init__(self):
        super().__init__(sec_http.default_sec_limiter(),budget)
        self.last=0.
    def acquire(self,label=None):
        time.sleep(max(0.,1.-(time.monotonic()-self.last)))
        value=super().acquire(label)
        self.last=time.monotonic()
        return value
limiter=PilotLimiter()
url_cache={r['url']:r for r in receipts if r.get('http_status')==200}
def fetch(url,kind,accession,maximum):
    global landed
    if url in url_cache:
        with gzip.open(rawroot/url_cache[url]['path'],'rb') as f:
            return f.read()
    maximum=min(maximum,750*1024*1024-landed)
    if maximum<=0:
        raise RuntimeError('750 MiB pilot response budget exhausted')
    status,payload,error,nrequests,fetched=FT.http_get(url,limiter,maximum=maximum,timeout=60.)
    digest=hashlib.sha256(payload).hexdigest() if payload else None
    relative=f'{accession}/{kind}-{digest}.gz' if payload else None
    if payload:
        target=rawroot/relative
        target.parent.mkdir(parents=True,exist_ok=True)
        temp=target.with_suffix('.partial')
        with gzip.open(temp,'wb') as stream:
            stream.write(payload)
        temp.replace(target)
        landed+=len(payload)
    row={'url':url,'bytes':len(payload),'sha256':digest,'http_status':status,'fetched_at':fetched,
         'requests':nrequests,'error':error,'kind':kind,'adsh':accession,'path':relative}
    with receipt_path.open('a',encoding='utf-8') as f:
        f.write(json.dumps(row)+'\n')
    receipts.append(row)
    if status==200 and payload:
        url_cache[url]=row
    return payload if status==200 else None

cases=[json.loads(s) for s in (artifact/'t14-fix-r2-causal-cases.jsonl').read_text(encoding='utf-8').splitlines()]
inventory={r['adsh']:r for r in map(json.loads,(artifact/'t14-fix-r2-acquisition-requests.jsonl').read_text(encoding='utf-8').splitlines())}
con=C.connect(memory='150MB',threads=1)
sub=(C.build_root()/'notes/parts/source=*/sub.parquet').as_posix()
con.register('case_keys',__import__('pyarrow').table({'adsh':[c['adsh'] for c in cases]}))
industry={adsh:(sic,name) for adsh,sic,name in con.execute(f"SELECT s.adsh,min(sic),min(name) FROM read_parquet('{sub}',hive_partitioning=false) s JOIN case_keys k USING(adsh) GROUP BY s.adsh").fetchall()}
con.close()
rank=lambda c:hashlib.sha256(('t14-r2-pilot-1:'+c['adsh']).encode()).hexdigest()
chosen=sorted([c for c in cases if c['cause'].startswith('local_')],key=rank)
used={c['adsh'] for c in chosen}
strata=defaultdict(list)
for case in sorted(cases,key=rank):
    if case['adsh'] not in used:
        sic=industry.get(case['adsh'],(None,None))[0]
        strata[(case['status'],int(sic or 0)//100)].append(case)
while len(chosen)<100 and any(strata.values()):
    for key in sorted(strata):
        if strata[key] and len(chosen)<100:
            chosen.append(strata[key].pop(0))
pilot=[]
for c in chosen:
    sic,name=industry.get(c['adsh'],(None,None))
    pilot.append({**c,'sic':sic,'issuer':name,'request':inventory[c['adsh']]})
(artifact/'t14-fix-r2-pilot-selection.json').write_text(json.dumps(pilot,indent=2),encoding='utf-8')
print(json.dumps({'selected':len(pilot),'prior_requests':prior_requests,'remaining_attempt_budget':budget,
                 'disk_free_bytes':shutil.disk_usage('C:/').free}),flush=True)
snippet_rows=[]
section_paths=[p.as_posix() for p in Path('data/raw/sec_text/sections').glob('part-*.parquet')]
cached={}
cache_errors=[]
if section_paths:
    con=C.connect(memory='150MB',threads=1)
    con.register('pilot_keys',__import__('pyarrow').table({'accession':[c['adsh'] for c in chosen]}))
    for section_path in section_paths:
        try:
            rows=con.execute(f"""SELECT s.accession,substring(s.text,1,200000) FROM read_parquet('{section_path}') s
              JOIN pilot_keys p ON replace(s.accession,'-','')=replace(p.accession,'-','') WHERE section='business'""").fetchall()
        except Exception as exc:
            cache_errors.append({'path':section_path,'error':str(exc)})
            continue
        for accession,text in rows:
            cached[accession]=text
    con.close()
def snippets(text):
    matches=list(re.finditer(r'(?i)reportable segments|operating segments|segment (?:information|reporting)|consolidated revenues|net revenues|revenues net of',text))
    return [text[max(0,m.start()-250):m.start()+1800] for m in matches[-12:]]
for seq,case in enumerate(pilot,1):
    accession=case['adsh']
    cache_text=cached.get(accession,'')
    primary=fetch(case['request']['document_url'],'primary',accession,50*1024*1024)
    row={'adsh':accession,'sic':case['sic'],'issuer':case['issuer'],'status':case['status'],'cause':case['cause'],
         'document_url':case['request']['document_url'],'cached_business_snippets':snippets(cache_text),'primary_fetched':bool(primary)}
    if primary:
        # Preserve numeric tables for disclosure audit (the production text extractor deliberately removes them).
        body=re.sub(rb'<(?:script|style|ix:header)\b[^>]*>.*?</(?:script|style|ix:header)>',b' ',primary,flags=re.I|re.S)
        text=html.unescape(re.sub(rb'<[^>]+>',b' ',body).decode('utf-8','replace'))
        text=re.sub(r'\s+',' ',text)
        row['primary_snippets']=snippets(text)
    index=fetch(case['request']['index_url'],'index',accession,5*1024*1024)
    if index:
        entries=json.loads(index).get('directory',{}).get('item',[])
        names=[]
        for suffix in ('_def.xml','_pre.xml'):
            names+=sorted(e['name'] for e in entries if e['name'].lower().endswith(suffix))[:1]
        row['linkbases']=[]
        for name in names:
            url=case['request']['index_url'].rsplit('/',1)[0]+'/'+name
            blob=fetch(url,'linkbase',accession,10*1024*1024)
            row['linkbases'].append({'url':url,'bytes':len(blob) if blob else 0})
    snippet_rows.append(row)
    (artifact/'t14-fix-r2-pilot-snippets.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in snippet_rows),encoding='utf-8')
    print(json.dumps({'filing':seq,'adsh':accession,'attempts':prior_requests+limiter.count,'response_bytes':landed}),flush=True)
summary={'selected':len(pilot),'processed':len(snippet_rows),'successful_primary':sum(r['primary_fetched'] for r in snippet_rows),
 'cached_business_available':len(cached),'cache_errors':cache_errors,'requests_total':prior_requests+limiter.count,'response_bytes':landed,
 'disk_used_bytes':sum(p.stat().st_size for p in rawroot.rglob('*') if p.is_file()),
 'classes':dict(Counter(c['status'] for c in pilot)),'sic_two_digit':dict(Counter(str(int(c['sic'] or 0)//100) for c in pilot)),
 'receipt_path':str(receipt_path),'completed_at':dt.datetime.now(dt.UTC).isoformat(),'rate_per_second':1,'attempt_budget':600}
(artifact/'t14-fix-r2-pilot-summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
print(json.dumps(summary),flush=True)
