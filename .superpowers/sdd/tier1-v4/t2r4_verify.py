"""Standalone publication integrity and bounded-source checks; no return values read."""
from pathlib import Path
import json,datetime as dt
from atx_db.alpha_panel import common as C,issuer_history as I,issuer_classes as K
art=Path('../.superpowers/sdd/tier1-v4');root=C.build_root();out={}
con=C.connect(memory='350MB',threads=1,db_file='t2r4_verify.duckdb')
out['publications']={}
for stage in ('universe','issuer_history','issuer_classes'):
    directory=root/stage;manifest=C.read_json(directory/'manifest.json');actual=C.output_hashes(directory)
    item={'manifest_sha256':C.sha256_file(directory/'manifest.json'),'manifest_files':manifest['files'],'actual_files':actual,'all_files_match':actual==manifest['files'],'partial_files':[str(p) for p in directory.rglob('*.partial')],'code_matches':{}}
    for name,h in manifest['code'].items():
        if not isinstance(h,dict):continue
        p=C.PACKAGE_ROOT/'src/atx_db/alpha_panel'/name
        if p.exists():
            import hashlib
            item['code_matches'][name]=hashlib.sha256(p.read_bytes().replace(b'\r\n',b'\n')).hexdigest()==h['sha256_lf']
    out['publications'][stage]=item
out['universe_keys']={}
for y in range(2018,2027):
    p=root/'universe'/f'year={y}'/'universe.parquet'
    out['universe_keys'][str(y)]=con.execute(f"SELECT count(*),count(DISTINCT(session_date,security_id)),min(session_date),max(session_date) FROM read_parquet('{p.as_posix()}')").fetchone()
    print('verified keys',y,flush=True)
out['source_tables']={}
for stage,filename in [('issuer_history','incorporation.parquet'),('issuer_classes','incorporation.parquet'),('issuer_classes','reit_status.parquet')]:
    p=root/stage/filename
    row=con.execute(f"SELECT count(*),count(DISTINCT cik),count(DISTINCT(cik,accession)),count(*) FILTER(WHERE available_at<accepted_at),min(available_at),max(available_at) FROM read_parquet('{p.as_posix()}')").fetchone()
    byyear=con.execute(f"SELECT year(available_at),count(*),count(DISTINCT cik) FROM read_parquet('{p.as_posix()}') GROUP BY 1 ORDER BY 1").fetchall()
    out['source_tables'][f'{stage}/{filename}']={'rows_ciks_keys_badclocks_bounds':row,'availability_years':byyear}
    if filename=='reit_status.parquet':out['source_tables'][f'{stage}/{filename}']['status_counts']=con.execute(f"SELECT reit_status,count(*),count(DISTINCT cik) FROM read_parquet('{p.as_posix()}') GROUP BY 1").fetchall()
for stage in ('issuer_history','issuer_classes'):
    m=C.read_json(root/stage/'manifest.json')
    out['publications'][stage]['plan_matches']=C.sha256_file(root/stage/'plan.json')==m.get('plan_sha256',m.get('source_plan_sha256'))
    out['publications'][stage]['raw_ledger_matches']=C.sha256_file(I.RAW/'fetch-ledger.jsonl')==m['raw_ledger_sha256']
    from atx_db import sec_http as H
    out['publications'][stage]['network_contract_matches']=C.sha256_file(Path(H.__file__))==m['network_contract_sha256']
    if stage=='issuer_classes':out['publications'][stage]['assessment_matches']=C.sha256_file(root/stage/'assessment.json')==m['assessment_sha256']
attempts=[json.loads(line) for line in (I.RAW/'http-attempts.jsonl').read_text().splitlines()]
out['attempts']={'count':len(attempts),'within_cap':len(attempts)<=3000,'counter_sha256':C.sha256_file(I.RAW/'http-attempts.jsonl'),'first':attempts[0],'last':attempts[-1],'minimum_spacing_seconds':min(b['time']-a['time'] for a,b in zip(attempts,attempts[1:]))}
out['raw_receipts']={}
for directory in (I.RAW,C.PACKAGE_ROOT/'data/raw/sec_universe_hand_check/round2'):
    records=[json.loads(line) for line in (directory/'fetch-ledger.jsonl').read_text().splitlines()];latest={r['url']:r for r in records};ok=[r for r in latest.values() if r.get('status')==200 and not r.get('error')]
    out['raw_receipts'][str(directory)]={'ledger_sha256':C.sha256_file(directory/'fetch-ledger.jsonl'),'receipt_rows':len(records),'latest_urls':len(latest),'successful_headers':sum(r['url'].endswith('-index-headers.html') for r in ok),'successful_primary':sum(not r['url'].endswith('-index-headers.html') for r in ok),'latest_errors':[{'url':r['url'],'status':r.get('status'),'error':r.get('error')} for r in latest.values() if r not in ok],'historical_error_receipts':sum(bool(r.get('error')) for r in records),'successful_url_source_bytes':sum(r['bytes'] for r in ok),'raw_directory_bytes':sum(p.stat().st_size for p in directory.rglob('*') if p.is_file())}
