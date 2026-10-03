from pathlib import Path
from atx_db.alpha_panel import common as C
art=Path('../.superpowers/sdd/tier1-v4');out=C.read_json(art/'t2-header-assessment.json'); ciks=sorted(set(c for r in out['cohorts'].values() for c in r['ciks']))
con=C.connect(memory='200MB',threads=1,db_file='t2_header_cache.duckdb');con.execute('CREATE TABLE cohort(cik BIGINT)');con.executemany('INSERT INTO cohort VALUES (?)',[(c,) for c in ciks])
out['existing_text_cache_pre2020']=con.execute("SELECT count(*),count(DISTINCT d.cik),min(filing_date),max(filing_date) FROM read_parquet('data/raw/sec_text/docs/*.parquet') d JOIN cohort USING(cik) WHERE filing_date<DATE '2020-01-01'").fetchone()
out['existing_sections_header_literal_pre2020']=con.execute("SELECT count(*),count(DISTINCT s.cik) FROM read_parquet('data/raw/sec_text/sections/*.parquet') s JOIN cohort USING(cik) WHERE filing_date<DATE '2020-01-01' AND contains(upper(text),'STATE OF INCORPORATION')").fetchone()
out['cache_limits']='Text docs are primary-document metadata; sections are selected parsed document text, not guaranteed complete EDGAR submission headers. Zero literal matches is no existing verified header-incorporation coverage in these caches, not proof every raw cache has been searched.'
C.write_json_atomic(art/'t2-header-assessment.json',out);print(out['existing_text_cache_pre2020']);print(out['existing_sections_header_literal_pre2020']);con.close()
