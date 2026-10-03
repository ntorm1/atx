"""Bounded accession-key causal evidence; no numerical hierarchy inference or data changes."""
import json
from collections import Counter
from pathlib import Path

from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import fund_notes as F

root=C.stage_dir(F.STAGE)
con=C.connect(memory='300MB',threads=1)
full=','.join(repr(t) for t in F.FULL_REVENUE_TAGS)
con.execute(f"""CREATE TEMP TABLE cases AS SELECT * FROM read_parquet('{(root/'segment_recon.parquet').as_posix()}')
 WHERE year(filed) BETWEEN 2020 AND 2025 AND multi_segment AND recon_class NOT IN ('direct','with_reconciling','reconciled_search')""")
con.execute(f"""CREATE TEMP VIEW it AS SELECT * FROM read_parquet('{(root/'items/year=*/items.parquet').as_posix()}',hive_partitioning=false)""")
con.execute(f"""CREATE TEMP VIEW sg AS SELECT * FROM read_parquet('{(root/'segments.parquet').as_posix()}')""")
# Alternative contexts are identified by full concept identity and all-member coverage, not best arithmetic fit.
alt=con.execute(f"""WITH v AS (SELECT c.adsh,s.tag,s.member,
 arg_min(s.value,CASE WHEN s.consolidation_member IS NULL THEN 0 ELSE 1 END) AS value
 FROM cases c JOIN sg s USING(adsh) WHERE s.metric='revenue' AND s.axis_kind='business'
 AND s.tag IN ({full}) AND s.qtrs=4 AND s.ddate=c.period AND s.uom=c.currency
 AND (s.consolidation_member IS NULL OR s.consolidation_member='OperatingSegments')
 AND NOT regexp_matches(s.member,'{F.RECONCILING_RE}') GROUP BY 1,2,3),
 sums AS (SELECT adsh,tag,count(*) AS n,sum(value) s FROM v GROUP BY 1,2),
 totals AS (SELECT c.adsh,i.tag,min(i.value) t,count(DISTINCT i.value) nt FROM cases c JOIN it i USING(adsh)
 WHERE i.segments IS NULL AND i.coreg IS NULL AND i.qtrs=4 AND i.ddate=c.period AND i.uom=c.currency
 AND i.tag IN ({full}) GROUP BY 1,2)
 SELECT c.adsh,x.tag,x.n,x.s,t.t,c.revenue_tag,c.n_revenue_members,c.n_members,
 (abs(x.s-t.t)<=.01*abs(t.t)) AS reconciles
 FROM cases c JOIN sums x USING(adsh) JOIN totals t ON t.adsh=x.adsh AND t.tag=x.tag
 WHERE t.nt=1 AND t.t<>0 AND x.n=c.n_members AND x.tag<>coalesce(c.revenue_tag,'')
 ORDER BY c.adsh,{F.rev_prio_sql('x.tag')}""").fetchall()
by_alt={}
for row in alt:
    by_alt.setdefault(row[0],[]).append(row[1:])
# Raw NUM/DIM covers all numeric axis tags, including those rejected by the feature registry.
con.execute("CREATE TEMP TABLE rawfacts(adsh VARCHAR,tag VARCHAR,version VARCHAR,segments VARCHAR,ddate DATE,qtrs INTEGER,uom VARCHAR,value DOUBLE,coreg VARCHAR)")
sub=(C.build_root()/'notes/parts/source=*/sub.parquet').as_posix()
con.execute(f"""CREATE TEMP TABLE subs AS SELECT adsh,source FROM read_parquet('{sub}',hive_partitioning=false)
 WHERE adsh IN (SELECT adsh FROM cases) QUALIFY row_number() OVER(PARTITION BY adsh ORDER BY source)=1""")
parts=C.build_root()/'notes/parts'
for (source,) in con.execute('SELECT DISTINCT source FROM subs ORDER BY source').fetchall():
    part=parts/f'source={source}'
    con.execute(f"""CREATE OR REPLACE TEMP TABLE rawnum AS SELECT n.adsh,n.tag,n.version,n.dimh,n.ddate,n.qtrs,n.uom,n.value,n.coreg
     FROM read_parquet('{(part/'num_items.parquet').as_posix()}') n JOIN subs s USING(adsh,source) WHERE n.value IS NOT NULL
     QUALIFY row_number() OVER(PARTITION BY n.adsh,n.tag,n.version,n.ddate,n.qtrs,n.uom,n.dimh,coalesce(n.coreg,'') ORDER BY n.iprx)=1""")
    con.execute(f"""CREATE OR REPLACE TEMP TABLE rawdim AS SELECT dimhash,segments FROM read_parquet('{(part/'dim.parquet').as_posix()}')
      WHERE dimhash IN (SELECT dimh FROM rawnum)""")
    con.execute('INSERT INTO rawfacts SELECT n.adsh,n.tag,n.version,d.segments,n.ddate,n.qtrs,n.uom,n.value,n.coreg FROM rawnum n LEFT JOIN rawdim d ON d.dimhash=n.dimh')
