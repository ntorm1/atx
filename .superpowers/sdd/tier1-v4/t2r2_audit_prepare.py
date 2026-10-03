from pathlib import Path
from atx_db.alpha_panel import common as C, universe as U
root=C.build_root();art=Path('../.superpowers/sdd/tier1-v4');con=C.connect(memory='300MB',threads=1,db_file='t2r2_audit.duckdb');U.load_issuer_tables(con)
samples=[];sizes=[]
groups={'retained_blank_check_sic':'sic_asof=6770','fund_trust_financial_name':"regexp_matches(upper(ticker),'TRUST|FUND|CAPITAL') OR sic_asof BETWEEN 6000 AND 6999",'unverified_security_class':"security_type='common_unverified'",'unknown_incorporation':'inc IS NULL','recent_issuer_link_or_spac':'cik_u>=1800000','multiple_lines_same_issuer':"cik_u IN (SELECT cik_u FROM t WHERE exchange_u='XNYS' AND univ_all_common GROUP BY cik_u,session_date HAVING count(*)>1)",'ordinary_control':'true'}
for y,day in [(2021,'2021-11-30'),(2022,'2022-03-31')]:
    old=U._p;subset=(root/'_tmp'/f't2_month_panel_{y}.parquet').as_posix();U._p=lambda *parts:subset if parts[0]=='panel' else old(*parts)
    U.build_year(con,y);U._p=old
    for label,condition in groups.items():
        n=con.execute(f"SELECT count(*) FROM t WHERE session_date=DATE '{day}' AND exchange_u='XNYS' AND univ_all_common AND ({condition})").fetchone()[0];sizes.append([day,label,n])
        cur=con.execute(f"SELECT session_date,security_id,ticker,cik_u,inc,sic_asof,sale_ttm,security_type,shrout,cutoff FROM t WHERE session_date=DATE '{day}' AND exchange_u='XNYS' AND univ_all_common AND ({condition}) ORDER BY md5(cast(security_id AS VARCHAR)||'{day}:independent-audit-20261003') LIMIT 3")
        cols=[c[0] for c in cur.description]
        for row in cur.fetchall():
            r=dict(zip(cols,row));r['stratum']=label
            filing=con.execute("SELECT accession,url,form,filing_date,available_at,primary_document FROM read_parquet('data/raw/sec_text/docs/*.parquet') WHERE cik=? AND available_at<? AND form IN ('10-K','10-Q','20-F','40-F') AND http_status=200 ORDER BY available_at DESC LIMIT 1",[r['cik_u'],r['cutoff']]).fetchone()
            r['cached_primary']=dict(zip(['accession','url','form','filing_date','available_at','primary_document'],filing)) if filing else None
            if filing:
                r['cached_section_limit']='Do not inherit cached classification; fetch/read original primary disclosure. Live section glob produced Invalid data during inspection; foreign cache not modified.'
            samples.append(r)
C.write_json_atomic(art/'t2r2-independent-audit-sample.json',{'seed':'independent-audit-20261003','selection':'three deterministic admitted NYSE line-date samples per risk stratum and date; overlapping strata disclosed, no fitted count thresholds','stratum_sizes':sizes,'samples':samples,'limits':'A deterministic diagnostic audit, not an unbiased population precision estimate. Cache lacks some pre-date originals; requires archived accession discovery and primary disclosure fetch.'})
print('samples',len(samples),'unique',len({(str(r['session_date']),r['security_id']) for r in samples}));print([(r['ticker'],r['stratum'],r['cik_u'],bool(r['cached_primary'])) for r in samples]);con.close()
