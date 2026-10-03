"""Bounded independent arithmetic from the existing 40 filing extractions; no new source discovery."""
import json, math
from collections import Counter
from pathlib import Path
from atx_db.alpha_panel import common as C
root=C.build_root(); con=C.connect(memory='200MB',threads=1)
v=C.read_json(root/'ratios'/'validation.json'); checks=[]
for r in v.get('handcheck',{}).get('checks',[]):
 if 'filing' not in r: continue
 h=r['filing']
 def div(a,b): return None if a is None or b is None or b==0 else a/b
 def add(a,b): return None if a is None or b is None else a+b
 average=None if h.get('seq') is None or h.get('seq_p') is None else (h['seq']+h['seq_p'])/2
 expected={'aftret_equity':div(h.get('ni'),average),'profit_lct':div(add(h.get('oiadp'),h.get('dp')),h.get('lct'))}
 sql=f"SELECT aftret_equity,profit_lct FROM read_parquet('{(root/'ratios'/'monthly'/'year=*'/'ratios_monthly.parquet').as_posix()}') WHERE cik=? AND session_date=CAST(? AS DATE) AND linkprim='P'"
 row=con.execute(sql,[r['cik'],r['session_date']]).fetchone()
 for i,(name,source) in enumerate(expected.items()):
  value=row[i] if row else None
  verdict='both_absent' if source is None and value is None else 'source_absent' if source is None else 'ratio_absent' if value is None else 'match' if abs(source-value)<=1e-6*max(abs(source),1e-12) else 'mismatch'
  checks.append({'cik':r['cik'],'accession':r['accession'],'session_date':r['session_date'],'ratio':name,'source':source,'ratio_value':value,'verdict':verdict})
out={'scope':'2 additional primary formulas, existing filing primitives only; neither absent verdict certifies identity','counts':dict(Counter(x['verdict'] for x in checks)),'checks':checks}
Path('../.superpowers/sdd/tier1-v4/t4-r1-extra-sourcecheck.json').write_text(json.dumps(out,indent=2))
print(json.dumps(out['counts']))