con.execute(f"""CREATE TEMP TABLE raw_contexts AS SELECT r.*,c.period,c.currency,c.n_members,
 regexp_extract(r.segments,'(^|;)BusinessSegments=([^;]*)',2) member
 FROM rawfacts r JOIN cases c USING(adsh) WHERE r.coreg IS NULL AND r.ddate=c.period""")
cur=con.execute('''SELECT adsh,tag,segments,qtrs,uom,value FROM raw_contexts
 WHERE member<>'' ORDER BY adsh,tag,segments,qtrs,uom''')
raw_by={}
for adsh,tag,segments,qtrs,uom,value in cur.fetchall():
    raw_by.setdefault(adsh,[]).append({'tag':tag,'segments':segments,'qtrs':qtrs,'uom':uom,'value':value})
total_tags={}
for adsh,tag,qtrs,uom,value in con.execute('SELECT adsh,tag,qtrs,uom,value FROM raw_contexts WHERE segments IS NULL ORDER BY adsh,tag').fetchall():
    total_tags.setdefault(adsh,[]).append([tag,qtrs,uom,value])
all_cases=[]
for adsh,cik,filed,period,status,tag,nrev,nmembers,currency,candidate in con.execute('SELECT adsh,cik,filed,period,recon_class,revenue_tag,n_revenue_members,n_members,currency,candidate_only FROM cases ORDER BY filed,adsh').fetchall():
    facts=raw_by.get(adsh,[])
    current=[f for f in facts if f['qtrs']==4 and f['uom']==currency]
    revenue=[f for f in current if f['tag'] in F.SEGMENT_METRICS['revenue']+F.REVENUE_TIER2 or __import__('re').fullmatch(F.CUSTOM_REVENUE_RE,f['tag'])]
    extra=[f for f in revenue if not __import__('re').search(F.SEGMENTS_ONLY_RE,f['segments'])]
    alternatives=by_alt.get(adsh,[])
    if alternatives:
        cause='local_alternate_full_concept_with_all_numeric_members'
    elif nrev==0 and extra:
        cause='unknown_extra_axis_structure_requires_disclosure'
    elif nrev==0 and revenue:
        cause='local_context_or_concept_review_needed'
    elif nrev==0:
        cause='no_eligible_annual_revenue_fact_requires_expanded_source_or_disclosure'
    elif status=='no_total':
        cause='component_equivalence_or_consolidated_total_requires_source_evidence'
    elif candidate:
        cause='numeric_candidate_without_hierarchy_provenance_requires_disclosure'
    elif extra:
        cause='unknown_extra_axis_structure_requires_disclosure'
    else:
        cause='unknown_discrepancy_requires_primary_disclosure'
    case={'adsh':adsh,'cik':cik,'filed':str(filed),'period':str(period),'status':status,'selected_tag':tag,
          'n_revenue_members':nrev,'n_members':nmembers,'currency':currency,'cause':cause,
          'alternate_full_concept_candidates':alternatives,'current_annual_revenue_count':len(revenue),
          'extra_axis_revenue_count':len(extra),'revenue_context_samples':revenue[:3],
          'current_period_consolidated_tags':sorted(set(r[0] for r in total_tags.get(adsh,[]))),
          'source_notes_zip_pattern':f'https://www.sec.gov/files/dera/data/financial-statement-notes-data-sets/{{source}}_notes.zip',
          'edgar_index':f'https://www.sec.gov/Archives/edgar/data/{cik}/{adsh.replace("-","")}/{adsh}-index.html'}
    all_cases.append(case)
summary={'case_count':len(all_cases),'observed_revenue_misses':sum(x['n_revenue_members']>0 for x in all_cases),
 'missing_revenue':sum(x['n_revenue_members']==0 for x in all_cases),'cause_counts':dict(Counter(x['cause'] for x in all_cases)),
 'alt_case_count':len(by_alt),'alternate_source_reconciles':sum(any(r[-1] for r in rows) for rows in by_alt.values()),
 'hierarchy_tables_retained':False,'raw_parts_only':['sub','txt_dei','num_dei','num_items','dim']}
out=Path('../.superpowers/sdd/tier1-v4')
(out/'t14-fix-r2-causal-cases.jsonl').write_text(''.join(json.dumps(c)+'\n' for c in all_cases),encoding='utf-8')
(out/'t14-fix-r2-causal-summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
print(json.dumps(summary))
con.close()
