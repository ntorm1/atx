"""Guarded reopened nine proofs, five automatic cuts, and all prior 497 text positives."""
import collections,gzip,hashlib,json,re
from pathlib import Path
from atx_db.alpha_panel import common as C,fund_notes_disclosure as D,fund_notes_semantic as P,fund_notes_structure as S
a=Path('../.superpowers/sdd/tier1-v4');raw=Path('data/raw/sec_notes_t14_pilot')
pilot=[json.loads(s) for s in (a/'t14-fix-r5-pilot-outcomes.jsonl').read_text(encoding='utf8').splitlines()]
by_adsh={r['adsh']:r for r in pilot};curated=[]
receipts=[json.loads(s) for s in (raw/'receipts.jsonl').read_text(encoding='utf8').splitlines()]
for adsh,proof in S.CORROBORATED.items():
 receipt=next(r for r in receipts if r['adsh']==adsh and r['kind']=='primary' and r['http_status']==200)
 compressed=(raw/receipt['path']).read_bytes();payload=gzip.decompress(compressed)
 assert hashlib.sha256(payload).hexdigest()==receipt['sha256']==proof['primary_sha256']
 current=by_adsh[adsh]['parser'];assert current['status']=='verified_structure' and current['members']==sorted(proof['members'])
 parsed=P.Primary(payload.decode('utf8','replace'));citations=[]
 for i,b in enumerate(parsed.blocks):
  if 'reportable segment' not in b['text'].lower():continue
  blocks=parsed.blocks[max(0,i-2):i+4];text=' '.join(b['text'] for b in blocks)
  if not any(alias in P.name_key(text) for member in proof['members'] for alias in P.member_aliases(member)):continue
  start,end=blocks[0]['start'],blocks[-1]['end'];span=parsed.text[start:end]
  citations.append({'start_offset':start,'end_offset':end,'visible_text':text,'raw_span_sha256':hashlib.sha256(span.encode()).hexdigest()})
 assert citations
 curated.append({'adsh':adsh,'source_receipt':receipt,'compressed_sha256':hashlib.sha256(compressed).hexdigest(),'primary_sha256':proof['primary_sha256'],'selected_members':current['members'],'citations':citations})
old=[json.loads(s) for s in (a/'t14-fix-r4-independent-positive-disclosures.jsonl').read_text(encoding='utf8').splitlines()]
grouped=collections.defaultdict(list)
for r in old:grouped[r['part']].append(r)
con=C.connect(memory='150MB',threads=1);checked=[];errors=[]
for part,proofs in grouped.items():
 keys=sorted({r['adsh'].replace('-','') for r in proofs})
 con.register('keys',__import__('pyarrow').table({'accession':keys}))
 rows=con.execute(f"SELECT s.accession,s.section,substring(s.text,1,200000) FROM read_parquet('{Path(part).as_posix()}') s JOIN keys k ON replace(s.accession,'-','')=k.accession WHERE s.section IN ('business','risk','mdna')").fetchall()
 texts={(accession.replace('-',''),section):text for accession,section,text in rows}
 for r in proofs:
  text=texts[(r['adsh'].replace('-',''),r['section'])];p=r['proof']
  assert hashlib.sha256(text.encode()).hexdigest()==r['section_scanned_sha256']
  assert text[p['start_offset']:p['list_end_offset']]==p['source_text']
  current=D.named_current_disclosures(text,r['period']);same=[c for c in current if c['start_offset']==p['start_offset']]
  tail=text[p['end_offset']:p['list_end_offset']]
  checked.append({'adsh':r['adsh'],'section':r['section'],'section_sha256':r['section_scanned_sha256'],'old_proof':p,'current_proofs':same,'syntax_partitions':D.list_partitions(tail,p['declared_count']),'retained_source_reopened':True})
con.close()
automatic=[r for r in pilot if r['parser']['status']=='verified_structure' and r['adsh'] not in S.CORROBORATED]
summary={'retained_primary_pilot_processed':len(pilot),'curated_proofs_reopened':len(curated),'curated_hash_and_role_matches':len(curated),'automatic_cuts':len(automatic),
 'automatic_external_measure_selections':[{'adsh':r['adsh'],'members':r['parser']['members'],'selection':r['parser'].get('external_revenue_selection',{})} for r in automatic],
 'prior_independent_positive_accessions_reopened':len({r['adsh'] for r in checked}),'prior_independent_spans_reopened':len(checked),
 'prior_accessions_still_positive':len({r['adsh'] for r in checked if r['current_proofs']}),
 'rejected_prior_accessions':sorted({r['adsh'] for r in checked}-{r['adsh'] for r in checked if r['current_proofs']}),
 'rejected_prior_spans':sum(not r['current_proofs'] for r in checked),'all_filer_denominator_certified':False}
(a/'t14-fix-r5-reopened-proof.json').write_text(json.dumps(curated,indent=2),encoding='utf8')
(a/'t14-fix-r5-prior-positive-revalidation.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in checked),encoding='utf8')
(a/'t14-fix-r5-retained-validation.json').write_text(json.dumps(summary,indent=2),encoding='utf8')
print(json.dumps(summary))
