"""Original EDGAR submission-header incorporation observations, never current profiles.

Prepare from archived accession lists; fetch original index-headers under the shared
SEC limiter and an additional persisted one-per-second/3000-attempt round budget;
load raw SHA-verified objects without network. Filing/header change dates and actual
acceptance govern a conservative end-of-day Eastern availability. Sparse observations
do not establish that no unobserved redomiciliation occurred.
"""
from __future__ import annotations
import argparse
from collections import Counter
import datetime as dt
import html
import json
import os
import re
import time
from pathlib import Path
from zoneinfo import ZoneInfo
from atx_db import sec_http as H
from . import common as C, sec_filings as SF, listing_events as LE

STAGE='issuer_history'
RULE='original-sec-header-incorporation-v2'
FORMS={'10-K','10-Q','8-K','20-F','40-F','6-K','S-1','F-1','S-3','F-3','424B4','N-2','N-54A','N-54C'}
ET=ZoneInfo('America/New_York')
RAW=C.PACKAGE_ROOT/'data/raw/sec_issuer_history/round2'

class RequestBudgetExhausted(BaseException):
    pass

class RoundLimiter:
    """Count every adapter attempt, including retries and redirects, before grant."""
    def __init__(self,path:Path,cap:int=3000,clock=time.time,sleep=time.sleep):
        if not 0<=cap<=3000:raise ValueError('round cap may not exceed3000')
        self.inner=H.default_sec_limiter();self.path=path;self.cap=cap;self.clock=clock;self.sleep=sleep
        self.count=0;self.last=0.0
        if path.exists():
            for line in path.read_text().splitlines():
                row=json.loads(line);self.count+=1;self.last=float(row['time'])
    def __getattr__(self,name):
        return getattr(self.inner,name)
    def acquire(self,label=None):
        if self.count>=self.cap:raise RequestBudgetExhausted('round HTTP-attempt cap exhausted')
        self.sleep(max(0.0,self.last+1.0-self.clock()))
        grant=self.inner.acquire(label)
        self.last=self.clock();self.path.parent.mkdir(parents=True,exist_ok=True)
        with self.path.open('a',encoding='utf-8') as fh:
            fh.write(json.dumps({'attempt':self.count+1,'time':self.last,'url':label})+'\n');fh.flush()
        self.count+=1
        return grant

def parse_header(raw:bytes,cik:int,accession:str,*,require_incorporation:bool=True)->dict|None:
    """Match the issuer and clock; incorporation output additionally requires known state.

    A missing incorporation literal cannot supply a state, but a corporate issuer-bound
    header can still date an independently bound primary status assertion. Conflicting
    issuer states remain ambiguous even for that clock-only use.
    """
    original=html.unescape(raw.decode('utf-8',errors='replace'))
    text=html.unescape(re.sub(r'<[^>]+>','',original))
    accepted=re.search(r'<ACCEPTANCE-DATETIME>\s*(\d{14})',original,re.I)
    if not accepted:
        accepted=re.search(r'ACCEPTANCE-DATETIME[:\s]+(\d{14})',text,re.I)
    acc=re.search(r'ACCESSION NUMBER:\s*([\d-]+)',text)
    form=re.search(r'CONFORMED SUBMISSION TYPE:\s*([^\r\n]+)',text)
    if not accepted or not acc or acc.group(1)!=accession or not form:return None
    if form.group(1).strip().upper().removesuffix('/A') not in FORMS:return None
    # Repeated company blocks may describe filing agents/owners: only exact issuer CIK.
    blocks=re.split(r'COMPANY DATA:',text,flags=re.I)[1:]
    states=set();matched=False
    for block in blocks:
        block=re.split(r'BUSINESS ADDRESS:|MAIL ADDRESS:|FORMER COMPANY:',block,flags=re.I)[0]
        key=re.search(r'CENTRAL INDEX KEY:\s*(\d+)',block,re.I)
        state=re.search(r'STATE OF INCORPORATION:\s*([A-Z0-9]{2})(?:\s|$)',block,re.I)
        if key and int(key.group(1))==cik:
            matched=True
            if state:states.add(state.group(1).upper())
    if not matched or len(states)>1:return None
    # EDGAR's incorporation jurisdiction namespace: XX explicitly means unknown.
    # SC is South Carolina here, irrespective of FINRA's exchange class SC.
    if require_incorporation and (len(states)!=1 or states=={'XX'}):return None
    try:local=dt.datetime.strptime(accepted.group(1),'%Y%m%d%H%M%S')
    except ValueError:return None
    if local.replace(tzinfo=ET,fold=0).utcoffset()!=local.replace(tzinfo=ET,fold=1).utcoffset():return None
    accepted_at=local.replace(tzinfo=ET).astimezone(dt.UTC)
    days=re.findall(r'(?:FILED AS OF DATE|DATE AS OF CHANGE):\s*(\d{8})',text,re.I)
    if not days:return None
    try:end_day=max(dt.datetime.strptime(d,'%Y%m%d').date() for d in days)
    except ValueError:return None
    eod=dt.datetime.combine(end_day,dt.time(23,59,59),ET).astimezone(dt.UTC)
    return {'cik':cik,'accession':accession,'inc_state_country':next(iter(states)) if states and states!={'XX'} else None,
            'accepted_at':accepted_at,'available_at':max(accepted_at,eod),
            'clock':'max(original_acceptance_utc,filing_and_change_date_eod_et)',
            'revision_risk':'header_state_at_submission; changed-date conservatively bounded'}

