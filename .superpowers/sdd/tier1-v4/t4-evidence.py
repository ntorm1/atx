from pathlib import Path
import json
from atx_db.alpha_panel import common as C, ratios as R
root=C.build_root(); out=root/'ratios'; con=C.connect(memory='300MB', threads=1)
v=C.read_json(out/'validation.json'); m=C.read_json(out/'manifest.json')
e={'manifest':m,'validation_summary':{},'primitive_coverage':{},'source_hash':C.sha256_file(root/'compustat_std'/'manifest.json'),'universe_hash':C.sha256_file(root/'universe'/'manifest.json'),'bytes':sum(p.stat().st_size for p in out.rglob('*') if p.is_file())}
e['validation_universe_hash']=v.get('universe_manifest_sha256')
e['code_matches_manifest']=C.sha256_file(Path(R.__file__))==m['code']['ratios.py']['sha256']
e['input_source_matches_manifest']=e['source_hash']==m['input_manifests_sha256']['compustat_std']
for k,x in v.items():
 if isinstance(x,dict) and 'pass' in x:
  e['validation_summary'][k]={a:b for a,b in x.items() if a not in ('checks','by_ratio','by_pair','by_year')}
R.member_cells(con,root,out)
for basis in ('member_equity','univ_all_common','univ_r3000p'):
 if basis!='member_equity':R.universe_cells(con,root,'ratios',basis)
 st=(out/'fund_state.parquet').as_posix()
 selects=[]
 for x in sorted({*R.FLOW_Q,*R.EPS_Q,*R.YTD_Q}):selects.append(f'count(s.{x}_ttm) as {x}')
 for x in R.BAL_Q:selects.append(f'count(s.{x}_q0) as {x}_balance')
 rel=con.execute(f"SELECT c.year,count(*) cells,count(c.cik) linked,count(s.cik) state,{','.join(selects)} FROM cells c LEFT JOIN read_parquet('{st}') s ON c.cik=s.cik AND c.fund_available_at=s.available_at GROUP BY1 ORDER BY1".replace('BY1','BY 1'))
 names=[d[0] for d in rel.description];e['primitive_coverage'][basis]=[dict(zip(names,r)) for r in rel.fetchall()]
e['handcheck_exceptions']={}
for row in v.get('handcheck',{}).get('checks',[]):
 for item in row.get('ratios',[]):
  if item['verdict'] not in ('match','both_absent'):
   key=item['ratio']+':'+item['verdict'];e['handcheck_exceptions'][key]=e['handcheck_exceptions'].get(key,0)+1
e['monthly']=con.execute(f"SELECT year(session_date),count(*),min(session_date),max(session_date) FROM read_parquet('{(out/'monthly'/'year=*'/'ratios_monthly.parquet').as_posix()}') GROUP BY 1 ORDER BY1".replace('BY1','BY 1')).fetchall()
e['schema_columns']={r[0]:r[1] for r in con.execute(f"DESCRIBE SELECT * FROM read_parquet('{(out/'monthly'/'year=*'/'ratios_monthly.parquet').as_posix()}')").fetchall()}
e['pit_monthly']=con.execute(f"SELECT count(*) FILTER(WHERE fund_available_at>=available_at),count(*) FILTER(WHERE available_at IS NULL),count(*)-count(DISTINCT(session_date,security_id)) FROM read_parquet('{(out/'monthly'/'year=*'/'ratios_monthly.parquet').as_posix()}')").fetchone()
e['state']=con.execute(f"SELECT count(*),count(DISTINCT cik),min(available_at),max(available_at) FROM read_parquet('{(out/'fund_state.parquet').as_posix()}')").fetchone()
con.close()
Path('../.superpowers/sdd/tier1-v4/t4-evidence.json').write_text(json.dumps(e,indent=2,default=str),encoding='utf-8')
print(json.dumps(e['validation_summary'],indent=2,default=str))
