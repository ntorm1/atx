"""Guarded exact annual row/concept evidence from the retained automatic-source filings."""
import gzip,hashlib,json,re
from pathlib import Path
from atx_db.alpha_panel import common as C,fund_notes_semantic as P,fund_notes_structure as S
a=Path('../.superpowers/sdd/tier1-v4');raw=Path('data/raw/sec_notes_t14_pilot')
cuts=[r for r in json.loads((a/'t14-fix-r4-effects.json').read_text(encoding='utf8'))['cuts'] if 'automatic' in r['proof'].get('corroboration',{}).get('basis','')]
receipts=[json.loads(s) for s in (raw/'receipts.jsonl').read_text(encoding='utf8').splitlines()]
con=C.connect(memory='150MB',threads=1);out=[]
for cut in cuts:
 adsh=cut['adsh'];proof=cut['proof'];period=proof['period']
 receipt=next(r for r in receipts if r['adsh']==adsh and r['kind']=='primary' and r['http_status']==200)
 payload=gzip.decompress((raw/receipt['path']).read_bytes());assert hashlib.sha256(payload).hexdigest()==receipt['sha256']
 primary=P.Primary(payload.decode('utf8','replace'))
 tables=proof['corroboration']['table_spans']
 blocks=[b for b in primary.blocks if any(t['start']<=b['start']<=t['end'] for t in tables)]
 terms=[b for b in primary.blocks if re.search(r'(?i)segment revenues include|eliminat.{0,60}inter.?segment|revenue.{0,60}eliminat',b['text'])]
 contexts=S.primary_contexts(payload,proof['concept'],period)
 records=[]
 for f in primary.facts:
  if any(t['start']<=f['start']<=t['end'] for t in tables):records.append(f)
 items=(C.stage_dir('fundamentals_notes')/'items/year=*/items.parquet').as_posix()
 facts=con.execute(f"SELECT tag,segments,ddate,qtrs,uom,value,item_group,item FROM read_parquet('{items}',hive_partitioning=false) WHERE adsh=? AND ddate=? AND qtrs=4 AND coreg IS NULL ORDER BY 1,2",[adsh,period]).fetchall()
 out.append({'adsh':adsh,'source_sha256':receipt['sha256'],'cut':cut,'revenue_table_blocks':blocks,'source_measure_terms':terms,'inline_table_fact_identity':records,'retained_annual_num':facts})
con.close()
(a/'t14-fix-r5-components-inspect.json').write_text(json.dumps(out,indent=2,default=str),encoding='utf8')
for r in out:
 print(json.dumps({'adsh':r['adsh'],'measure_terms':r['source_measure_terms'],'revenue_table_text':[b['text'] for b in r['revenue_table_blocks']], 'num_revenue_related':[f for f in r['retained_annual_num'] if re.search(r'(?i)revenue|equityinearnings|incomelossfromequitymethod',f[0])]},default=str))