def header_failure_reason(raw:bytes,cik:int)->str:
    text=html.unescape(re.sub(r'<[^>]+>','',html.unescape(raw.decode(errors='replace'))))
    states=set();matched=False
    for block in re.split(r'COMPANY DATA:',text,flags=re.I)[1:]:
        block=re.split(r'BUSINESS ADDRESS:|MAIL ADDRESS:|FORMER COMPANY:',block,flags=re.I)[0]
        key=re.search(r'CENTRAL INDEX KEY:\s*(\d+)',block,re.I)
        if not key or int(key.group(1))!=cik:continue
        matched=True;state=re.search(r'STATE OF INCORPORATION:\s*([A-Z0-9]{2})(?:\s|$)',block,re.I)
        if state:states.add(state.group(1).upper())
    if not matched:return 'issuer_block_mismatch_or_missing'
    if len(states)>1:return 'conflicting_target_incorporation_states'
    if states=={'XX'}:return 'explicit_unknown_incorporation_XX'
    if not states:return 'missing_target_incorporation_state'
    return 'unverified_form_accession_or_clock'

def retain_plan_observations(rows, plan_path:Path):
    """Extend an acquisition plan without replacing previously selected observations.

    Source identity is immutable for an accession. Discovery priority may change,
    but the retained row (including its URLs) remains the authoritative receipt key.
    """
    previous=C.read_json(plan_path)['rows'] if plan_path.exists() else []
    result=[];seen={}
    for row in [*previous,*rows]:
        key=(int(row['cik']),row['accession'])
        if key in seen:
            old=seen[key]
            for field in ('form','filing_date','primary_document','url','header_url','primary_url'):
                if field in old and field in row and str(old[field])!=str(row[field]):
                    raise ValueError(f'conflicting retained source identity {key}: {field}')
            continue
        copy=dict(row);seen[key]=copy;result.append(copy)
    return result

def select_observations(rows:list[dict],first:dt.date)->list[dict]:
    eligible=[r for r in rows if r['form'].removesuffix('/A') in FORMS and r['filing_date']<=dt.date(2022,12,31)]
    before=[r for r in eligible if r['filing_date']<first]
    out=[]
    if before:out.append({**max(before,key=lambda r:(r['filing_date'],r['accession'])),'priority':0})
    for y in range(2018,2023):
        candidates=[r for r in eligible if r['filing_date'].year==y and r['filing_date']>=first]
        if candidates:out.append({**max(candidates,key=lambda r:(r['filing_date'],r['accession'])),'priority':y-2017})
    return out

