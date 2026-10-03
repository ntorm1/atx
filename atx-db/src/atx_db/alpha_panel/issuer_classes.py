"""Issuer-bound original annual-filing REIT status, with dated positive/termination events.

Financial SIC is a broad source-inspection cohort, never a classification rule.
Only direct issuer current-status/tax-election assertions count; subsidiary, prospective,
ambiguous and nonasserting passages remain unknown. Raw original filing and matching
issuer header are required. Missing observations are not evidence of non-REIT status.
"""
from __future__ import annotations
import argparse,html,json,re,os
import datetime as dt
from pathlib import Path
from . import common as C,issuer_history as I,issuer_class_replay as R,sec_filings as SF,listing_events as LE
from atx_db import sec_http as H
STAGE='issuer_classes';RULE='issuer-bound-original-reit-status-v3'
PRIMARY_MAXIMUM=64_000_000

def report_period(header:bytes)->dt.date|None:
    text=html.unescape(re.sub(r'<[^>]+>','',html.unescape(header.decode(errors='replace'))))
    match=re.search(r'CONFORMED PERIOD OF REPORT:\s*(\d{8})',text,re.I)
    if not match:return None
    try:return dt.datetime.strptime(match.group(1),'%Y%m%d').date()
    except ValueError:return None

def plain_text(raw:bytes)->str:
    text=raw.decode('utf-8',errors='replace')
    text=re.sub(r'<(?:script|style)\b[^>]*>.*?</(?:script|style)>',' ',text,flags=re.I|re.S)
    return re.sub(r'\s+',' ',html.unescape(re.sub(r'<[^>]+>',' ',text))).strip()

def issuer_name(header:bytes,cik:int)->str|None:
    text=html.unescape(re.sub(r'<[^>]+>','',html.unescape(header.decode(errors='replace'))))
    names=set()
    for block in re.split(r'COMPANY DATA:',text,flags=re.I)[1:]:
        key=re.search(r'CENTRAL INDEX KEY:\s*(\d+)',block,re.I);name=re.search(r'COMPANY CONFORMED NAME:\s*([^\r\n]+)',block,re.I)
        if key and name and int(key.group(1))==cik:names.add(name.group(1).strip())
    return next(iter(names)) if len(names)==1 else None

