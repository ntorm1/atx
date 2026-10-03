import datetime as dt
from pathlib import Path
from atx_db.alpha_panel import common as C, universe as U, security_master as S
root=C.build_root();con=C.connect(memory='380MB',threads=1,db_file='t2r2_reconcile.duckdb');U.load_issuer_tables(con)
con.create_function('name_class',S.classify_finra,['VARCHAR','VARCHAR'],'VARCHAR',null_handling='special')
rows=[]; suspects=[]; classes=[]
for y in range(2018,2023):
 panel=(root/'panel'/f'year={y}'/'*.parquet').as_posix()
 subset=root/'_tmp'/f't2_month_panel_{y}.parquet'
 con.execute(f"COPY (SELECT * FROM read_parquet('{panel}',union_by_name=true,hive_partitioning=false) WHERE session_date IN (SELECT max(session_date) FROM read_parquet('{panel}',hive_partitioning=false) GROUP BY date_trunc('month',session_date))) TO '{subset.as_posix()}' (FORMAT PARQUET)")
 original_p=U._p
 U._p=lambda *parts: subset.as_posix() if parts[0]=='panel' else original_p(*parts)
 U.build_year(con,y)
 U._p=original_p
 con.execute('CREATE OR REPLACE TABLE month_t AS SELECT * FROM t WHERE session_date IN (SELECT max(session_date) FROM t GROUP BY date_trunc(\'month\',session_date))')
 qualified=U.all_common_sql('month_t').replace('coalesce(month_t.shrout > 0, false)', 'true')
 q=f'''SELECT session_date,count(*) FILTER(WHERE exchange_u='XNYS') nyse_rows,
 count(*) FILTER(WHERE exchange_u='XNYS' AND mi_repaired) base_common,
 count(*) FILTER(WHERE exchange_u='XNYS' AND mi_repaired AND NOT coalesce(shrout>0,false)) missing_shares,
 count(*) FILTER(WHERE exchange_u='XNYS' AND mi_repaired AND shrout>0 AND NOT coalesce(raw_close>=1,false)) price_excluded,
 count(*) FILTER(WHERE exchange_u='XNYS' AND mi_repaired AND {qualified} AND NOT coalesce(shrout>0,false)) otherwise_eligible_missing_shares,
 count(*) FILTER(WHERE exchange_u='XNYS' AND univ_all_common) admitted,
 count(DISTINCT coalesce(cik_u,-security_id)) FILTER(WHERE exchange_u='XNYS' AND univ_all_common) companies,
 count(*) FILTER(WHERE exchange_u='XNYS' AND univ_all_common AND inc IS NULL) unknown_inc,
 count(*) FILTER(WHERE exchange_u='XNYS' AND mi_repaired AND is_foreign_issuer) foreign_flag,
 count(*) FILTER(WHERE exchange_u='XNYS' AND mi_repaired AND is_spac) spac_flag,
 count(*) FILTER(WHERE exchange_u='XNYS' AND mi_repaired AND is_cef_etf) fund_bdc_flag,
 count(*) FILTER(WHERE exchange_u='XNYS' AND mi_repaired AND coalesce(filer_regime<>'domestic',true)) non_domestic_regime,
 count(*) FILTER(WHERE exchange_u='XNYS' AND mi_repaired AND regexp_matches(ticker,'SUFFIX')) suffix_flag
 , count(*) FILTER(WHERE exchange_u='XNYS' AND reit_observed=true) source_reit_positive, count(*) FILTER(WHERE exchange_u='XNYS' AND reit_observed=false) source_reit_termination FROM month_t GROUP BY 1 ORDER BY 1'''.replace('SUFFIX',U.SUFFIX_RE)
 cur=con.execute(q); cols=[c[0] for c in cur.description]
 rows += [dict(zip(cols,r)) for r in cur.fetchall()]
 panel=(root/'panel'/f'year={y}'/'*.parquet').as_posix()
 cur=con.execute(f'''SELECT t.session_date,t.security_id,t.ticker,t.cik_u,t.inc,t.sic_asof,t.sale_ttm,p.finra_issue_name,
 name_class(p.finra_issue_name,t.ticker) class_from_name,t.shrcd_like
 FROM month_t t JOIN read_parquet('{panel}',union_by_name=true,hive_partitioning=false) p USING(session_date,security_id)
 WHERE t.univ_all_common AND t.exchange_u='XNYS' AND name_class(p.finra_issue_name,t.ticker) NOT IN ('common','common_unverified') ORDER BY 1,2''')
 cols=[c[0] for c in cur.description];suspects += [dict(zip(cols,r)) for r in cur.fetchall()]
 cur=con.execute(f"""WITH c AS (SELECT session_date,
 CASE WHEN NOT mi_repaired THEN 'base_filter'
      WHEN NOT coalesce(shrout>0,false) THEN 'missing_shares'
      WHEN NOT coalesce(raw_close>=1,false) THEN 'price_below_one'
      WHEN coalesce(regexp_matches(ticker,'{U.SUFFIX_RE}'),false) THEN 'preferred_warrant_unit_right_suffix'
      WHEN NOT coalesce(filer_regime='domestic',false) THEN 'non_domestic_or_missing_regime'
      WHEN is_foreign_issuer THEN 'foreign_incorporation'
      WHEN is_spac THEN 'blank_check'
      WHEN is_cef_etf THEN 'fund_bdc'
      WHEN is_reit THEN 'reit'
      WHEN univ_all_common THEN 'admitted'
      ELSE 'unexplained_rule' END reason FROM month_t WHERE exchange_u='XNYS')
 SELECT session_date,reason,count(*) n FROM c GROUP BY 1,2 ORDER BY 1,2""")
 cols=[c[0] for c in cur.description];classes += [dict(zip(cols,r)) for r in cur.fetchall()]
 print(y,len(rows),len(suspects),flush=True)
french=dict(con.execute(f"SELECT month_end,n_nyse_firms FROM read_parquet('{U.FRENCH_BREAKPOINTS.as_posix()}')").fetchall())
for r in rows:
 r['french']=french.get((r['session_date'].replace(day=28)+dt.timedelta(days=4)).replace(day=1)-dt.timedelta(days=1));r['residual']=r['admitted']-r['french'] if r['french'] else None
C.write_json_atomic(Path('../.superpowers/sdd/tier1-v4/t2r2-french-reconciliation.json'),{'monthly':rows,'exclusive_classes':classes,'name_classifier_suspects':suspects,'limits':'Overlapping class counters are diagnostic, not additive attribution; no licensed CRSP line-level comparator exists. Unknown incorporation is never snapshot-backfilled. Same-FINRA-classifier suspects are internal consistency only, not independent class accuracy. Original receipt-backed class audit is a separate artifact.'})
con.close()
