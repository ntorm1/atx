"""Metadata-only frozen 200 mapping; never fetch, parse, replace cases or publish a stage."""
import argparse
import hashlib
import json
from pathlib import Path
from atx_db.alpha_panel import common as C, ea_actuals as EA, events_sources as ES, ea_publication as P


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--parse-receipt',required=True,help='stdout JSON from the approved bounded parser pass')
    ap.add_argument('--output',required=True)
    args=ap.parse_args()
    if Path(args.output).exists():
        raise RuntimeError('refusing to overwrite a retained audit map')
    checkpoint=json.loads(Path(args.parse_receipt).read_text(encoding='utf-8-sig').strip())
    if checkpoint.get('status')!='partial_cache_only' or checkpoint.get('published') is not False:
        raise RuntimeError('expected bounded incomplete parse receipt')
    sec=EA.verify_input('sec_filings'); catalog=ES.catalog_path(); cat_sha=C.sha256_file(catalog)
    if sec['sha256']!=checkpoint['input_manifests_sha256']['sec_filings']['sha256'] or cat_sha!=checkpoint['catalog_sha256']:
        raise RuntimeError('bounded parse source snapshot changed')
    code=C.code_identity('ea_actuals','ea_publication','events_sources','common')
    expected_key=hashlib.sha256(json.dumps({'code':{k:v['sha256_lf'] for k,v in code.items() if isinstance(v,dict)},
      'sec_filings':sec['sha256'],'catalog':cat_sha,'version':EA.VERSION},sort_keys=True).encode()).hexdigest()
    if expected_key!=checkpoint['run_key']:
        raise RuntimeError('bounded parse code/input identity changed')
    root=C.build_root(); con=C.connect(memory='150MB',threads=1)
    sql=f"""SELECT f.cik,f.accession,f.filing_date,f.available_at,f.acceptance_clock,f.vintage_risk
      FROM read_parquet('{(root/'sec_filings/filings.parquet').as_posix()}') f
      WHERE f.form='8-K' AND NOT f.is_amendment
        AND f.filing_date>=DATE '2020-01-01' AND f.filing_date<DATE '2026-01-01'
        AND EXISTS (SELECT 1 FROM read_parquet('{(root/'sec_filings/eight_k_items.parquet').as_posix()}') i
            WHERE i.cik=f.cik AND i.accession=f.accession AND i.item='2.02')
      ORDER BY sha256(CAST(f.cik AS VARCHAR)||'/'||f.accession||'/t9-precision-200-v1') LIMIT 200"""
    records=con.execute(sql).fetchall(); names=[d[0] for d in con.description]; con.close()
    rows=[dict(zip(names,r)) for r in records]
    if len(rows)!=200 or len({(r['cik'],r['accession']) for r in rows})!=200:
        raise RuntimeError('frozen denominator must contain 200 distinct originals')
    mapped={(r['cik'],r['accession']):r for r in rows}
    for r in rows:
        r.update(documents=[],manual_reviewed=False,manual_verdict=None,manual_reason=None)
    cache=root/'_tmp/ea_actuals/parse_units'/checkpoint['run_key']
    for doc in ES.iter_catalog():
        row=mapped.get((doc['cik'],doc['accession']))
        if row is None or doc['form']!='8-K' or doc['is_amendment']:continue
        identity={'run_key':checkpoint['run_key'],'cik':row['cik'],'accession':row['accession'],
          'filing_date':str(row['filing_date']),'available_at':str(row['available_at']),
          'acceptance_clock':row['acceptance_clock'],'source_doc':doc['source_doc'],'doc_sha256':doc['doc_sha256']}
        unit_key=hashlib.sha256(json.dumps(identity,sort_keys=True).encode()).hexdigest()
        path=cache/unit_key[:2]/(unit_key+'.json'); unit=C.read_json(path) if path.is_file() else None
        complete=unit is not None and unit.get('status')=='complete' and unit.get('identity')==identity
        canonical=f"https://www.sec.gov/Archives/edgar/data/{doc['cik']}/{doc['accession'].replace('-','')}/{doc['source_doc']}"
        row['documents'].append({'source_doc':doc['source_doc'],'doc_sha256':doc['doc_sha256'],
          'catalog_url':doc['url'],'canonical_url_from_identity':canonical,'doc_source':doc['doc_source'],
          'parse_unit_complete':complete,'parse_unit_path':str(path) if complete else None,
          'parser_result':unit['result'] if complete else None})
    for r in rows:
        r['mapping_reason']='catalog_documents_mapped' if r['documents'] else 'no_matching_catalog_document; other caches not audited'
        r['documents'].sort(key=lambda d:(d['source_doc'],d['doc_sha256']))
    key_sha=hashlib.sha256('\n'.join(str(r['cik'])+'/'+r['accession'] for r in rows).encode()).hexdigest()
    existing={'status':'no_published_parse_sample'}
    alias=root/EA.STAGE/'releases_manifest.json'
    if alias.is_file():
        import pyarrow.parquet as pq
        lineage=EA.verify_input(EA.STAGE,'releases_manifest.json',required_phase=('parsed',))
        sample=pq.read_table(P.file(EA.STAGE,'precision_sample.parquet','releases_manifest.json'),columns=['cik','accession']).to_pylist()
        sample_sha=hashlib.sha256('\n'.join(str(r['cik'])+'/'+r['accession'] for r in sample).encode()).hexdigest()
        existing={'status':'membership_match' if sample_sha==key_sha else 'membership_mismatch_reject',
                  'sample_keys_sha256':sample_sha,'parse_lineage':lineage}
        if sample_sha!=key_sha:raise RuntimeError('retained frozen sample membership mismatch; do not replace cases')
    if EA.verify_input('sec_filings')['sha256']!=sec['sha256'] or C.sha256_file(catalog)!=cat_sha:
        raise RuntimeError('source snapshot changed during audit mapping')
    result={'status':'metadata_map_only','manual_reviewed':0,'denominator':200,'selection':'lowest SHA256(cik/accession/t9-precision-200-v1), original 2.02 releases 2020-2025; missing cases retained',
      'sample_keys_sha256':key_sha,'parse_checkpoint':checkpoint,'existing_sample':existing,
      'input_manifests_sha256':{'sec_filings':sec},'catalog_sha256':cat_sha,'rows':rows,
      'limitations':'No raw-body reads/hash verification, HTTP, precision judgment or full parse. Catalog absence is not all-cache absence.'}
    C.write_json_atomic(Path(args.output),result)
    print(json.dumps({'status':result['status'],'denominator':200,'manual_reviewed':0,
      'sample_keys_sha256':key_sha,'catalog_mapped_cases':sum(bool(r['documents']) for r in rows),
      'completed_unit_cases':sum(any(d['parse_unit_complete'] for d in r['documents']) for r in rows),
      'existing_sample':existing,'output':args.output}))


if __name__=='__main__':main()