def reit_assertion(raw:bytes,name:str,cik:int)->dict:
    text=plain_text(raw);name=re.sub(r'\s*/[A-Z]{2}/?\s*$','',name,flags=re.I)
    norm=lambda s:re.sub(r'[^a-z0-9]+',' ',s.lower()).strip()
    normalized=norm(text);needle=norm(name)
    # Generic pronouns are accepted only if this document binds the target CIK and
    # has a single registrant CIK, or explicitly defines the named issuer as Company.
    cik_values=set(int(v) for v in re.findall(r'name=["\']dei:EntityCentralIndexKey["\'][^>]*>\s*0*(\d+)\s*<',raw.decode(errors='replace'),re.I))
    definition=bool(re.search(re.escape(needle)+r'\s+(?:the\s+)?company\b',normalized))
    generic=(cik_values=={cik}) or definition
    subjects=[re.escape(needle)] if needle and needle in normalized else []
    if generic:subjects.extend(['we','the company'])
    if not subjects:return {'status':None,'reason':'issuer_not_bound','evidence':None}
    subject=r'\b(?:'+ '|'.join(subjects)+r')\b'
    positives=[];negatives=[]
    negative=subject+r'\s+(?:is|are)\s+no longer\s+(?:(?:taxed|treated|qualified)\s+as\s+)?(?:a\s+)?(?:reit|real estate investment trust)\b'
    ended_rules=subject+r'\s+(?:is|are)\s+no longer required to operate under reit rules\b'
    # Match offsets below refer to this punctuation-preserving normalized copy.
    # A condition can qualify either side of the assertion and be arbitrarily far
    # from its subject within the same sentence. Never truncate its context.
    contextual=re.sub(r'[^a-z0-9.!?;]+',' ',text.lower()).strip()
    contextual=re.sub(r'\b(?:[a-z]\.){2,}',lambda m:m.group(0).replace('.',' '),contextual)
    # Common issuer abbreviations are not sentence boundaries.
    contextual=re.sub(r'\b(inc|corp|co|ltd|u|s)\.(?= |$)',r'\1 ',contextual)
    contextual=re.sub(r' +',' ',contextual)
    normalized=contextual
    def conditional(m):
        left=max(normalized.rfind(mark,0,m.start()) for mark in '.!?')+1
        ends=[normalized.find(mark,m.end()) for mark in '.!?']
        right=min((end for end in ends if end>=0),default=len(normalized))
        context=normalized[left:right]
        if re.search(r'\b(?:if|unless|whether|assuming|when|once|until|upon|as long as|provided that|subject to|pending|proposed|contingent)\b',context):return True
        # A current completed election is not made prospective by a separate
        # intention to maintain/continue that already asserted status. Conditions
        # above still apply across the entire sentence, including that clause.
        current=re.split(r'\band\s+(?:we\s+)?(?:intend\w*|expect\w*)\s+to\s+(?:maintain|continue)\b',context,maxsplit=1)[0]
        return bool(re.search(r'\b(?:would|will|intend\w*|expect\w*)\b',current))
    for pattern in (negative,ended_rules):
        for m in re.finditer(pattern,normalized):
            if not conditional(m):negatives.append(m.group(0))
    qualifier=r'(?:a|an|and|publicly traded|fully integrated|self administered|self managed|internally managed|externally managed|equity|mortgage|maryland|delaware|u s|domestic|residential|commercial|industrial|diversified|specialty|healthcare|retail|office|hotel|lodging|multifamily|corporation|organized as)'
    patterns=[subject+r'\s+(?:is|are|operates as|operate as)\s+(?:'+qualifier+r'\s+){0,10}(?:real estate investment trust|reit)\b',
              subject+r'\s+(?:have|has)\s+elected\s+(?:to be taxed|to qualify|to be treated)\s+as\s+(?:a\s+)?(?:real estate investment trust|reit)\b']
    for pattern in patterns:
        for m in re.finditer(pattern,normalized):
            evidence=m.group(0)
            if conditional(m):continue
            if re.search(r'\b(?:have|has) elected\b',evidence) and re.search(r'\b(?:commencing|beginning|effective|will)\b',normalized[m.end():m.end()+180]):continue
            if re.search(r'\b(?:not|no longer|subsidiar\w*|will|would|intend\w*|expect\w*|holding company|invests|investing|if)\b',evidence):continue
            positives.append(evidence)
    if negatives and positives:return {'status':None,'reason':'contradictory_status_needs_manual_review','evidence':negatives+positives}
    if negatives:return {'status':False,'reason':'explicit_current_termination','evidence':negatives}
    if positives:return {'status':True,'reason':'explicit_issuer_status_or_election','evidence':positives}
    return {'status':None,'reason':'no_unambiguous_issuer_status_assertion','evidence':None}

def audit_observations(rows,cut):
    """Retain annual context and the newest dated periodic disclosure for status changes."""
    available=[r for r in rows if r['filing_date']<cut]
    annual=[r for r in available if r['form'] in ('10-K','20-F','40-F')]
    periodic=[r for r in available if r['form'] in ('10-K','10-Q','20-F','40-F')]
    selected=[]
    for eligible,priority in ((annual or available,-1),(periodic,-2)):
        if eligible:
            chosen={**max(eligible,key=lambda r:(r['filing_date'],r['accession'])),'priority':priority}
            if not any(r['accession']==chosen['accession'] for r in selected):selected.append(chosen)
    return selected

