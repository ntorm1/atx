import json,gzip,re,hashlib
from pathlib import Path
from html.parser import HTMLParser
class Text(HTMLParser):
 def __init__(self):super().__init__();self.parts=[];self.hide=0
 def handle_starttag(self,t,a):
  if t in ('ix:header','script','style'):self.hide+=1
  if t in ('p','div','tr','li','br') and not self.hide:self.parts.append('\n')
 def handle_endtag(self,t):
  if t in ('ix:header','script','style'):self.hide=max(0,self.hide-1)
 def handle_data(self,d):
  if not self.hide:self.parts.append(d)
raw=Path('data/raw/sec_notes_t14_pilot');out=[]
for r in map(json.loads,(raw/'receipts.jsonl').read_text().splitlines()):
 if r['kind']!='primary' or r['http_status']!=200:continue
 body=gzip.decompress((raw/r['path']).read_bytes());assert hashlib.sha256(body).hexdigest()==r['sha256']
 p=Text();p.feed(body.decode('utf8','replace'));text=re.sub(r'[ \t\r\xa0]+',' ',''.join(p.parts))
 snippets=[text[max(0,m.start()-200):m.end()+900] for m in re.finditer('reportable segments',text,re.I)]
 out.append({'adsh':r['adsh'],'source':r,'snippets':snippets})
Path('../.superpowers/sdd/tier1-v4/t14-fix-r4-primary-snippets.json').write_text(json.dumps(out,indent=2),encoding='utf8')
print(json.dumps({'primary':len(out),'with_reportable_phrase':sum(bool(r['snippets']) for r in out),'snippets':sum(len(r['snippets']) for r in out)}))
