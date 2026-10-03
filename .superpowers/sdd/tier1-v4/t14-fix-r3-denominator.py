"""Guarded positive disclosure classification from retained Business sections, no negative inference."""
import collections,hashlib,json,re
from pathlib import Path
from atx_db.alpha_panel import common as C,fund_notes_disclosure as D
a=Path('../.superpowers/sdd/tier1-v4');root=C.stage_dir('fundamentals_notes')
con=C.connect(memory='150MB',threads=1)
census=(root/'segment_census.parquet').as_posix()
rows=con.execute(f"""SELECT adsh,cik,period,filed,available_at FROM read_parquet('{census}')
 WHERE form IN ('10-K','10-KT') AND year(filed) BETWEEN 2020 AND 2025""").fetchall()
keys={r[0].replace('-',''):r for r in rows}
con.register('annual_keys',__import__('pyarrow').table({'accession':list(keys)}))
claims=collections.defaultdict(list);available=set();errors=[]
for part in sorted(Path('data/raw/sec_text/sections').glob('part-*.parquet')):
 try:
  data=con.execute(f"""SELECT s.accession,s.section,substring(s.text,1,200000) FROM read_parquet('{part.as_posix()}') s
   JOIN annual_keys k ON replace(s.accession,'-','')=k.accession WHERE s.section IN ('business','risk','mdna')""").fetchall()
 except Exception as exc:
  errors.append({'part':str(part),'error':str(exc)});continue
 for accession,section,text in data:
  key=accession.replace('-','');available.add(key)
  for claim in D.disclosure_claims(text,str(keys[key][2])):
   claims[key].append(claim|{'part':str(part),'section':section,
    'section_scanned_sha256':hashlib.sha256(text.encode()).hexdigest(),'scan_characters':len(text)})
con.close();out=[]
for key,evidence in sorted(claims.items()):
 adsh,cik,period,filed,available_at=keys[key]
 numbers={e['declared_count'] for e in evidence}
 classification='conflicting_claims' if len(numbers)>1 else 'temporal_or_reorganization_ambiguous' if any(e['quality']=='temporal_or_reorganization_ambiguous' for e in evidence) else 'operating_only_reportable_unverified' if all(e['quality']=='operating_only_reportable_unverified' for e in evidence) else 'candidate_multi_period_uncorroborated' if next(iter(numbers))>=2 else 'candidate_single_period_uncorroborated'
 out.append({'adsh':adsh,'cik':cik,'period':str(period),'filed':str(filed),'available_at':str(available_at),
   'classification':classification,'evidence':evidence})
(a/'t14-fix-r3-disclosure-census.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in out),encoding='utf-8')
pilot={r['adsh'] for r in json.loads((a/'t14-fix-r2-pilot-selection-keys.json').read_text())}
summary={'all_original_annual':len(keys),'retained_public_section_accessions_scanned':len(available),
 'positive_disclosure_classifications':dict(collections.Counter(r['classification'] for r in out)),
 'without_explicit_claim':len(keys)-len(out),'without_readable_public_section_or_pilot_primary':sum(k not in available and r[0] not in pilot for k,r in keys.items()),
 'errors':errors,'denominator_certified':False,'basis':'explicit claim spans/offsets/period qualifiers, not certified reportable partitions; no negative inference from missing/truncated public section text'}
(a/'t14-fix-r3-denominator-summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
print(json.dumps(summary))