def prepare(cohort_path:Path,audit_path:Path,status_followup:bool=False)->dict:
    cohort=C.read_json(cohort_path);audit=C.read_json(audit_path)
    wanted={r['cik']:r for r in cohort['candidates']};wanted.update({r['cik_u']:{'cik':r['cik_u'],'first_candidate':r['session_date'],'sic_values':[]} for r in audit['samples'] if r['cik_u'] not in wanted})
    rows={c:[] for c in wanted};archive=C.PACKAGE_ROOT/'data/cache/submissions.zip'
    with archive.open('rb') as fh:
        for entry in LE.iter_entries_fast(fh):
            m=re.match(r'CIK(\d+)(?:-submissions-\d+)?\.json$',entry.name)
            if not m or int(m.group(1)) not in wanted:continue
            c=int(m.group(1));d=json.loads(SF.read_zip_member(fh,entry));f=d.get('filings',{}).get('recent',d)
            for i,a in enumerate(f.get('accessionNumber',[])):
                if f['form'][i] not in ('10-K','20-F','40-F','10-Q','S-1','F-1','424B4','8-K'):continue
                day=dt.date.fromisoformat(f['filingDate'][i])
                if day<=dt.date(2022,12,31):rows[c].append({'cik':c,'accession':a,'form':f['form'][i],'filing_date':day,'primary_document':f.get('primaryDocument',['']*len(f['accessionNumber']))[i]})
    plan=[];missing=[]
    for c,candidate in wanted.items():
        first=dt.date.fromisoformat(str(candidate['first_candidate'])[:10]);realestate=any(6500<=s<=6599 for s in candidate['sic_values'])
        boundaries=[(dt.date(2021,12,31),0),(dt.date(2022,12,31),1)]
        if realestate:boundaries+=[(first-dt.timedelta(days=1),2)]+[(dt.date(y,12,31),3+y-2018) for y in range(2018,2021)]
        for end,priority in boundaries:
            eligible=[r for r in rows[c] if r['filing_date']<=end and r['form'] in ('10-K','20-F','40-F')]
            if eligible:plan.append({**max(eligible,key=lambda r:(r['filing_date'],r['accession'])),'priority':priority})
            else:missing.append({'cik':c,'boundary':str(end)})
    for sample in audit['samples']:
        c=sample['cik_u'];cut=dt.date.fromisoformat(str(sample['session_date'])[:10])
        selected=audit_observations(rows[c],cut)
        if selected:plan.extend(selected)
        else:missing.append({'cik':c,'boundary':str(cut),'reason':'no_annual_before_audit_date'})
    followup=[]
    if status_followup:
        assessment_path=C.stage_dir(STAGE)/'assessment.json'
        known={r['cik'] for r in C.read_json(assessment_path)['rows'] if r['status'] is True}
        for c in sorted(known):
            periodic=[r for r in rows[c] if r['form'] in ('10-K','10-Q','20-F','40-F')]
            if periodic:
                chosen={**max(periodic,key=lambda r:(r['filing_date'],r['accession'])),'priority':-3}
                plan.append(chosen);followup.append(chosen)
    plan.sort(key=lambda r:(r['priority'],r['cik'],r['filing_date']));seen=set();plan=[r for r in plan if not ((r['cik'],r['accession']) in seen or seen.add((r['cik'],r['accession'])))]
    for r in plan:
        base=f"https://www.sec.gov/Archives/edgar/data/{r['cik']}/{r['accession'].replace('-','')}/"
        r['header_url']=base+r['accession']+'-index-headers.html';r['primary_url']=base+r['primary_document']
    plan=I.retain_plan_observations(plan,C.stage_dir(STAGE)/'plan.json')
    result={'rule':RULE,'rows':plan,'missing_annual_candidates':missing,'candidate_cohort_sha256':C.sha256_file(cohort_path),'audit_sample_sha256':C.sha256_file(audit_path),'candidates':cohort['candidates'],
        'selection':'all285 financial candidates annual2021/2022;41 real-estate candidates also last annual pre-eligibility and2018-2020 annual; deterministic audit annual context and latest periodic before sampled date; retain all previously selected observations on extension; observations never backdated; sparse years do not prove unchanged status',
        'status_followup':{'rule':'latest available 2022-or-earlier periodic filing for every issuer with a positive original REIT assertion; checks subsequent status uniformly, no ticker patches','rows':followup}}
    C.write_json_atomic(C.stage_dir(STAGE)/'plan.json',result);print('class source plan',len(plan),'header+primary maximum',2*len(plan),'missing',len(missing));return result

def fetch_source(store,url,limiter,retried,retry_path):
    """Reuse receipts; permit one recorded larger-bound retry of an oversize receipt."""
    old=store.lookup(url)
    if old and old.error==H.RESPONSE_TOO_LARGE and url not in retried:
        rec=store.fetch(url,limiter=limiter,maximum=PRIMARY_MAXIMUM)
        with retry_path.open('a',encoding='utf-8') as fh:
            fh.write(json.dumps({'url':url,'maximum':PRIMARY_MAXIMUM,'status':rec.status,'error':rec.error,'sha256':rec.sha256})+'\n')
        retried.add(url)
        return rec
    return store.ensure(url,limiter=limiter,maximum=PRIMARY_MAXIMUM)[0]