out['class_assessment_reason_counts']={}
from collections import Counter
out['class_assessment_reason_counts']=dict(Counter(r['reason'] for r in C.read_json(root/'issuer_classes/assessment.json')['rows']))
out['source_unknown_inventory']={'incorporation':C.read_json(root/'issuer_history/manifest.json').get('unresolved'), 'class':[{k:r.get(k) for k in ('cik','accession','form','filing_date','reason','status')} for r in C.read_json(root/'issuer_classes/assessment.json')['rows'] if r.get('status') is None]}
out['input_manifest_freshness']={}
from atx_db.alpha_panel import market_common as M,universe as U
manifest=C.read_json(root/'universe/manifest.json')
actual_inputs=M.input_manifests(*U.INPUTS)
out['input_manifest_freshness']={'expected':manifest['input_manifests_sha256'],'actual':actual_inputs,'all_match':manifest['input_manifests_sha256']==actual_inputs}
p=root/'market/index_returns.parquet'
out['return_boundary']=con.execute(f"SELECT max(period_end),count(*) FILTER(WHERE period_end>=DATE '2023-01-01'),count(*) FROM read_parquet('{p.as_posix()}')").fetchone()
for name in ('panel','issuer_history','issuer_classes','universe'):
    p=root/name/'manifest.json';out.setdefault('manifest_hashes',{})[name]=C.sha256_file(p)
out['validation']=C.read_json(root/'validation/universe.json')
out['independent_audit_current_flags']=[]
for r in C.read_json(art/'t2r2-independent-audit-verdicts.json')['rows']:
    p=root/'universe'/f"year={r['session_date'][:4]}"/'universe.parquet'
    flags=con.execute(f"SELECT univ_all_common,is_reit,is_foreign_issuer,is_spac,is_cef_etf FROM read_parquet('{p.as_posix()}') WHERE security_id=? AND session_date=?::DATE",[r['security_id'],r['session_date']]).fetchone()
    out['independent_audit_current_flags'].append({'ticker':r['ticker'],'security_id':r['security_id'],'session_date':r['session_date'],'initial_audit_verdict':r['verdict'],'flags':flags})
    if r['verdict']=='confirmed_wrong_admission_reit':assert flags and not flags[0] and flags[1]
out['hand_judgments_sha256']=C.sha256_file(C.PACKAGE_ROOT/'docs/datasets/universe_hand_check.json')
out['hand_sample_sha256']=C.sha256_file(root/'validation/universe_hand_check_sample.json')
C.write_json_atomic(art/'t2r4-after-verification.json',out)
assert all(x['all_files_match'] and not x['partial_files'] and all(x['code_matches'].values()) for x in out['publications'].values()),'publication integrity failed'
assert all(v[0]==v[1] for v in out['universe_keys'].values()),'duplicate universe key'
assert all(v['rows_ciks_keys_badclocks_bounds'][0]==v['rows_ciks_keys_badclocks_bounds'][2] and v['rows_ciks_keys_badclocks_bounds'][3]==0 for v in out['source_tables'].values()),'source keys/clock failed'
assert out['attempts']['within_cap'] and out['return_boundary'][1]==0
assert out['attempts']['minimum_spacing_seconds']>=1.0
assert all(v['plan_matches'] and v['raw_ledger_matches'] for k,v in out['publications'].items() if k!='universe')
assert out['input_manifest_freshness']['all_match']
assert out['manifest_hashes']['panel']=='c19a4d850c3ed9f59d1202e40c7d77b3147def81c45bd676302008a75d5fb2b4','foreign frozen panel changed'
print('all published SHA/size, key and source-clock checks passed',flush=True)
con.close()
