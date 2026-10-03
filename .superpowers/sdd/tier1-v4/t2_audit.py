import json,sys
from pathlib import Path
from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import market_common as M
root=C.build_root(); con=C.connect(memory='350MB',threads=1,db_file='t2_audit.duckdb')
phase=sys.argv[1]; out={}
for rel in ('validation/universe.json','validation/factors_market.json','indexes/manifest.json','market/market_index_manifest.json','security_master/share_exchange_history_manifest.json'):
 p=root/rel
 if p.exists(): out[rel]={'sha256':C.sha256_file(p),'doc':C.read_json(p)}
for y in range(2018,2027):
 panel=(root/'panel'/f'year={y}'/'*.parquet').as_posix()
 if not list((root/'panel'/f'year={y}').glob('*.parquet')):continue
 q=f'''WITH p AS (SELECT session_date,security_id,exchange FROM read_parquet('{panel}',union_by_name=true,hive_partitioning=false)),
 f AS (SELECT security_id,dissemination_date,any_value(market_class) market_class FROM read_parquet('{root.as_posix()}/security_master/finra_names.parquet') WHERE issue_name IS NOT NULL GROUP BY 1,2)
 SELECT coalesce(f.market_class,'UNKNOWN'),count(*),count(DISTINCT p.security_id),count(*) FILTER(WHERE p.exchange IS NULL),
 count(*) FILTER(WHERE p.exchange IS NULL AND p.session_date-f.dissemination_date<=45)
 FROM p ASOF LEFT JOIN f ON p.security_id=f.security_id AND p.session_date>f.dissemination_date GROUP BY 1 ORDER BY 1'''
 out[str(y)]=[list(r) for r in con.execute(q).fetchall()]
 print(y,flush=True)
C.write_json_atomic(Path('../.superpowers/sdd/tier1-v4')/f't2-{phase}-audit.json',out)
con.close()