def fetch(cap:int=3000)->dict:
    plan=C.read_json(C.stage_dir(STAGE)/'plan.json');store=H.FetchLedgerStore(I.RAW).load();limiter=I.RoundLimiter(I.RAW/'http-attempts.jsonl',cap=cap);stopped=False
    retry_path=I.RAW/'larger-payload-retries.jsonl'
    retried={json.loads(line)['url'] for line in retry_path.read_text().splitlines()} if retry_path.exists() else set()
    try:
        for n,r in enumerate(plan['rows']):
            for url in (r['header_url'],r['primary_url']):
                fetch_source(store,url,limiter,retried,retry_path)
            if (n+1)%25==0:print('class observations',n+1,'total round attempts',limiter.count,flush=True)
    except I.RequestBudgetExhausted:stopped=True
    except BaseException as exc:
        remaining=[{'cik':r['cik'],'accession':r['accession']} for r in plan['rows'] if any(not (store.lookup(url) and store.lookup(url).terminal) for url in (r['header_url'],r['primary_url']))]
        C.write_json_atomic(C.stage_dir(STAGE)/'fetch_status.json',{'http_attempts_total_round':limiter.count,'cap':cap,'aborted':type(exc).__name__,'remaining_observations':remaining})
        raise
    remaining=[{'cik':r['cik'],'accession':r['accession']} for r in plan['rows'] if any(not (store.lookup(url) and store.lookup(url).terminal) for url in (r['header_url'],r['primary_url']))]
    result={'http_attempts_total_round':limiter.count,'cap':cap,'budget_stop':stopped,'remaining_observations':remaining}
    C.write_json_atomic(C.stage_dir(STAGE)/'fetch_status.json',result);print({k:v for k,v in result.items() if k!='remaining_observations'});return result

