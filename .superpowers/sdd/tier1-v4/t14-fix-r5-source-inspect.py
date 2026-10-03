"""Guarded retained-source diagnosis for bounded semantic corrections; no network."""
import collections, gzip, hashlib, json
from pathlib import Path
from atx_db.alpha_panel import common as C, fund_notes_semantic as P
a=Path('../.superpowers/sdd/tier1-v4'); raw=Path('data/raw/sec_notes_t14_pilot')
effects=json.loads((a/'t14-fix-r4-effects.json').read_text(encoding='utf8'))
automatic={r['adsh']:r for r in effects['cuts'] if 'automatic' in r['proof'].get('corroboration',{}).get('basis','')}
out=[]
for receipt in map(json.loads,(raw/'receipts.jsonl').read_text(encoding='utf8').splitlines()):
 if receipt['kind']!='primary' or receipt['http_status']!=200:continue
 body=gzip.decompress((raw/receipt['path']).read_bytes());assert hashlib.sha256(body).hexdigest()==receipt['sha256']
 primary=P.Primary(body.decode('utf8','replace'))
 snippets=[]
 for i,b in enumerate(primary.blocks):
  if 'reportable segment' in b['text'].lower():snippets.append(primary.blocks[max(0,i-1):i+7])
 out.append({'adsh':receipt['adsh'],'automatic':receipt['adsh'] in automatic,'source_sha256':receipt['sha256'],'snippets':snippets})
(a/'t14-fix-r5-source-inspect.json').write_text(json.dumps(out,indent=2),encoding='utf8')
print(json.dumps({'primary_reopened':len(out),'automatic':[{ 'adsh':r['adsh'],'claim':automatic[r['adsh']]['proof'].get('corroboration'),'snippets':r['snippets']} for r in out if r['automatic']]},indent=2))
