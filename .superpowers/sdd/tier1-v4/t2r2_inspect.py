from pathlib import Path
import json
from atx_db.alpha_panel import common as C,sec_filings as SF
art=Path('../.superpowers/sdd/tier1-v4');h=C.read_json(art/'t2-header-assessment.json');ciks=sorted(set(c for v in h['cohorts'].values() for c in v['ciks']))
con=C.connect(memory='220MB',threads=1,db_file='t2r2_inspect.duckdb');con.execute('CREATE TABLE cohort(cik BIGINT)');con.executemany('INSERT INTO cohort VALUES (?)',[(c,) for c in ciks])
cur=con.execute("SELECT s.cik,s.accession,s.filing_date,s.available_at,s.source,substring(s.text,greatest(strpos(upper(s.text),'STATE OF INCORPORATION')-300,1),1000) excerpt FROM read_parquet('data/raw/sec_text/sections/*.parquet') s JOIN cohort USING(cik) WHERE filing_date<DATE '2020-01-01' AND contains(upper(text),'STATE OF INCORPORATION') ORDER BY 1,3")
cols=[c[0] for c in cur.description];out={'cached_candidates':[dict(zip(cols,r)) for r in cur.fetchall()]}
with open('data/cache/submissions.zip','rb') as fh:
    entry=next(e for e in SF.iter_zip_entries(fh) if e.name==f'CIK{ciks[0]:010d}.json')
    d=json.loads(SF.read_zip_member(fh,entry));out['sample_cik']=ciks[0];out['submission_keys']=list(d);out['filings_keys']=list(d.get('filings',{}));out['recent_keys']=list(d.get('filings',{}).get('recent',{}));out['history_files']=d.get('filings',{}).get('files',[]);out['sample_recent']={k:v[:2] for k,v in d.get('filings',{}).get('recent',{}).items()}
C.write_json_atomic(art/'t2r2-cached-inspection.json',out);print(out);con.close()