def load(checkpoint_dir:Path|None=None)->dict:
    import pyarrow as pa
    import pyarrow.parquet as pq
    plan=C.read_json(C.stage_dir(STAGE)/'plan.json');store=H.FetchLedgerStore(I.RAW).load();events=[];inc=[];assessment=[]
    keys=[(r['cik'],r['accession']) for r in plan['rows']]
    if len(set(keys))!=len(keys):raise ValueError('duplicate issuer-class source plan key')
    plan_sha=C.sha256_file(C.stage_dir(STAGE)/'plan.json')
    replay=R.AssertionReplay(checkpoint_dir,R.code_identity(Path(__file__),RULE,PRIMARY_MAXIMUM)) if checkpoint_dir is not None else None
    ledger_sha=C.sha256_file(I.RAW/H.FETCH_LEDGER_NAME) if replay is not None else None
    for r in plan['rows']:
        head=store.lookup(r['header_url']);primary=store.lookup(r['primary_url'])
        item={'cik':r['cik'],'accession':r['accession'],'primary_url':r['primary_url'],'header_url':r['header_url'],'filing_date':r['filing_date']}
        if not head or not primary or not head.ok or not primary.ok:assessment.append({**item,'status':None,'reason':'source_not_fetched_or_failed'});continue
        try:
            rawhead=store.read(head,maximum=2_000_000)
            clock=I.parse_header(rawhead,r['cik'],r['accession'],require_incorporation=False);name=issuer_name(rawhead,r['cik'])
        except (ValueError,OSError) as exc:
            assessment.append({**item,'status':None,'reason':'header_read_or_parse_failure','error':type(exc).__name__});continue
        if not clock or not name:assessment.append({**item,'status':None,'reason':'header_identity_state_or_clock_unverified'});continue
        if clock['inc_state_country'] is not None:
            inc.append({**clock,'source_url':r['header_url'],'raw_sha256':head.sha256,'fetched_at':head.fetched_at})
        try:
            process=lambda:reit_assertion(store.read(primary,maximum=PRIMARY_MAXIMUM),name,r['cik'])
            assertion=replay.assertion(r,store,name,process) if replay is not None else process()
        except (ValueError,OSError) as exc:
            assessment.append({**item,'status':None,'reason':'primary_read_or_parse_failure','error':type(exc).__name__});continue
        period=report_period(rawhead)
        item={**item,**assertion,'issuer_name':name,'report_period_end':period,'available_at':clock['available_at'],'accepted_at':clock['accepted_at'],'primary_sha256':primary.sha256,'header_sha256':head.sha256};assessment.append(item)
        if assertion['status'] is not None:events.append({'cik':r['cik'],'accession':r['accession'],'accepted_at':clock['accepted_at'],'available_at':clock['available_at'],'report_period_end':period,'reit_status':assertion['status'],'primary_url':r['primary_url'],'primary_sha256':primary.sha256,'header_sha256':head.sha256,'evidence':json.dumps(assertion['evidence'])})
    if len(assessment)!=len(keys) or {(r['cik'],r['accession']) for r in assessment}!=set(keys):raise RuntimeError('incomplete issuer-class source assessment')
    if C.sha256_file(C.stage_dir(STAGE)/'plan.json')!=plan_sha:raise RuntimeError('source plan changed during assessment')
    replay_receipt=replay.finish(R.code_identity(Path(__file__),RULE,PRIMARY_MAXIMUM)) if replay is not None else None
    if replay is not None and C.sha256_file(I.RAW/H.FETCH_LEDGER_NAME)!=ledger_sha:raise RuntimeError('source receipts changed during replay')
    schema=pa.schema([('cik',pa.int64()),('accession',pa.string()),('accepted_at',pa.timestamp('us',tz='UTC')),('available_at',pa.timestamp('us',tz='UTC')),('report_period_end',pa.date32()),('reit_status',pa.bool_()),('primary_url',pa.string()),('primary_sha256',pa.string()),('header_sha256',pa.string()),('evidence',pa.string())])
    target=C.stage_dir(STAGE)/'reit_status.parquet';tmp=target.with_suffix('.partial');pq.write_table(pa.Table.from_pylist(events,schema=schema),tmp,compression='zstd');tmp.replace(target)
    ischema=pa.schema([('cik',pa.int64()),('accession',pa.string()),('inc_state_country',pa.string()),('accepted_at',pa.timestamp('us',tz='UTC')),('available_at',pa.timestamp('us',tz='UTC')),('clock',pa.string()),('revision_risk',pa.string()),('source_url',pa.string()),('raw_sha256',pa.string()),('fetched_at',pa.string())])
    target=C.stage_dir(STAGE)/'incorporation.parquet';tmp=target.with_suffix('.partial');pq.write_table(pa.Table.from_pylist(inc,schema=ischema),tmp,compression='zstd');tmp.replace(target)
    C.write_json_atomic(C.stage_dir(STAGE)/'assessment.json',{'rule':RULE,'rows':assessment})
    result={'rule':RULE,'observations':len(assessment),'explicit_status_events':len(events),'explicit_status_ciks':len({r['cik'] for r in events}),'verified_header_inc_rows':len(inc),'unknown_observations':sum(r['status'] is None for r in assessment),'plan_sha256':C.sha256_file(C.stage_dir(STAGE)/'plan.json'),'assessment_sha256':C.sha256_file(C.stage_dir(STAGE)/'assessment.json'),'raw_ledger_sha256':C.sha256_file(I.RAW/H.FETCH_LEDGER_NAME),'source_coverage':'sparse bounded primary observations; unknown is not false; source cap and remaining listed','fetch_status':C.read_json(C.stage_dir(STAGE)/'fetch_status.json'),
        'network_contract_sha256':C.sha256_file(Path(H.__file__)),'guard_job':os.environ.get('ATX_GUARD_JOB'),'replay_checkpoint':replay_receipt,
        'source':{'terms':'SEC public EDGAR; fair-access policy','rate':'shared SEC limiter plus<=1 attempt/s,<=3000 combined round attempts','cadence':'issuer periodic filings; historical event observations'},
        'min_available_at':min((r['available_at'] for r in events),default=None),'max_available_at':max((r['available_at'] for r in events),default=None)}
    C.write_stage_manifest(STAGE,'atx.alpha-panel.issuer-classes/v1',('issuer_classes','issuer_history','issuer_class_replay','common','sec_filings','listing_events'),result);print(result);return result

def main():
    p=argparse.ArgumentParser();p.add_argument('command',choices=['prepare','fetch','load']);p.add_argument('--cohort',type=Path);p.add_argument('--audit',type=Path);p.add_argument('--cap',type=int,default=3000);p.add_argument('--status-followup',action='store_true');p.add_argument('--checkpoint-dir',type=Path);a=p.parse_args()
    if a.command=='prepare':prepare(a.cohort,a.audit,a.status_followup)
    elif a.command=='fetch':fetch(a.cap)
    else:load(a.checkpoint_dir)
if __name__=='__main__':main()
