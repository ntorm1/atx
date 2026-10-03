"""Guarded source/version provenance and downstream eligibility for 36 legacy item keys."""
import collections,json
from pathlib import Path
from atx_db.alpha_panel import common as C,notes_fetch as N,fund_notes as F
a=Path('../.superpowers/sdd/tier1-v4');root=C.stage_dir('fundamentals_notes');con=C.connect(memory='200MB',threads=1)
rows=[]
for file in sorted((root/'items').glob('year=*/items.parquet')):
 cur=con.execute(f"""SELECT * EXCLUDE (key_count) FROM (SELECT *,count(*) OVER (PARTITION BY adsh,tag,is_custom,segments,ddate,qtrs,uom,coreg) AS key_count FROM read_parquet('{file.as_posix()}')) WHERE key_count>1 ORDER BY adsh,tag,segments,ddate,qtrs,uom,coreg""")
 cols=[d[0] for d in cur.description]
 rows.extend(dict(zip(cols,r))|{'published_file':str(file)} for r in cur.fetchall())
by_source=collections.defaultdict(list)
for r in rows:by_source[r['source']].append(r)
raw=[]
for source,source_rows in by_source.items():
 con.register('collision_accessions',__import__('pyarrow').table({'adsh':sorted({r['adsh'] for r in source_rows})}))
 path=N.parts_dir()/f'source={source}'/'num_items.parquet'
 cur=con.execute(f"SELECT n.* FROM read_parquet('{path.as_posix()}') n JOIN collision_accessions USING(adsh)")
 cols=[d[0] for d in cur.description]
 for tup in cur.fetchall():
  r=dict(zip(cols,tup))
  if any(r['adsh']==p['adsh'] and r['tag']==p['tag'] and r['ddate']==p['ddate'] and r['qtrs']==p['qtrs'] and r['uom']==p['uom'] and (r['coreg'] or None)==(p['coreg'] or None) for p in source_rows):raw.append(r)
same_period=[r for r in rows if r['ddate']==r['period']]
business=[r for r in same_period if r['axis_kind']=='business']
key=lambda r:tuple(r[k] for k in ('adsh','tag','is_custom','segments','ddate','qtrs','uom','coreg'))
groups=collections.defaultdict(list)
for r in rows:groups[key(r)].append(r)
details=[]
for grouped in groups.values():
 p=grouped[0];values=sorted({r['value'] for r in grouped})
 provenance=[r for r in raw if r['adsh']==p['adsh'] and r['tag']==p['tag'] and str(r['ddate'])==str(p['ddate']) and r['qtrs']==p['qtrs'] and r['uom']==p['uom'] and (r['coreg'] or None)==(p['coreg'] or None) and r['dimn']==p['dimn']]
 detail={k:p[k] for k in ('adsh','tag','item_group','item','ddate','period','uom')}
 detail.update({'published_values':values,'disagreeing_values':len(values)>1,'raw_same_dimension_version_values':sorted({(r['version'],r['value']) for r in provenance})})
 if p['ddate']==p['period'] and p['item_group']!='segment':
  item=p['item'];wide=(root/'notes_wide.parquet').as_posix()
  val,currency=con.execute(f'SELECT "{item}",currency FROM read_parquet(\'{wide}\') WHERE adsh=?',[p['adsh']]).fetchone()
  item_file=Path(p['published_file']).as_posix()
  candidates=con.execute(f"SELECT tag,value FROM read_parquet('{item_file}') WHERE adsh=? AND ddate=? AND qtrs=0 AND coreg IS NULL AND segments IS NULL AND (uom=? OR uom IN ('pure','shares') OR ? IS NULL)",[p['adsh'],p['period'],currency,currency]).fetchall()
  ranks={t:i for it,g,kind,chain in F.ITEMS if it==item for i,t in enumerate(chain)}
  eligible=sorted((ranks[t],t,v) for t,v in candidates if t in ranks)
  best=eligible[0][:2] if eligible else None
  tied_values=sorted({v for rank,t,v in eligible if (rank,t)==best})
  detail.update({'current_notes_wide_value':val,'current_currency':currency,'preferred_notes_tag':best[1] if best else None,'preferred_tag_values':tied_values,'notes_wide_ambiguous_source_choice':len(tied_values)>1,'collision_tag_selected_by_precedence':bool(best and best[1]==p['tag'])})
 details.append(detail)
summary={'published_duplicate_rows':len(rows),'duplicate_key_groups':len(rows)//2,'by_year_rows':dict(collections.Counter(str(r['filed'])[:4] for r in rows)),
 'source_versions':sorted({r['version'] for r in raw}),'by_group_item_rows':dict(collections.Counter(r['item_group']+':'+r['item'] for r in rows)),
 'current_period_rows':len(same_period),'current_business_rows':len(business),
 'raw_provenance_rows':len(raw),'raw_exact_source_path_sha256':{source:C.sha256_file(N.parts_dir()/f'source={source}'/'num_items.parquet') for source in by_source},
 'advertised_item_key_unique':False,'outside_round5_item_extraction_diff':True,
 'disagreeing_value_key_groups':sum(r['disagreeing_values'] for r in details),
 'current_notes_key_groups':sum('current_notes_wide_value' in r for r in details),
 'current_notes_ambiguous_source_choices':sum(r.get('notes_wide_ambiguous_source_choice',False) for r in details),
 'current_notes_collision_tag_selected_by_precedence':sum(r.get('collision_tag_selected_by_precedence',False) for r in details),
 'segment_revenue_keys_affected':sum(r['item']=='revenue' for r in rows),
 'affected_feature_field_names':sorted({r['item'] for r in details if r.get('notes_wide_ambiguous_source_choice')} & set(F.FEATURE_COLS)),
 'published_feature_columns_affected':len({r['item'] for r in details if r.get('notes_wide_ambiguous_source_choice')} & set(F.FEATURE_COLS)),
 'diagnosis':'ny dedup includes version; public items omit version. US-GAAP and IFRS equal local tags collide. wide_sql tie order omits version/value for current tax/goodwill. Neither these note fields nor non-business assets enter segment features/reconciliation/census.'}
result={'summary':summary,'key_groups_and_downstream_notes':details,'published_rows':rows,'retained_raw_provenance_rows':raw}
(a/'t14-fix-r5-item-collisions.json').write_text(json.dumps(result,indent=2,default=str),encoding='utf8');con.close()
print(json.dumps(summary));print(json.dumps({'examples':rows[:6],'raw_examples':raw[:6]},default=str))
