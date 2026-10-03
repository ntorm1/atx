"""Receipt-backed originals for rechecking every affected hand-check stratum."""
from pathlib import Path
import datetime as dt,json,re
from atx_db.alpha_panel import common as C,issuer_history as I,issuer_classes as K,sec_filings as SF,listing_events as LE
from atx_db import sec_http as H
art=Path('../.superpowers/sdd/tier1-v4');sample=C.read_json(C.stage_dir('validation')/'universe_hand_check_sample.json')
wanted=[r for r in sample['rows'] if r['stratum'] in ('univ_all_common','is_reit','is_foreign_issuer')]
filings={r['cik']:[] for r in wanted if r['cik'] is not None}
with (C.PACKAGE_ROOT/'data/cache/submissions.zip').open('rb') as fh:
    for entry in LE.iter_entries_fast(fh):
        m=re.match(r'CIK(\d+)(?:-submissions-\d+)?\.json$',entry.name)
        if not m or int(m.group(1)) not in filings:continue
        c=int(m.group(1));d=json.loads(SF.read_zip_member(fh,entry));f=d.get('filings',{}).get('recent',d)
        for n,acc in enumerate(f.get('accessionNumber',[])):
            if f['form'][n].removesuffix('/A') not in ('10-K','10-Q','20-F','40-F','6-K','S-1','F-1','424B4'):continue
            if f['filingDate'][n]<'2024-06-28':filings[c].append({'cik':c,'accession':acc,'form':f['form'][n],'filing_date':f['filingDate'][n],'primary_document':f.get('primaryDocument',['']*len(f['accessionNumber']))[n]})
hand_raw=C.PACKAGE_ROOT/'data/raw/sec_universe_hand_check/round2';store=H.FetchLedgerStore(hand_raw).load();source_store=H.FetchLedgerStore(I.RAW).load();limiter=I.RoundLimiter(I.RAW/'http-attempts.jsonl',cap=3000)
out=[];stopped=False
try:
    for s in wanted:
        choices=filings.get(s['cik'],[]);periodic=[r for r in choices if r['form'] in ('10-K','10-Q','20-F','40-F','6-K')]
        annual=[r for r in choices if r['form'] in ('10-K','20-F','40-F')]
        selected=[]
        first=(annual or periodic or choices) if s['stratum']=='is_foreign_issuer' else (periodic or choices)
        for options in (first,annual if s['stratum']=='is_reit' else []):
            if options:
                r=max(options,key=lambda r:(r['filing_date'],r['accession']))
                if r not in selected:selected.append(r)
        if s['stratum']=='is_foreign_issuer' and selected and periodic and (dt.date.fromisoformat(s['session_date'])-dt.date.fromisoformat(selected[0]['filing_date'])).days>365:
            r=max(periodic,key=lambda r:(r['filing_date'],r['accession']))
            if r not in selected:selected.append(r)
        item={**s,'sources':[]}
        for r in selected:
            url=f"https://www.sec.gov/Archives/edgar/data/{r['cik']}/{r['accession'].replace('-','')}/{r['primary_document']}"
            rec=source_store.lookup(url);receipt_store=source_store
            if not rec or not rec.ok:
                rec=store.ensure(url,limiter=limiter,maximum=K.PRIMARY_MAXIMUM)[0];receipt_store=store
            source={**r,'url':url,'raw_sha256':rec.sha256,'status':rec.status,'error':rec.error}
            if rec.ok:
                raw=receipt_store.read(rec,maximum=K.PRIMARY_MAXIMUM);text=K.plain_text(raw)
                source['excerpts']={k:[text[max(0,m.start()-100):m.end()+300] for m in list(re.finditer(p,text))[:5]] for k,p in {
                    'issuer_cover':r'(?i)exact name of registrant',
                    'tax_status':r'(?i)(?:elect.{0,80}tax|tax.{0,60}reit|qualif.{0,60}reit|reit.{0,40}tax)',
                    'cover':r'(?i)(?:securities registered pursuant|title of each class|trading symbol|common stock|ordinary shares)',
                    'incorporation':r'(?i)(?:jurisdiction of incorporation|incorporation or organization|state of incorporation|incorporated in|incorporated under|cayman|bermuda|ireland|switzerland|israeli|taiwan|canada)',
                    'status':r'(?i)(?:real estate investment trust|no longer.{0,35}reit|terminate.{0,35}reit|reit election|conversion|business combination|blank.check company)',
                    'business':r'(?i)(?:we are|the company is|business overview|our business|nature of operations)',
                }.items()}
                source['registrant_ciks']=sorted(set(int(v) for v in re.findall(r'name=["\']dei:EntityCentralIndexKey["\'][^>]*>\s*0*(\d+)\s*<',raw.decode(errors='replace'),re.I)))
                source['reit_assertion']=K.reit_assertion(raw,s['name'] or '',s['cik'])
            item['sources'].append(source)
        out.append(item);print('hand evidence',len(out),s['stratum'],s['ticker'],'attempts',limiter.count,flush=True)
except I.RequestBudgetExhausted:stopped=True
result={'rule':sample['rule'],'sample_sha256':sample['sample_sha256'],'rows':out,'attempts_total':limiter.count,'budget_stop':stopped,'remaining':wanted[len(out):],'raw_root':str(hand_raw),'limits':'Primary originals are hand-audit evidence only, never model state inputs. Every affected all-common/REIT/foreign claim is rechecked; retained ADR/SPAC/fund claims require unchanged identity/stratum. Original annual context supplements latest pre-sample periodic disclosure for REIT conversion checks; no historical status is backdated.'}
C.write_json_atomic(art/'t2r2-hand-source-evidence.json',result);print('hand completed',len(out),'remaining',len(result['remaining']),'attempts',limiter.count)
