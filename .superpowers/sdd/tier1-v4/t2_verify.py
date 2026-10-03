import datetime as dt
from pathlib import Path
from atx_db.alpha_panel import common as C, universe as U, security_master as S, market_common as M
root=C.build_root();con=C.connect(memory='350MB',threads=1,db_file='t2_verify.duckdb');out={}
for rel,keys in [('security_master/share_exchange_history.parquet','(security_id,valid_from)'),('indexes/constituents.parquet','(index_id,security_id,recon_date)'),('indexes/recon_summary.parquet','(index_id,recon_date)'),('market/index_returns.parquet','(freq,period_end,universe,weighting)')]:
 f=(root/rel).as_posix();out[rel]={'rows_keys':con.execute(f'SELECT count(*),count(DISTINCT {keys}) FROM read_parquet(\'{f}\')').fetchone(),'bytes':(root/rel).stat().st_size}
 f=(root/rel).as_posix()
 if 'index_returns' in rel:out[rel]['max_period']=con.execute(f"SELECT max(period_end),count(*) FILTER(WHERE period_end>=DATE '2023-01-01') FROM read_parquet('{f}')").fetchone()
f=(root/'security_master/share_exchange_history.parquet').as_posix()
out['codes_pit_bad']=con.execute(f'''WITH c AS (SELECT session_date,coalesce(lag(session_date) OVER(ORDER BY session_date),session_date-1) prev FROM {M.calendar_sql()})
 SELECT count(*) FROM read_parquet('{f}') h JOIN c ON c.session_date=h.valid_from WHERE h.available_at>=CAST(c.prev AS TIMESTAMP)+INTERVAL 22 HOUR''').fetchone()[0]
out['codes_overlap']=con.execute(f'''SELECT count(*) FROM (SELECT valid_from,lag(valid_to) OVER(PARTITION BY security_id ORDER BY valid_from) prior_to FROM read_parquet('{f}')) WHERE valid_from<=prior_to''').fetchone()[0]
out['per_year']={}
for y in range(2018,2027):
 panel=(root/'panel'/f'year={y}'/'*.parquet').as_posix();u=(root/'universe'/f'year={y}'/'universe.parquet').as_posix();m=(root/'indexes/membership'/f'year={y}'/'membership.parquet').as_posix()
 if not Path(u).exists():continue
 fresh=f'''SELECT p.session_date,p.security_id,p.exchange,f.market_class,p.session_date-f.dissemination_date age FROM read_parquet('{panel}',union_by_name=true,hive_partitioning=false) p
 ASOF LEFT JOIN (SELECT security_id,dissemination_date,any_value(market_class) market_class FROM read_parquet('{root.as_posix()}/security_master/finra_names.parquet') WHERE issue_name IS NOT NULL GROUP BY 1,2) f ON p.security_id=f.security_id AND p.session_date>f.dissemination_date'''
 out['per_year'][str(y)]={'universe_keys':con.execute(f"SELECT count(*),count(DISTINCT(session_date,security_id)) FROM read_parquet('{u}')").fetchone(),
 'exchange_classes':con.execute(f'''SELECT coalesce(f.market_class,'UNKNOWN'),count(*),count(*) FILTER(WHERE f.exchange IS NULL),count(*) FILTER(WHERE u.exchcd_like IS NULL),count(*) FILTER(WHERE u.univ_all_common),count(*) FILTER(WHERE u.univ_r2000p),count(*) FILTER(WHERE u.univ_r3000p)
 FROM ({fresh}) f JOIN read_parquet('{u}') u USING(session_date,security_id) GROUP BY 1 ORDER BY 1''').fetchall()}
 if Path(m).exists():
  out['per_year'][str(y)]['index_alignment_bad']=con.execute(f'''WITH c AS (SELECT session_date,coalesce(prev_session,session_date-1) prev FROM {M.calendar_sql()}) SELECT count(*) FROM read_parquet('{u}') u JOIN c USING(session_date) LEFT JOIN read_parquet('{m}') m ON u.session_date=m.session_date AND u.security_id=m.security_id AND m.available_at<CAST(c.prev AS TIMESTAMP)+INTERVAL 22 HOUR WHERE u.univ_r2000p<>coalesce(m.r2000,false) OR u.univ_r3000p<>coalesce(m.r3000,false)''').fetchone()[0]
 print(y,flush=True)
out['r2000_sc_recons']=[]
for d,rank in con.execute(f"SELECT effective_from,rank_date FROM read_parquet('{root.as_posix()}/indexes/recon_summary.parquet') WHERE index_id='R2000P' ORDER BY 1").fetchall():
 panel=(root/'panel'/f'year={rank.year}'/'*.parquet').as_posix()
 q=f'''SELECT count(*),count(DISTINCT coalesce(c.cik,-c.security_id)) FROM read_parquet('{root.as_posix()}/indexes/constituents.parquet') c
 ASOF LEFT JOIN (SELECT security_id,dissemination_date,any_value(market_class) market_class FROM read_parquet('{root.as_posix()}/security_master/finra_names.parquet') GROUP BY 1,2) f ON c.security_id=f.security_id AND c.rank_date>f.dissemination_date
 WHERE c.index_id='R2000P' AND c.effective_from=DATE '{d}' AND f.market_class='SC' AND c.rank_date-f.dissemination_date<=45'''
 out['r2000_sc_recons'].append([str(d),*con.execute(q).fetchone()])
out['manifests']={}
for rel in ('universe/manifest.json','indexes/manifest.json','market/market_index_manifest.json','security_master/share_exchange_history_manifest.json'):
 p=root/rel;doc=C.read_json(p)
 out['manifests'][rel]={'sha256':C.sha256_file(p),'input_hashes':doc.get('input_manifests_sha256'),
                      'code':doc.get('code'),'gate_status':doc.get('gate_status')}
C.write_json_atomic(Path('../.superpowers/sdd/tier1-v4/t2-after-verification.json'),out);con.close()
