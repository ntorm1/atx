"""Reopen original primary bytes, retain independently readable citation spans for nine prior cuts."""
import gzip,hashlib,json,re
from pathlib import Path
from atx_db.alpha_panel import fund_notes_structure as S, fund_notes_semantic as P
raw=Path('data/raw/sec_notes_t14_pilot');receipts=list(map(json.loads,(raw/'receipts.jsonl').read_text().splitlines()))
out=[]
for adsh,proof in S.CORROBORATED.items():
 receipt=next(r for r in receipts if r['adsh']==adsh and r['kind']=='primary' and r['http_status']==200)
 compressed=(raw/receipt['path']).read_bytes();payload=gzip.decompress(compressed)
 assert hashlib.sha256(payload).hexdigest()==receipt['sha256']==proof['primary_sha256']
 parsed=P.Primary(payload.decode('utf8','replace'))
 citations=[]
 for i,block in enumerate(parsed.blocks):
  if 'reportable segment' not in block['text'].lower():continue
  scope=parsed.blocks[max(0,i-2):i+4]
  text=' '.join(b['text'] for b in scope)
  aliases={m for m in proof['members'] if any(a in P.name_key(text) for a in P.member_aliases(m))}
  if not aliases:continue
  start,end=scope[0]['start'],scope[-1]['end']
  source=parsed.text[start:end]
  citations.append({'start_offset':start,'end_offset':end,'offset_unit':'decoded UTF-8 HTML character',
                   'visible_text':text,'raw_span_sha256':hashlib.sha256(source.encode('utf8')).hexdigest(),
                   'matched_declared_names':sorted(aliases)})
 # Some grouped names differ from taxonomy spellings; retain all exact reportability
 # paragraphs so the source judgment is inspectable without trusting name aliases.
 all_claims=[b for b in parsed.blocks if 'reportable segment' in b['text'].lower()]
 out.append({'adsh':adsh,'source_receipt':receipt,'compressed_sha256':hashlib.sha256(compressed).hexdigest(),
             'primary_sha256':hashlib.sha256(payload).hexdigest(),'prior_selected_members':proof['members'],
             'citations':citations,'other_reportability_paragraphs':all_claims,
             'reopened_original_bytes':True,'not_arithmetic_certification':True})
Path('../.superpowers/sdd/tier1-v4/t14-fix-r4-reopened-proof.json').write_text(json.dumps(out,indent=2),encoding='utf8')
print(json.dumps({'reopened':len(out),'sha_matches':len(out),'with_citations':sum(bool(r['citations']) for r in out)}))
