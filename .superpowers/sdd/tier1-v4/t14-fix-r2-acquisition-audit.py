"""Existing receipt/metadata estimates for minimal primary disclosure acquisition; no network."""
import json
from pathlib import Path
from atx_db.alpha_panel import common as C

out=Path('../.superpowers/sdd/tier1-v4')
cases=[json.loads(line) for line in (out/'t14-fix-r2-causal-cases.jsonl').read_text(encoding='utf-8').splitlines()]
local=[{k:c[k] for k in ['adsh','status','selected_tag','n_revenue_members','n_members','alternate_full_concept_candidates','revenue_context_samples']}
       for c in cases if c['cause'].startswith('local_')]
receipts=[json.loads(line) for line in Path('data/raw/sec_notes/receipts.jsonl').read_text(encoding='utf-8').splitlines()]
zips={}
for receipt in receipts:
    if receipt.get('kind')=='zip' and receipt.get('bytes',0)>0 and receipt.get('key','')[:4] in [str(y) for y in range(2020,2026)]:
        zips[receipt['key']]={k:receipt.get(k) for k in ['key','url','bytes','span','status']}
con=C.connect(memory='300MB',threads=1)
census=(C.stage_dir('fundamentals_notes')/'segment_census.parquet').as_posix()
docs=[p.as_posix() for p in Path('data/raw/sec_text/docs').glob('part-*.parquet')]
metadata=con.execute(f"""SELECT count(*),quantile_cont(doc_bytes,.5),quantile_cont(doc_bytes,.9),max(doc_bytes)
 FROM read_parquet({docs},union_by_name=true) WHERE http_status=200 AND doc_bytes>0""").fetchone()
filings=(C.build_root()/'sec_filings/filings.parquet').as_posix()
requests=con.execute(f"""WITH f AS (SELECT accession,min(primary_document) primary_document FROM read_parquet('{filings}')
 WHERE primary_document IS NOT NULL AND primary_document<>'' AND form IN ('10-K','10-KT')
 AND filing_date BETWEEN DATE '2020-01-01' AND DATE '2025-12-31' GROUP BY 1)
 SELECT c.adsh,c.cik,c.filed,f.primary_document FROM read_parquet('{census}') c
 LEFT JOIN f ON replace(c.adsh,'-','')=replace(f.accession,'-','')
 WHERE c.form IN ('10-K','10-KT') AND year(c.filed) BETWEEN 2020 AND 2025 ORDER BY c.filed,c.adsh""").fetchall()
case_keys={c['adsh'] for c in cases}
inventory=[]
for accession,cik,filed,document in requests:
    base=f'https://www.sec.gov/Archives/edgar/data/{cik}/{accession.replace("-","")}'
    inventory.append({'adsh':accession,'cik':cik,'filed':str(filed),'gate_case':accession in case_keys,
      'index_url':base+'/index.json','document_url':base+'/'+document if document else None,
      'required':'whole annual document/Item8 segment note; relevant presentation/definition/calculation linkbases from filing index'})
request_counts={'all_original_10k':len(inventory),'known_primary_document':sum(bool(r['document_url']) for r in inventory),
 'gate_cases':sum(r['gate_case'] for r in inventory),'case_primary_document_known':sum(r['gate_case'] and bool(r['document_url']) for r in inventory)}
sample_median=metadata[1]
result={'local_cases':local,'notes_zip_sources':list(zips.values()),'notes_zip_count':len(zips),
 'notes_zip_download_bytes':sum(r['bytes'] for r in zips.values()),'largest_notes_zip_bytes':max(r['bytes'] for r in zips.values()),
 'existing_successful_document_size_sample':{'rows':metadata[0],'median_bytes':metadata[1],'p90_bytes':metadata[2],'max_bytes':metadata[3]},
 'request_counts':request_counts,
 'document_estimates_median_bytes':{'gate_cases':round(len(case_keys)*sample_median),'all_original_10k':round(len(inventory)*sample_median)},
 'request_estimate':{'case_known_doc_gets':request_counts['case_primary_document_known'],'case_index_gets':len(case_keys),
 'case_linkbase_gets_range':[len(case_keys),3*len(case_keys)],'all_filer_primary_gets':len(inventory)},
 'disk_note':'Estimates from historical uncompressed response bytes; prospective sizes vary. Fetch sequentially, retain case structure/Item8 text, remove bulky originals only after receipt/verification. Preserve >=40 GB free before >1GB landing.',
 'sec_access':'atx_db.sec_http approved User-Agent, <=5 requests/s; budget all requests/retries and write receipts.'}
con.close()
(out/'t14-fix-r2-acquisition-estimate.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
(out/'t14-fix-r2-acquisition-requests.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in inventory),encoding='utf-8')
print(json.dumps({k:v for k,v in result.items() if k not in ['notes_zip_sources','local_cases']}))
print(json.dumps(local))
