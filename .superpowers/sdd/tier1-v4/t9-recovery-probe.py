"""Cache-only bounded source probe. Run with the shared <=1GiB guard."""
import json
from pathlib import Path
from atx_db.alpha_panel import common as C, events_sources as ES, ea_actuals as EA

out = Path('C:/atx/.superpowers/sdd/tier1-v4/t9-recovery-probe.json')
con = C.connect(memory='200MB', threads=1)
catalog = ES.catalog_path()
rows = con.execute(f"""SELECT * FROM read_parquet('{catalog.as_posix()}')
  WHERE form='8-K' AND filing_date BETWEEN DATE '2020-01-01' AND DATE '2025-12-31'
    AND NOT is_amendment
  ORDER BY hash(accession || 't9-probe-v1') LIMIT 20""").fetchall()
cols = [x[0] for x in con.description]
store = ES.open_store()
output = []
for vals in rows:
    row = dict(zip(cols,vals))
    blob = ES.read_document(row, store)
    parsed = EA.parse_release(blob, row['filing_date']) if blob else {'reason':'missing_object'}
    text = ES.document_text(blob,row['source_doc']) if blob else ''
    output.append({'accession':row['accession'],'source_doc':row['source_doc'],
                   'doc_sha256':row['doc_sha256'],'clock':row['acceptance_clock'],
                   'parsed':parsed,'opening_text':text[:2000],
                   'table_excerpt':'\n'.join(l for l in text.splitlines() if any(x in l.lower() for x in ('months','revenues','diluted','net income','dollars','million')))[:4000]})
con.close()
out.write_text(json.dumps(output,default=str,indent=2)+'\n',encoding='utf-8')
print(json.dumps({'output':str(out),'documents':len(output),'qualified':sum(r['parsed'].get('saleq') is not None for r in output)}))
