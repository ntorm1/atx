"""Snapshot/compare omitted-year economics and all-year census under guard."""
import hashlib,json,sys
from pathlib import Path
from atx_db.alpha_panel import common as C
root=C.stage_dir('fundamentals_notes')
con=C.connect(memory='150MB',threads=1)
result={}
for name,query in {
 'census':"SELECT adsh,n_members,multi_segment FROM read_parquet(?) ORDER BY adsh",
 'omitted_2020_2022_features':"SELECT adsh,bseg_n,bseg_rev_hhi,bseg_basis FROM read_parquet(?) WHERE year(filed) BETWEEN 2020 AND 2022 ORDER BY adsh",
 'omitted_2020_2022_recon':"SELECT adsh,n_members,multi_segment,n_revenue_members,recon_class FROM read_parquet(?) WHERE year(filed) BETWEEN 2020 AND 2022 ORDER BY adsh"
}.items():
 file='segment_census.parquet' if name=='census' else 'segment_features.parquet' if 'features' in name else 'segment_recon.parquet'
 rows=con.execute(query,[str(root/file)]).fetchall()
 result[name]={'rows':len(rows),'sha256':hashlib.sha256(json.dumps(rows,default=str,separators=(',',':')).encode()).hexdigest()}
result['segments']=json.loads((root/'coverage.json').read_text())['segments']
con.close()
base=Path('../.superpowers/sdd/tier1-v4/t14-fix-r2-preservation-before.json')
if sys.argv[-1]=='before':
 base.write_text(json.dumps(result,indent=2),encoding='utf-8')
else:
 old=json.loads(base.read_text())
 result['matches']={k:old[k]==result[k] for k in ('census','omitted_2020_2022_features','omitted_2020_2022_recon')}
 result['omitted_gate_counts_preserved']=all(old['segments'][str(y)]==result['segments'][str(y)] for y in range(2020,2023))
 Path('../.superpowers/sdd/tier1-v4/t14-fix-r2-preservation-after.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
 assert all(result['matches'].values()) and result['omitted_gate_counts_preserved']
print(json.dumps(result))