def prepare(cohort:Path)->dict:
    h=C.read_json(cohort);ciks=sorted(set(c for v in h['cohorts'].values() for c in v['ciks']))
    con=C.connect(memory='220MB',threads=1,db_file='issuer_history_prepare.duckdb')
    con.execute('CREATE TABLE cohort(cik BIGINT)');con.executemany('INSERT INTO cohort VALUES (?)',[(c,) for c in ciks])
    first=dict(con.execute(f"SELECT u.cik,min(session_date) FROM read_parquet('{C.build_root().as_posix()}/universe/year=201[89]/*.parquet',hive_partitioning=false) u JOIN cohort USING(cik) WHERE univ_all_common GROUP BY 1").fetchall());con.close()
    candidates={c:[] for c in ciks};zip_path=C.PACKAGE_ROOT/'data/cache/submissions.zip'
    with zip_path.open('rb') as fh:
        for entry in LE.iter_entries_fast(fh):
            m=re.match(r'CIK(\d+)(?:-submissions-\d+)?\.json$',entry.name)
            if not m or int(m.group(1)) not in candidates:continue
            cik=int(m.group(1));doc=json.loads(SF.read_zip_member(fh,entry));fil=doc.get('filings',{}).get('recent',doc)
            # Accession arrays are discovery only; current profile state never read.
            for i,a in enumerate(fil.get('accessionNumber',[])):
                form=fil['form'][i];date=dt.date.fromisoformat(fil['filingDate'][i])
                if form.removesuffix('/A') in FORMS and date<=dt.date(2022,12,31):
                    candidates[cik].append({'cik':cik,'accession':a,'form':form,'filing_date':date,
                        'primary_document':fil.get('primaryDocument',['']*len(fil['accessionNumber']))[i]})
    plan=[];missing=[]
    for cik in ciks:
        selected=select_observations(candidates[cik],first[cik]);plan.extend(selected)
        if not any(r['priority']==0 for r in selected):missing.append(cik)
    plan.sort(key=lambda r:(r['priority'],r['cik'],r['filing_date'],r['accession']))
    seen=set();plan=[r for r in plan if not ((r['cik'],r['accession']) in seen or seen.add((r['cik'],r['accession'])))]
    for r in plan:r['url']=f"https://www.sec.gov/Archives/edgar/data/{r['cik']}/{r['accession'].replace('-','')}/{r['accession']}-index-headers.html"
    plan=retain_plan_observations(plan,C.stage_dir(STAGE)/'plan.json')
    result={'rule':RULE,'cohort_ciks':ciks,'first_eligibility':{str(k):str(v) for k,v in first.items()},'no_pre_eligibility_filing':missing,'rows':plan,
        'source_archive':C.file_identity(zip_path),'cohort_sha256':C.sha256_file(cohort),'selection':'last issuer filing before first eligible session; latest subsequent issuer observation per calendar year appended to all previously selected observations; priority initial then2018..2022; sparse not exhaustive'}
    C.write_json_atomic(C.stage_dir(STAGE)/'plan.json',result);print('plan',len(plan),'cohort',len(ciks),'no prefiling',len(missing));return result

def fetch(limit:int=3000)->dict:
    if not 0<=limit<=3000:raise ValueError('round cap may not exceed3000')
    plan=C.read_json(C.stage_dir(STAGE)/'plan.json');store=H.FetchLedgerStore(RAW).load();limiter=RoundLimiter(RAW/'http-attempts.jsonl',limit)
    completed=0;stopped=False
    try:
        for r in plan['rows']:
            rec,new=store.ensure(r['url'],limiter=limiter,maximum=2_000_000,timeout=30)
            completed+=int(rec.terminal)
            if completed%100==0:print('completed',completed,'HTTP attempts',limiter.count,flush=True)
    except RequestBudgetExhausted:
        stopped=True
    except BaseException as exc:
        C.write_json_atomic(C.stage_dir(STAGE)/'fetch_status.json',{
            'http_attempts':limiter.count,'cap':limit,'aborted':type(exc).__name__,
            'plan_rows':len(plan['rows']),'terminal_plan_rows':completed,
            'remaining_urls':[r['url'] for r in plan['rows'] if not (store.lookup(r['url']) and store.lookup(r['url']).terminal)]})
        raise
    result={'http_attempts':limiter.count,'cap':limit,'budget_stop':stopped,'plan_rows':len(plan['rows']),
        'terminal_plan_rows':completed,'remaining_urls':[r['url'] for r in plan['rows'] if not (store.lookup(r['url']) and store.lookup(r['url']).terminal)]}
    C.write_json_atomic(C.stage_dir(STAGE)/'fetch_status.json',result);print({k:v for k,v in result.items() if k!='remaining_urls'});return result

