from pathlib import Path
import zipfile
from atx_db.alpha_panel import common as C, universe as U
root=C.build_root(); art=Path('../.superpowers/sdd/tier1-v4'); con=C.connect(memory='300MB',threads=1,db_file='t2_headers.duckdb'); U.load_issuer_tables(con)
out={'acquisition_launched':False,'cohorts':{},'cache_schema':{},'code_hashes':{}}
for y in (2018,2019):
    old=U._p; subset=(root/'_tmp'/f't2_month_panel_{y}.parquet').as_posix()
    U._p=lambda *parts: subset if parts[0]=='panel' else old(*parts)
    U.build_year(con,y); U._p=old
    rows=con.execute("SELECT DISTINCT cik_u FROM t WHERE exchange_u='XNYS' AND univ_all_common AND inc IS NULL AND cik_u IS NOT NULL ORDER BY 1").fetchall()
    out['cohorts'][str(y)]={'distinct_ciks':len(rows),'ciks':[r[0] for r in rows]}
allc=sorted(set(c for r in out['cohorts'].values() for c in r['ciks']))
with zipfile.ZipFile('data/cache/submissions.zip') as z:
    names=set(z.namelist()); out['submissions_archive_matching_current_cik_files']=sum(f'CIK{c:010d}.json' in names or f'cik{c:010d}.json' in names for c in allc)
    out['submissions_sample_names']=list(sorted(names))[:3]
for rel in ('data/raw/sec_text/docs/*.parquet','data/raw/sec_text/sections/*.parquet'):
    out['cache_schema'][rel]=con.execute(f"DESCRIBE SELECT * FROM read_parquet('{rel}')").fetchall()
out['distinct_union_ciks']=len(allc)
out['minimum_candidate_header_requests']=len(allc)
out['request_estimate_limit']='One pre-first-eligibility accession header per CIK is a minimum; historic changes require additional dated headers. Current submissions profile state is forbidden for backdating. Cache schemas do not prove original complete submission headers exist.'
for rel in ('universe/manifest.json','indexes/manifest.json','market/market_index_manifest.json','security_master/share_exchange_history_manifest.json'):
    m=C.read_json(root/rel); code=m['code']; modules=[k[:-3] for k in code if k.endswith('.py')]; now=C.code_identity(*modules)
    out['code_hashes'][rel]={k:code[k]['sha256_lf']==now[k]['sha256_lf'] for k in code if k.endswith('.py')}
out['owned_product_disk_bytes']={d:sum(p.stat().st_size for p in (root/d).rglob('*') if p.is_file()) for d in ('universe','indexes','market')}
C.write_json_atomic(art/'t2-header-assessment.json',out);print({k:v for k,v in out.items() if k not in ('cohorts','cache_schema')}); print({k:v['distinct_ciks'] for k,v in out['cohorts'].items()});print(out['cache_schema']);con.close()
