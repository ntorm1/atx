from pathlib import Path
from atx_db.alpha_panel import common as C
root=C.build_root(); con=C.connect(memory='250MB',threads=1,db_file='t2_vendor_coverage.duckdb')
f=(root/'universe/year=2020/universe.parquet').as_posix();p=(root/'panel/year=2020/*.parquet').as_posix()
con.execute(f'''CREATE TABLE cohort AS SELECT a.security_id,a.cik FROM read_parquet('{f}') a JOIN read_parquet('{f}') b USING(security_id) WHERE a.session_date=DATE '2020-05-29' AND b.session_date=DATE '2020-07-31' AND a.univ_all_common AND b.univ_all_common AND a.exchcd_like=1 AND b.exchcd_like=1''')
con.execute(f'''CREATE TABLE missing AS SELECT c.* FROM cohort c LEFT JOIN read_parquet('{p}',union_by_name=true,hive_partitioning=false) p ON p.security_id=c.security_id AND p.session_date=DATE '2020-06-30' WHERE p.security_id IS NULL''')
out={'cohort_definition':'NYSE all_common on both neighboring month-end sessions May29 and July31; diagnostic only, no historical feature backfill', 'cohort_lines':con.execute('SELECT count(*) FROM cohort').fetchone()[0], 'missing_panel_june30':con.execute('SELECT count(*) FROM missing').fetchone()[0]}
price=(root/'prices/year=2020/*.parquet').as_posix();vendor=C.TICKERHISTORY.as_posix()
out['missing_panel_with_price_bar_june30']=con.execute(f"SELECT count(DISTINCT m.security_id) FROM missing m JOIN read_parquet('{price}',hive_partitioning=false) p ON p.security_id=m.security_id AND p.session_date=DATE '2020-06-30'").fetchone()[0]
out['missing_panel_with_vendor_bar_june30']=con.execute(f"SELECT count(DISTINCT m.security_id) FROM missing m JOIN read_parquet('{vendor}') v ON v.securityID=m.security_id AND v.tradingDate=DATE '2020-06-30'").fetchone()[0]
out['lines']=con.execute(f"SELECT m.security_id,m.cik,arg_max(p.ticker,p.session_date) ticker FROM missing m LEFT JOIN read_parquet('{p}',union_by_name=true,hive_partitioning=false) p ON p.security_id=m.security_id AND p.session_date<DATE '2020-06-30' GROUP BY 1,2 ORDER BY 1").fetchall()
C.write_json_atomic(Path('../.superpowers/sdd/tier1-v4/t2-vendor-coverage.json'),out);print(out);con.close()