def load()->dict:
    import pyarrow as pa
    import pyarrow.parquet as pq
    plan=C.read_json(C.stage_dir(STAGE)/'plan.json');store=H.FetchLedgerStore(RAW).load();rows=[];bad=[]
    for r in plan['rows']:
        rec=store.lookup(r['url'])
        if not rec or not rec.ok:bad.append({'cik':r['cik'],'accession':r['accession'],'reason':'not_fetched_or_failed'});continue
        raw=store.read(rec,maximum=2_000_000);obs=parse_header(raw,r['cik'],r['accession'])
        if obs:rows.append({**obs,'source_url':r['url'],'raw_sha256':rec.sha256,'fetched_at':rec.fetched_at})
        else:bad.append({'cik':r['cik'],'accession':r['accession'],'reason':header_failure_reason(raw,r['cik'])})
    schema=pa.schema([('cik',pa.int64()),('accession',pa.string()),('inc_state_country',pa.string()),('accepted_at',pa.timestamp('us',tz='UTC')),('available_at',pa.timestamp('us',tz='UTC')),('clock',pa.string()),('revision_risk',pa.string()),('source_url',pa.string()),('raw_sha256',pa.string()),('fetched_at',pa.string())])
    dst=C.stage_dir(STAGE)/'incorporation.parquet';tmp=dst.with_suffix('.partial');pq.write_table(pa.Table.from_pylist(rows,schema=schema),tmp,compression='zstd');tmp.replace(dst)
    result={'rule':RULE,'rows':len(rows),'distinct_ciks':len({r['cik'] for r in rows}),'unresolved':bad,
        'source_plan_sha256':C.sha256_file(C.stage_dir(STAGE)/'plan.json'),'raw_ledger_sha256':C.sha256_file(RAW/H.FETCH_LEDGER_NAME),
        'source':{'url_contract':'original EDGAR accession index-headers.html','terms':'SEC public EDGAR; fair-access policy','rate':'shared SEC limiter plus <=1 attempt/s; <=3000 per round including retries/redirects'},
        'fetch_status':C.read_json(C.stage_dir(STAGE)/'fetch_status.json'),'raw_bytes':sum(rec.bytes for rec in (store.lookup(r['url']) for r in plan['rows']) if rec and rec.ok),
        'network_contract_sha256':C.sha256_file(Path(H.__file__)), 'guard_job':os.environ.get('ATX_GUARD_JOB'),
        'source_coverage':'partial bounded observations; unknown stays unknown; remaining URLs and missing/ambiguous observations explicitly retained',
        'state_counts':dict(Counter(r['inc_state_country'] for r in rows)), 'unresolved_reason_counts':dict(Counter(r['reason'] for r in bad)),
        'min_available_at':min((r['available_at'] for r in rows),default=None),'max_available_at':max((r['available_at'] for r in rows),default=None)}
    C.write_stage_manifest(STAGE,'atx.alpha-panel.issuer-history/v1',('issuer_history','common','sec_filings','listing_events'),result)
    print('loaded',len(rows),'ciks',result['distinct_ciks'],'unresolved',len(bad));return result

def main():
    p=argparse.ArgumentParser();p.add_argument('command',choices=['prepare','fetch','load']);p.add_argument('--cohort',type=Path);p.add_argument('--limit',type=int,default=3000);a=p.parse_args()
    if a.command=='prepare':prepare(a.cohort)
    elif a.command=='fetch':fetch(a.limit)
    else:load()
if __name__=='__main__':main()
