"""Guarded complete retained-pilot structural parser outcomes and cached ZIP inventory."""
import json,collections
from pathlib import Path
from atx_db.alpha_panel import common as C,fund_notes as F,fund_notes_structure as S
a=Path('../.superpowers/sdd/tier1-v4'); raw=Path('data/raw/sec_notes_t14_pilot')
cases={c['adsh']:c for c in map(json.loads,(a/'t14-fix-r2-causal-cases.jsonl').read_text().splitlines())}
selected=json.loads((a/'t14-fix-r2-pilot-selection-keys.json').read_text())
con=C.connect(memory='150MB',threads=1)
con.register('keys',__import__('pyarrow').table({'adsh':[c['adsh'] for c in selected]}))
path=(C.stage_dir(F.STAGE)/'segments.parquet').as_posix()
facts=con.execute(f"""SELECT s.adsh,s.tag,s.member,s.value,s.ddate,s.uom,s.segments FROM read_parquet('{path}') s
 JOIN keys USING(adsh) WHERE s.metric='revenue' AND s.axis_kind='business' AND s.qtrs=4
 AND {F.reportable_container_sql('s')} ORDER BY 1,2,3""").fetchall()
con.close(); grouped=collections.defaultdict(list)
for adsh,*fact in facts:grouped[adsh].append(fact)
out=[]
for selected_case in selected:
 c=cases[selected_case['adsh']]; period=str(c['period'])
 rows=[r for r in grouped[c['adsh']] if str(r[3])==period and r[4]==c['currency']]
 tag=c['selected_tag']
 if not tag:
  present={r[0] for r in rows}
  tag=next((t for t in F.FULL_REVENUE_TAGS if t in present),None)
 if not tag:
  result={'status':'no_eligible_source_revenue_concept'}
 elif tag not in F.FULL_REVENUE_TAGS:
  result={'status':'component_or_unverified_concept','concept':tag}
 else:
  members={r[1] for r in rows if r[0]==tag and not __import__('re').search(F.RECONCILING_RE,r[1])}
  result=S.cached_structure(raw,c['adsh'],tag,period,members)
 out.append({'adsh':c['adsh'],'cik':c['cik'],'filed':c['filed'],'baseline_status':c['status'],
             'selected_source_tag':tag,'parser':result})
(a/'t14-fix-r4-pilot-outcomes.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in out),encoding='utf-8')
zips=[str(p) for p in Path('data/raw/sec_notes').rglob('*.zip')]
summary={'processed':len(out),'outcomes':dict(collections.Counter(r['parser']['status'] for r in out)),
 'retained_notes_zip_paths':zips,'retained_part_kinds':sorted({p.name for p in (C.build_root()/'notes/parts').glob('source=*/*')}),
 'independent_all_filer_denominator_certified':False,'primary_pilot_only':True}
(a/'t14-fix-r4-pilot-summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
print(json.dumps(summary))

