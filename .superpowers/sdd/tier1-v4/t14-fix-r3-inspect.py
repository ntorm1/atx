import gzip,json,xml.etree.ElementTree as E
from pathlib import Path
r=Path('data/raw/sec_notes_t14_pilot'); ns={'l':'http://www.xbrl.org/2003/linkbase'}; x='{http://www.w3.org/1999/xlink}'
receipts=[json.loads(s) for s in (r/'receipts.jsonl').read_text().splitlines()]
for row in receipts:
 if row['adsh']!='0000950170-23-004361' or row['kind']!='linkbase':continue
 root=E.fromstring(gzip.decompress((r/row['path']).read_bytes()))
 for link in root:
  role=link.get(x+'role',''); loc={e.get(x+'label'):e.get(x+'href','').split('#')[-1] for e in link if e.tag.endswith('loc')}
  arcs=[(loc.get(e.get(x+'from')),loc.get(e.get(x+'to')),e.get(x+'arcrole'),e.attrib) for e in link if e.tag.endswith('Arc')]
  if any('StatementBusinessSegmentsAxis' in str(a) for a in arcs):
   selected=[a[:3] for a in arcs if any(s and ('Member' in s or s.endswith('SegmentDomain')) for s in a[:2]) and not any(s and ('Consolidation' in s or 'ProductsAndServices' in s or 'Range' in s or 'Subsegment' in s) for s in a[:2])]
   print(json.dumps({'url':row['url'],'role':role,'arcs':selected},default=str))
