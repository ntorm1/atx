import hashlib,json
from pathlib import Path
from lxml import html
from atx_db.alpha_panel import common as C,events_sources as ES

key='6d995da4afd51d81da1a3d1b1bfda72684a28c0c85bf6d22edc73284bbbedb81'
units=[json.loads(p.read_text()) for p in sorted((C.build_root()/'_tmp/ea_actuals/parse_units'/key).glob('*/*.json'))]
selected=[u for u in units if any('mixed_duration_columns' in r or 'currency_missing' in r for r in u['result']['extraction_reasons'])][:5]
ids={(u['identity']['accession'],u['identity']['doc_sha256']):u for u in selected}
store=ES.open_store();results=[]
for doc in ES.iter_catalog():
    unit=ids.get((doc['accession'],doc['doc_sha256']))
    if unit is None: continue
    blob=ES.read_document(doc,store)
    assert blob is not None and hashlib.sha256(blob).hexdigest()==doc['doc_sha256']
    root=html.fromstring(blob);tables=[]
    for n,table in enumerate(root.xpath('//table')):
        reasons=[r for r in unit['result']['extraction_reasons'] if r.startswith(f'table_{n}:') and ('mixed_duration' in r or 'currency_missing' in r)]
        if not reasons:continue
        trs=table.xpath('./tr|./tbody/tr|./thead/tr|./tfoot/tr')
        tables.append({'table':n,'rejections':reasons,'preceding_context':[' '.join(x.text_content().split())[-1000:] for x in table.xpath('preceding-sibling::*[position()<=4]')],
            'rows':[[{'text':' '.join(c.text_content().split()),'colspan':c.get('colspan','1'),'rowspan':c.get('rowspan','1')} for c in tr.xpath('./td|./th')] for tr in trs][:22]})
    canonical=f"https://www.sec.gov/Archives/edgar/data/{doc['cik']}/{doc['accession'].replace('-','')}/{doc['source_doc']}"
    results.append({'identity':unit['identity'],'url':doc['url'],
        'source_url_from_accession_identity':canonical,
        'url_note':'Catalog URL is null for landed_v2; canonical source URL constructed from exact retained accession/CIK/source_doc. No HTTP verification performed.',
        'headline':' '.join(root.text_content().split())[:1800],'tables':tables})
    ids.pop((doc['accession'],doc['doc_sha256']))
    if not ids:break
dest=Path('../.superpowers/sdd/tier1-v4/t9-r2-context.json')
C.write_json_atomic(dest,{'selection':'first five sorted completed cache units with mixed-duration or missing-currency table rejection; not precision sample','sources':results})
print(json.dumps({'sources':len(results),'context_file':str(dest),'tables':sum(len(r['tables']) for r in results)}))
