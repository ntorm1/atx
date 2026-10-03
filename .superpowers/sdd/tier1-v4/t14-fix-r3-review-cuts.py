import json,gzip,re,html
from pathlib import Path
a=Path('../.superpowers/sdd/tier1-v4');raw=Path('data/raw/sec_notes_t14_pilot')
receipts=[json.loads(s) for s in (raw/'receipts.jsonl').read_text().splitlines()]
for case in map(json.loads,(a/'t14-fix-r3-pilot-outcomes.jsonl').read_text().splitlines()):
 if case['parser']['status']!='verified_structure':continue
 rs=[r for r in receipts if r['adsh']==case['adsh'] and r['kind']=='primary']
 text=html.unescape(re.sub(r'<[^>]+>',' ',gzip.decompress((raw/rs[0]['path']).read_bytes()).decode('utf-8','replace')))
 text=re.sub(r'\s+',' ',text)
 matches=list(re.finditer(r'(?i)(?:reportable|operating) segments',text))
 excerpt=[text[max(0,m.start()-120):m.start()+700] for m in matches]
 if case['adsh'] in {'0000050493-23-000021','0001558370-25-001617','0001558370-25-001888','0001527166-24-000019','0001628280-25-015336','0001628280-20-010583','0001558370-25-001102'}:
  candidates=[s for s in excerpt if re.search(r'(?i)(?:segments? (?:are|consist|include|as follows)|(?:four|three|two|six|five|seven|eight) (?:operating|reportable) segments)',s)]
  print(json.dumps({'adsh':case['adsh'],'sha256':rs[0]['sha256'],'url':rs[0]['url'],
   'roots':case['parser']['roles'][0].get('roots'),'internal':case['parser']['roles'][0].get('internal'),'excerpts':candidates[-2:] or excerpt[-2:]}))
