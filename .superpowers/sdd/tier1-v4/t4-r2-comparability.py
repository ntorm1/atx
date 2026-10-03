import json, datetime as dt
from pathlib import Path
from atx_db.alpha_panel import common as C, ratios as R
root=C.build_root();con=C.connect(memory='400MB',threads=1);out={'scope':'same-date panel formula proxies; published characteristics stage absent; no universe read/no returns'}
R.member_cells(con,root,root/'ratios')
mon=(root/'ratios'/'monthly'/'year=*'/'ratios_monthly.parquet').as_posix();panel=(root/'panel'/'year=*'/'*.parquet').as_posix();state=(root/'ratios'/'fund_state.parquet').as_posix()
con.execute(f"CREATE TEMP TABLE cmp AS SELECT r.*,p.cik AS panel_cik,p.fund_clock,p.fund_period_end,p.currency,p.shares_out,p.raw_close,p.me_company,p.be AS panel_be,p.ni_ttm AS panel_ni,p.sale_ttm AS panel_sale,p.gp_ttm AS panel_gp,p.at AS panel_at,s.ni_ttm AS ratio_ni,s.sale_ttm AS ratio_sale,s.cogs_ttm,s.at_q0 AS ratio_at,s.seq_q0,s.pstk_q0,s.txditc_a0,s.txditc_a1,s.accession0,{','.join(a+' AS c_'+k+','+b+' AS r_'+k for k,(a,b) in R.SPEARMAN_PAIRS.items())} FROM cells r JOIN read_parquet('{panel}') p USING(session_date,security_id) LEFT JOIN read_parquet('{state}') s ON s.cik=r.cik AND s.available_at=r.fund_available_at")
def query(sql):
 rel=con.execute(sql);return [dict(zip([x[0] for x in rel.description],r)) for r in rel.fetchall()]
out['stage_hashes']={k:C.sha256_file(root/k/'manifest.json') if (root/k/'manifest.json').exists() else None for k in ('panel','characteristics','ratios','compustat_std')}
out['characteristics_published_files']=len(list((root/'characteristics').glob('year=*/characteristics.parquet')))
out['summary']=query("SELECT count(*) cells,count(*) FILTER(WHERE cik IS DISTINCT FROM panel_cik) issuer_diff,count(*) FILTER(WHERE fund_clock IS DISTINCT FROM fund_available_at) clock_diff,count(*) FILTER(WHERE fund_period_end IS DISTINCT FROM datadate) period_diff,count(*) FILTER(WHERE currency IS DISTINCT FROM curcd) currency_label_diff,count(*) FILTER(WHERE abs(me-me_company)>1e-6*greatest(abs(me),1)) cap_diff,count(*) FILTER(WHERE abs(shrout-shares_out)>1e-6*greatest(abs(shrout),1)) shares_diff,count(*) FILTER(WHERE abs(prc-raw_close)>1e-6*greatest(abs(prc),1)) close_diff FROM cmp")
out['by_formula']={}
primitive={'bm':('panel_be','be_q0'),'ep':('panel_ni','ratio_ni'),'sp':('panel_sale','ratio_sale'),'gpa':('panel_gp','ratio_sale-cogs_ttm')}
for k,(a,b) in primitive.items():
 out['by_formula'][k]={'summary':query(f"SELECT count(*) FILTER(WHERE c_{k} IS NOT NULL AND r_{k} IS NOT NULL) paired,count(*) FILTER(WHERE c_{k} IS NOT NULL AND r_{k} IS NOT NULL AND abs(c_{k}-r_{k})<=1e-6*greatest(abs(c_{k}),1e-12)) equal,count(*) FILTER(WHERE c_{k} IS NOT NULL AND r_{k} IS NOT NULL AND fund_clock IS DISTINCT FROM fund_available_at) paired_clock_diff,count(*) FILTER(WHERE c_{k} IS NOT NULL AND r_{k} IS NOT NULL AND abs(me-me_company)>1e-6*greatest(abs(me),1)) paired_cap_diff,count(*) FILTER(WHERE c_{k} IS NOT NULL AND r_{k} IS NOT NULL AND abs(({a})-({b}))>1e-6*greatest(abs({a}),1)) paired_numerator_diff FROM cmp"),'samples':query(f"SELECT session_date,security_id,cik,panel_cik,accession0,fund_clock,fund_available_at,fund_period_end,datadate,currency,curcd,me,me_company,shares_out,shrout,prc,raw_close,panel_be,be_q0,seq_q0,pstk_q0,txditc_a0,txditc_a1,panel_ni,ratio_ni,panel_sale,ratio_sale,panel_gp,cogs_ttm,panel_at,ratio_at,c_{k} AS panel_formula_proxy,r_{k} AS ratio_value FROM cmp WHERE c_{k} IS NOT NULL AND r_{k} IS NOT NULL AND abs(c_{k}-r_{k})>1e-6*greatest(abs(c_{k}),1e-12) QUALIFY row_number() OVER(PARTITION BY cik ORDER BY abs(c_{k}-r_{k})/greatest(abs(c_{k}),1e-12) DESC)=1 ORDER BY abs(c_{k}-r_{k})/greatest(abs(c_{k}),1e-12) DESC LIMIT 5")}
 # Independent cohort attribution only, never replaces the hard gate.
 for condition,label in [('true','all'),('fund_clock = fund_available_at AND fund_period_end=datadate','same_clock_period'),('fund_clock = fund_available_at AND fund_period_end=datadate AND abs(me-me_company)<=1e-6*greatest(abs(me),1)','same_clock_period_cap')]:
  out['by_formula'][k][label]=query(f"WITH z AS(SELECT session_date,c_{k},r_{k},rank() OVER(PARTITION BY session_date ORDER BY c_{k}) a,rank() OVER(PARTITION BY session_date ORDER BY r_{k}) b FROM cmp WHERE c_{k} IS NOT NULL AND r_{k} IS NOT NULL AND {condition}), d AS(SELECT session_date,count(*) n,corr(a,b) rho FROM z GROUP BY 1) SELECT count(*) dates,sum(n) cells,min(rho) min_rho,avg(rho) mean_rho FROM d WHERE n>=30")
