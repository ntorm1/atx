from pathlib import Path
from atx_db.alpha_panel import common as C,universe as U
root=C.build_root();art=Path('../.superpowers/sdd/tier1-v4');con=C.connect(memory='300MB',threads=1,db_file='t2r2_class_cohort.duckdb');U.load_issuer_tables(con);rows=[]
for y in range(2018,2023):
    old=U._p;subset=(root/'_tmp'/f't2_month_panel_{y}.parquet').as_posix();U._p=lambda *parts:subset if parts[0]=='panel' else old(*parts)
    U.build_year(con,y);U._p=old
    cur=con.execute("SELECT cik_u,min(session_date) first_candidate,max(session_date) last_candidate,min(sic_asof) min_sic,max(sic_asof) max_sic,list(DISTINCT ticker) tickers,count(*) line_months FROM t WHERE exchange_u='XNYS' AND univ_all_common AND sic_asof BETWEEN 6000 AND 6999 GROUP BY 1 ORDER BY 1")
    cols=[c[0] for c in cur.description];rows.extend([dict(zip(cols,r)) for r in cur.fetchall()]);print(y,len(rows),flush=True)
union={}
for r in rows:
    c=r['cik_u'];u=union.setdefault(c,{'cik':c,'first_candidate':r['first_candidate'],'last_candidate':r['last_candidate'],'sic_values':[],'tickers':[],'line_months':0})
    u['first_candidate']=min(u['first_candidate'],r['first_candidate']);u['last_candidate']=max(u['last_candidate'],r['last_candidate']);u['sic_values']=sorted(set(u['sic_values']+[r['min_sic'],r['max_sic']]));u['tickers']=sorted(set(u['tickers']+r['tickers']));u['line_months']+=r['line_months']
out={'definition':'Complete admitted NYSE month-end issuer cohort2018-2022 with dated SIC6000-6999, before any new class facts; broad financial/real-estate screen is candidate selection, never REIT assignment','candidates':list(union.values()),'year_rows':rows,'selection_limits':'Month-end cohort is complete at60 measured benchmark sessions, not every intervening session. SIC outside range or missing may still harbor unidentified REITs; explicitly unassessed.'}
C.write_json_atomic(art/'t2r2-class-cohort.json',out);print('distinct candidate CIKs',len(union),'realestate',sum(any(6500<=s<=6599 for s in u['sic_values']) for u in union.values()));con.close()
