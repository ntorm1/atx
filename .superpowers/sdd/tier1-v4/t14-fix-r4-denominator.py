"""Guarded independently named positive cached disclosure frame, never an exact census."""
import collections,hashlib,json
from pathlib import Path
from atx_db.alpha_panel import common as C, fund_notes_disclosure as D
a=Path('../.superpowers/sdd/tier1-v4');prior=[json.loads(s) for s in (a/'t14-fix-r3-disclosure-census.jsonl').read_text().splitlines()]
keys={r['adsh'].replace('-',''):r for r in prior}
con=C.connect(memory='150MB',threads=1);con.register('keys',__import__('pyarrow').table({'accession':list(keys)}))
out=[];errors=[]
for part in sorted(Path('data/raw/sec_text/sections').glob('part-*.parquet')):
 try:
  rows=con.execute(f"SELECT s.accession,s.section,substring(s.text,1,200000) FROM read_parquet('{part.as_posix()}') s JOIN keys k ON replace(s.accession,'-','')=k.accession WHERE section IN ('business','risk','mdna')").fetchall()
 except Exception as exc:errors.append({'path':str(part),'error':str(exc)});continue
 for accession,section,text in rows:
  case=keys[accession.replace('-','')]
  for proof in D.named_current_disclosures(text,case['period']):
   out.append({k:case[k] for k in ('adsh','cik','period','filed','available_at')}|{'section':section,'part':str(part),'section_scanned_sha256':hashlib.sha256(text.encode()).hexdigest(),'scan_characters':len(text),'proof':proof})
classifications=collections.defaultdict(set)
for r in out:classifications[r['adsh']].add(r['proof']['declared_count'])
consistent={k:next(iter(v)) for k,v in classifications.items() if len(v)==1}
con.register('positive_keys',__import__('pyarrow').table({'adsh':list(consistent),'declared_count':list(consistent.values())}))
path=(C.stage_dir('fundamentals_notes')/'segment_census.parquet').as_posix()
comparison=con.execute(f"SELECT year(filed),count(*) FILTER(WHERE declared_count>=2),count(*) FILTER(WHERE declared_count=1),count(*) FILTER(WHERE declared_count>=2 AND NOT multi_segment) FROM positive_keys JOIN read_parquet('{path}') USING(adsh) GROUP BY 1 ORDER BY 1").fetchall() if consistent else []
con.close()
(a/'t14-fix-r4-independent-positive-disclosures.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in out),encoding='utf8')
summary={'source_candidates_reopened':len(keys),'proof_spans':len(out),'positive_accessions':len(consistent),'conflicting_accessions':sum(len(v)>1 for v in classifications.values()),'by_year_multi_single_numeric_missing_multi':comparison,'errors':errors,'denominator_certified':False,'unknown_unexamined_filings':35677-len(consistent),'sampling':'nonprobability retained explicit-claim subset; no census or statistical extrapolation'}
(a/'t14-fix-r4-denominator-summary.json').write_text(json.dumps(summary,indent=2),encoding='utf8');print(json.dumps(summary))