con.execute(f"CREATE TEMP TABLE tails AS SELECT * FROM (SELECT r.*,s.pi_a0,s.sale_a0,s.accession0,s.curcd AS state_curcd,s.fx_converted AS state_fx FROM cells r JOIN read_parquet('{state}') s ON s.cik=r.cik AND s.available_at=r.fund_available_at WHERE r.ptpm IS NOT NULL) QUALIFY row_number() OVER(PARTITION BY cik,fy0_datadate ORDER BY session_date)=1")
out['ptpm_quantiles']=query('SELECT quantile_cont(ptpm,[.01,.5,.99]) quantiles,count(ptpm) n FROM cells')
tails=query('(SELECT cik,security_id,session_date,fy0_datadate,fund_available_at,pi_a0,sale_a0,ptpm,accession0,curcd,fx_converted FROM tails ORDER BY ptpm LIMIT 4) UNION ALL (SELECT cik,security_id,session_date,fy0_datadate,fund_available_at,pi_a0,sale_a0,ptpm,accession0,curcd,fx_converted FROM tails ORDER BY ptpm DESC LIMIT 4)')
fa=(root/'compustat_std'/'funda.parquet').as_posix()
for t in tails:
 t['standardized_annual']=query(f"SELECT cik,datadate,accession,available_at,fyear,fyr,pi,sale,curcd,fx_converted FROM read_parquet('{fa}') WHERE cik={t['cik']} AND datadate=DATE '{t['fy0_datadate']}' AND available_at<=TIMESTAMP '{t['fund_available_at']}' ORDER BY available_at DESC LIMIT 1")
fund_stage=C.read_json(root/'compustat_std'/'manifest.json')['fund_stage']
cf=list((root/fund_stage/'_work'/'cf').glob('batch-[0-9][0-9][0-9][0-9].parquet'))
out['retained_cf_batches']=len(cf)
if cf:
 out['cf_schema']=con.execute(f"DESCRIBE SELECT * FROM read_parquet('{cf[0].as_posix()}')").fetchall()
 cols=[x[0] for x in out['cf_schema']]
 if all(k in cols for k in ('cik','concept','period_start','period_end','value','accession_number')):
  ciklist=','.join(str(t['cik']) for t in tails)
  facts=query(f"SELECT cik,concept,period_start,period_end,unit,value,accession_number FROM read_parquet('{(root/fund_stage/'_work'/'cf'/'batch-[0-9][0-9][0-9][0-9].parquet').as_posix()}') WHERE cik IN ({ciklist}) AND concept IN ('IncomeLossFromContinuingOperationsBeforeIncomeTaxesExtraordinaryItemsNoncontrollingInterest','IncomeLossFromContinuingOperationsBeforeIncomeTaxesMinorityInterestAndIncomeLossFromEquityMethodInvestments','Revenues','RevenueFromContractWithCustomerExcludingAssessedTax','SalesRevenueNet','SalesRevenueGoodsNet','SalesRevenueServicesNet','SalesRevenueServicesGross','RevenueFromContractWithCustomerIncludingAssessedTax','RevenuesNetOfInterestExpense')")
  for t in tails:
   ann=t['standardized_annual'];acc=ann[0]['accession'] if ann else t['accession0']
   t['original_retained_facts']=[f for f in facts if f['cik']==t['cik'] and str(f['period_end'])==str(t['fy0_datadate']) and f['accession_number']==acc]
for t in tails:
 ann=t['standardized_annual']; acc=ann[0]['accession'] if ann else t['accession0']
 t['catalog_event_source']=query(f"SELECT cik,accession,period_end,clock_utc,sale_ttm,sale_src,currency FROM read_parquet('{(root/fund_stage/'events.parquet').as_posix()}') WHERE cik={t['cik']} AND accession='{acc}'")
out['ptpm_tail_samples']=tails
con.close();Path('../.superpowers/sdd/tier1-v4/t4-r2-comparability.json').write_text(json.dumps(out,indent=2,default=str));print(json.dumps({'summary':out['summary'],'quantiles':out['ptpm_quantiles'],'retained_cf_batches':len(cf)},default=str))
