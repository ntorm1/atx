"""Full phase-specific Task9 public replay using unchanged native parser kernels."""
from __future__ import annotations
from collections import Counter, defaultdict
import gzip
import hashlib
import json
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

from . import common as C, ea_actuals as E, earnings_calendar_v2 as V
from . import earnings_calendar as EC, ea_publication as P, events_sources as ES
from . import private_tier1_admission as A

PHASES = {'ea_actuals': {'parsed': ('releases.parquet', 'precision_sample.parquet'),
                         'features': ('ann_features.parquet', 'filing_lags.parquet', 'reaction_returns.parquet'),
                         'validated': ('ann_features.parquet','filing_lags.parquet','reaction_returns.parquet','validation.json')},
          'earnings_calendar_v2': {'calendar': ('announcements.parquet',)}}
CODE = ('tier1_native_readers', 'private_tier1_admission', 'private_tier1_earnings',
        'ea_actuals', 'ea_publication', 'earnings_calendar_v2', 'earnings_calendar', 'events_sources', 'common')


def publication(name, spec):
    lake = A.root(spec); directory = lake/name
    generation = A.member(lake, spec['generation'])
    if generation.parent != directory/'generations':
        raise ValueError('explicit Task9 immutable generation required')
    manifest = C.read_json(generation/'manifest.json')
    if A.pin(generation/'manifest.json') != spec['manifest_pin']:
        raise ValueError('independent Task9 manifest identity differs')
    phase = manifest.get('phase')
    if phase not in PHASES[name] or phase != spec['required_phase']:
        raise ValueError('required Task9 publication phase differs')
    schema = E.SCHEMA if name == 'ea_actuals' else V.SCHEMA
    if manifest.get('stage') != name or manifest.get('schema') != schema or manifest.get('status') != 'complete':
        raise ValueError('unsupported native Task9 publication')
    if manifest.get('generation') != generation.relative_to(directory).as_posix():
        raise ValueError('Task9 generation/manifest location mismatch')
    products = {product: (generation/product).relative_to(directory).as_posix() for product in PHASES[name][phase]}
    if manifest.get('products') != products or set(manifest.get('files', {})) != set(products.values()):
        raise ValueError('incomplete or substituted Task9 products')
    dependencies = A.files(directory, manifest)
    dependencies[str(generation/'manifest.json')] = spec['manifest_pin']
    code_names = ('ea_actuals', 'ea_publication', 'events_sources', 'common') if name == 'ea_actuals' else (
        'earnings_calendar_v2', 'earnings_calendar', 'events_sources', 'common')
    actual = C.code_identity(*code_names)
    for module, value in actual.items():
        if isinstance(value, dict) and manifest.get('code', {}).get(module, {}).get('sha256_lf') != value['sha256_lf']:
            raise ValueError('Task9 native producer code changed')
    # Preserve parse and current feature dependencies independently, avoiding
    # a parse/calendar/features cycle and rejecting alias generation substitution.
    for key, declared in manifest['input_manifests_sha256'].items():
        path = Path(declared['path']) if isinstance(declared, dict) else A.member(lake, key if key.endswith('.json') else key+'/manifest.json')
        expected = declared['sha256'] if isinstance(declared, dict) else declared
        if A.pin(path)['sha256'] != expected:
            raise ValueError('Task9 actual reader dependency changed')
        dependencies[str(path.resolve())] = A.pin(path)
        if path.name.endswith('manifest.json'):
            base = path.parent.parent.parent if path.parent.parent.name == 'generations' else path.parent
            dependencies.update(A.files(base, C.read_json(path)))
    dependencies.update(A.code(CODE));dependencies.update(A.execution(spec))
    return lake, generation, manifest, dependencies


class DocumentBytes(dict):
    """Retain paths/identities, reading at most one original document at a time."""
    def __init__(self):
        super().__init__();self.descriptors={}

    def get(self,sha,default=None):
        path=super().get(sha)
        if path is None:return default
        raw=path.read_bytes()
        blob=gzip.decompress(raw) if path.suffix=='.gz' else raw
        if hashlib.sha256(blob).hexdigest()!=sha:raise ValueError('retained source document changed during replay')
        return blob


def documents(lake, dependencies):
    """Read native retained as-served bytes without fetch or quarantine mutations."""
    catalog = ES.catalog_path()
    dependencies[str(catalog.resolve())] = A.pin(catalog)
    store = ES.open_store()
    if store.ledger_path.exists(): dependencies[str(store.ledger_path.resolve())] = A.pin(store.ledger_path)
    rows = list(ES.iter_catalog())
    blobs = DocumentBytes()
    for row in rows:
        if row['form'] != '8-K' and not (row['form'] or '').startswith('6-K'): continue
        sha = row['doc_sha256']
        if row['doc_source'] == 'landed_v2':
            directory = ES.EARNINGS_RELEASE_DIR/'objects'/sha[:2]
        else:
            receipt = store.lookup(row['url'])
            if receipt is None or not receipt.ok:
                blobs[sha] = None; blobs.descriptors[sha]=None; continue
            if receipt.sha256 != sha:
                raise ValueError('native document ledger/catalog identity differs')
            directory = store.objects_dir/sha[:2]
        compressed, plain = directory/(sha+'.gz'), directory/sha
        path = compressed if compressed.exists() else plain
        if not path.exists(): blobs[sha] = None; blobs.descriptors[sha]=None; continue
        dependencies[str(path.resolve())] = A.pin(path)
        raw = path.read_bytes()
        blob = gzip.decompress(raw) if path == compressed else raw
        if hashlib.sha256(blob).hexdigest() != sha:
            raise ValueError('retained source document byte identity differs')
        blobs[sha] = path
        blobs.descriptors[sha]={'sha256':sha,'bytes':len(blob)}
    return rows, blobs


def parsed_rows(con, lake, catalog, blobs):
    filings = A.sql([lake/'sec_filings/filings.parquet']); items = A.sql([lake/'sec_filings/eight_k_items.parquet'])
    cursor = con.execute(f"""SELECT f.cik,f.accession,f.filing_date,f.available_at,f.acceptance_clock,f.vintage_risk
        FROM {filings} f WHERE NOT f.is_amendment AND f.form='8-K' AND f.filing_date>=DATE '2015-01-01'
        AND EXISTS(SELECT 1 FROM {items} i WHERE i.cik=f.cik AND i.accession=f.accession AND i.item='2.02')
        ORDER BY f.cik,f.available_at,f.accession""")
    columns = [d[0] for d in cursor.description]; base = cursor.fetchall()
    by_key = defaultdict(list)
    for row in catalog:
        if row['form'] == '8-K' and not row['is_amendment']: by_key[(row['cik'], row['accession'])].append(row)
    output = []
    for values in base:
        row = dict(zip(columns, values)); row['source_available_at'] = row['available_at']
        row['available_at'] = E.qualified_clock(row)
        row['clock_reason'] = 'qualified' if row['available_at'] is not None else 'acceptance_clock_missing_or_unresolved'
        extracts, names, hashes = [], [], []
        missing_objects = 0
        for document in by_key.get((row['cik'], row['accession']), []):
            sha = document['doc_sha256']
            if sha in hashes: continue
            blob=blobs.get(sha)
            if blob is None:
                missing_objects += 1; continue
            names.append(document['source_doc']); hashes.append(sha)
            extracts.append(E.parse_release(blob, row['filing_date']))
        qualified = [r for r in extracts if any(r.get(f) is not None for f in E.FIELDS)]
        if qualified: row.update(E.merge_extracts(extracts))
        else:
            row.update(E.merge_extracts(extracts) if extracts else {'reason': 'no_cached_document'})
            row['extraction_reasons'] = [reason for r in extracts for reason in r.get('extraction_reasons', [])][:12]
            periods = {r['period_end'] for r in extracts if r.get('period_end') is not None}
            if len(periods) == 1: row.update(period_end=next(iter(periods)), period_basis='source_explicit_end_date')
            elif periods: row.update(period_basis='conflicting_document_periods')
        pairs = sorted(zip(names, hashes))
        row.update(source_doc=[p[0] for p in pairs], doc_sha256=[p[1] for p in pairs], _missing_objects=missing_objects); output.append(row)
    return output


def fpi_rows(catalog, blobs):
    by_accession = {}; seen = set()
    for row in catalog:
        q = row.get('qids') or []
        if not (row.get('form') or '').startswith('6-K') or not ('fpi_results' in q or 'fpi_period_results' in q): continue
        key = row['accession'], row.get('doc_sha256') or row['source_doc']
        if key in seen or row['filing_date'] is None or row['filing_date'] < V.START_6K: continue
        blob = blobs.get(row['doc_sha256']); valid = blob is not None
        parsed = V.classify_release(ES.document_text(blob, row['source_doc']) if valid else '', row['filing_date'])
        seen.add(key)
        result = {'cik': row['cik'], 'accession': row['accession'], 'form': row['form'], 'source_doc': row['source_doc'],
                  'doc_sha256': row.get('doc_sha256'), 'filing_date': row['filing_date'], 'is_results': parsed is not None,
                  'period_end': parsed and parsed['period_end'], 'period_basis': parsed and parsed['period_basis'],
                  'headline': parsed and parsed['headline'], 'classifier_version': V.CLASSIFIER_VERSION}
        evidence = json.dumps({'source_doc': row['source_doc'], 'doc_sha256': row.get('doc_sha256'),
                              'source_integrity': 'verified' if valid else 'missing_or_corrupt', 'is_results': parsed is not None,
                              'period_end': str(parsed and parsed['period_end']), 'period_basis': parsed and parsed['period_basis']}, sort_keys=True)
        prior = by_accession.get(row['accession']); result['document_evidence'] = [*(prior.get('document_evidence', []) if prior else []), evidence]
        if prior is None or (not prior['is_results'] and result['is_results']): by_accession[row['accession']] = result
        else:
            prior['document_evidence'] = result['document_evidence']
            if prior['is_results'] and result['is_results']:
                if prior['period_basis'] == 'conflicting_exhibits': continue
                if prior['period_end'] is not None and result['period_end'] is not None and prior['period_end'] != result['period_end']:
                    prior.update(period_end=None, period_basis='conflicting_exhibits')
                elif prior['period_end'] is None and result['period_end'] is not None:
                    prior.update(period_end=result['period_end'], period_basis=result['period_basis'], headline=result['headline'])
    return list(by_accession.values())


def write_rows(path, rows, schema):
    pq.write_table(pa.Table.from_pylist(rows, schema=schema), path, compression='zstd')


def calendar_replay(con, lake, releases, fpi, target):
    filings = A.sql([lake/'sec_filings/filings.parquet']); v1 = A.sql([lake/'earnings_calendar/announcements.parquet'])
    calendar_dates = [r[0] for r in con.execute('SELECT session_date FROM '+A.sql([lake/'calendar.parquet'])+' ORDER BY 1').fetchall()]
    calendar, _ = EC.build_calendar(calendar_dates)
    forms = ','.join("'"+f+"'" for f in V.PERIODIC_FORMS)
    con.execute(f"CREATE OR REPLACE TEMP TABLE t17_fpi_ciks AS SELECT DISTINCT cik FROM {filings} WHERE form IN ('20-F','40-F','6-K') AND filing_date>=DATE '{V.START_PERIODIC}'")
    parts = [f"""SELECT a.cik,a.accession,a.form,a.items,a.filing_date,a.event_date,a.announcement_utc,a.available_at,
        a.acceptance_clock,a.vintage_risk,r.period_end AS fiscal_period_end,NULL AS fiscal_period_form,
        '8k_202' AS source,r.period_basis,CAST(NULL AS VARCHAR) AS headline FROM {v1} a LEFT JOIN {A.sql([releases])} r USING(cik,accession)""",
        f"""SELECT cik,accession,form,NULL,filing_date,report_date,acceptance_utc,available_at,acceptance_clock,
        vintage_risk,report_date,form,'periodic_report','report_date',NULL FROM {filings}
        WHERE form IN ({forms}) AND NOT is_amendment AND filing_date>=DATE '{V.START_PERIODIC}' AND report_date IS NOT NULL"""]
    if fpi is not None:
        parts.append(f"""SELECT r.cik,r.accession,f.form,NULL,f.filing_date,f.report_date,f.acceptance_utc,f.available_at,
        f.acceptance_clock,f.vintage_risk,r.period_end,NULL,'6k_results',r.period_basis,r.headline FROM {A.sql([fpi])} r
        JOIN {filings} f USING(cik,accession) WHERE r.is_results""")
    cursor = con.execute(f"SELECT u.*,(u.cik IN (SELECT cik FROM t17_fpi_ciks)) AS is_fpi FROM ({' UNION ALL '.join(parts)}) u ORDER BY u.cik,u.announcement_utc,u.accession")
    columns = [d[0] for d in cursor.description]
    with pq.ParquetWriter(target, V.OUT_SCHEMA, compression='zstd') as writer:
        current = None; group = []
        while batch := cursor.fetchmany(4096):
            for values in batch:
                item = dict(zip(columns, values))
                if group and item['cik'] != current:
                    writer.write_table(pa.Table.from_pylist(V._merge_cik(group, calendar), schema=V.OUT_SCHEMA))
                    group = []
                current = item['cik']; group.append(item)
        if group: writer.write_table(pa.Table.from_pylist(V._merge_cik(group, calendar), schema=V.OUT_SCHEMA))


def check_parse_receipt(receipt, rows, sample):
    reasons = Counter(r['reason'] for r in rows)
    missing = sum(r.get('_missing_objects',0) for r in rows)
    if missing: reasons['missing_or_corrupt_object'] += missing
    expected = {'rows': len(rows),
        'qualified_any': sum(any(r.get(f) is not None for f in E.FIELDS) for r in rows),
        'qualified_all': sum(all(r.get(f) is not None for f in E.FIELDS) for r in rows),
        'reasons': dict(reasons),
        'precision_sample': {'rows':len(sample),
            'selection':'lowest SHA256(cik/accession/t9-precision-200-v1), all original 2.02 releases 2020-2025, independent of extraction success',
            'key_sha256':hashlib.sha256('\n'.join(str(r['cik'])+'/'+r['accession'] for r in sample).encode()).hexdigest(),
            'manual_reviewed':0}}
    if any(receipt.get(k) != v for k,v in expected.items()):
        raise ValueError('native parse/source population counters differ')


def parse_unit_sources(con,lake,catalog,blobs,manifest,dependencies):
    receipt=manifest['receipt'];code=C.code_identity('ea_actuals','ea_publication','events_sources','common')
    run_key=hashlib.sha256(json.dumps({'code':{k:v['sha256_lf'] for k,v in code.items() if isinstance(v,dict)},
        'sec_filings':manifest['input_manifests_sha256']['sec_filings']['sha256'],
        'catalog':C.sha256_file(ES.catalog_path()),'version':E.VERSION},sort_keys=True).encode()).hexdigest()
    if receipt['run_key']!=run_key or receipt['catalog_sha256']!=C.sha256_file(ES.catalog_path()):
        raise ValueError('native parser code/source run key differs')
    rows=parsed_rows(con,lake,catalog,blobs);by_key=defaultdict(list)
    for document in catalog:
        if document['form']=='8-K' and not document['is_amendment']:
            by_key[(document['cik'],document['accession'])].append(document)
    count=0
    for row in rows:
        seen=set()
        for document in by_key[(row['cik'],row['accession'])]:
            sha=document['doc_sha256'];blob=blobs.get(sha)
            if sha in seen or blob is None: continue
            seen.add(sha)
            identity={'run_key':run_key,'cik':row['cik'],'accession':row['accession'],
                'filing_date':str(row['filing_date']),'available_at':str(row.get('source_available_at')),
                'acceptance_clock':row.get('acceptance_clock'),'source_doc':document['source_doc'],'doc_sha256':sha}
            key=hashlib.sha256(json.dumps(identity,sort_keys=True).encode()).hexdigest()
            path=lake/'_tmp/ea_actuals/parse_units'/run_key/key[:2]/(key+'.json')
            unit=C.read_json(path)
            if unit.get('status')!='complete' or unit.get('identity')!=identity or unit.get('result')!=A.normalize(E.parse_release(blob,row['filing_date'])):
                raise ValueError('native parse unit identity/result differs from retained original document')
            dependencies[str(path.resolve())]=A.pin(path);count+=1
    counters=receipt['parse_units']
    if counters.get('cache_hits',0)+counters.get('new_units',0)!=count:
        raise ValueError('native full parse-unit population counters differ')


def replay(name, spec, con, source_only=False):
    lake, generation, manifest, dependencies = publication(name, spec)
    phase = manifest['phase']
    catalog, blobs = documents(lake, dependencies)
    for pattern in ('sec_filings/filings.parquet', 'sec_filings/eight_k_items.parquet', 'calendar.parquet'):
        dependencies.update(A.selected(lake, pattern))
    fpi = lake/'_tmp/events/fpi_releases.parquet'
    if name == 'earnings_calendar_v2':
        dependencies.update(A.selected(lake, 'earnings_calendar/announcements.parquet'))
        dependencies.update(A.selected(lake,'panel/year=*/*.parquet'))
        if fpi.exists(): dependencies.update(A.selected(lake, '_tmp/events/fpi_*'))
        parse_manifest_path=Path(manifest['input_manifests_sha256']['ea_actuals/releases_manifest.json']['path'])
        parse_manifest=C.read_json(parse_manifest_path)
        if parse_manifest.get('phase')!='parsed': raise ValueError('calendar requires complete native parse lineage')
        parse_unit_sources(con,lake,catalog,blobs,parse_manifest,dependencies)
    parse = None
    if name=='ea_actuals' and phase=='parsed': parse_unit_sources(con,lake,catalog,blobs,manifest,dependencies)
    if name == 'ea_actuals' and phase in ('features','validated'):
        parse = A.member(lake/'ea_actuals', manifest['parse_generation'])
        if A.pin(parse/'manifest.json')['sha256'] != manifest['parse_lineage']['sha256']:
            raise ValueError('immutable feature parse lineage differs')
        parsed = C.read_json(parse/'manifest.json')
        calendar_manifest = C.read_json(lake/'earnings_calendar_v2/manifest.json')
        if calendar_manifest.get('phase') != 'calendar': raise ValueError('native feature calendar phase differs')
        if calendar_manifest['input_manifests_sha256']['ea_actuals/releases_manifest.json']['sha256'] != manifest['parse_lineage']['sha256']:
            raise ValueError('native feature calendar and immutable parse snapshots incompatible')
        if parsed['input_manifests_sha256']['sec_filings']['sha256'] != manifest['input_manifests_sha256']['sec_filings']['sha256']:
            raise ValueError('native parse and feature SEC filing snapshots incompatible')
        if parsed.get('phase') != 'parsed': raise ValueError('feature lineage must be parsed phase')
        dependencies[str(parse/'manifest.json')] = A.pin(parse/'manifest.json')
        dependencies.update(A.files(lake/'ea_actuals', parsed))
        parse_unit_sources(con,lake,catalog,blobs,parsed,dependencies)
        for pattern in ('panel/year=*/*.parquet', 'prices/year=*/*.parquet'):
            dependencies.update(A.selected(lake, pattern))
        dependencies.update(A.selected(lake, 'earnings_calendar_v2/manifest.json'))
        dependencies.update(A.files(lake/'earnings_calendar_v2', C.read_json(lake/'earnings_calendar_v2/manifest.json')))
    audited = None
    if name == 'ea_actuals' and phase == 'validated':
        if manifest.get('audit_interface_version') != A.VERSION:
            raise ValueError('legacy flat financial audit cannot qualify final immutable sources; native audit integration required')
        from . import tier1_native_readers as T
        audited = T.admit('standardized_v11',spec['audit_standardized'],con)
        audit_source=manifest['audit_source_snapshot']
        if A.digest(audit_source)!=manifest['audit_source_snapshot_sha256'] or audit_source['standardized_manifest']!=spec['audit_standardized']['manifest_pin']:
            raise ValueError('native audit source snapshot differs')
        A.check_pins(audit_source['dependencies'])
        dependencies.update(audit_source['dependencies'])
        expected_inputs={str(Path(p).relative_to(lake)):{'path':p,'sha256':v['sha256']} for p,v in audit_source['dependencies'].items() if Path(p).is_relative_to(lake) and Path(p).name.endswith('manifest.json')}
        if expected_inputs!=manifest['audit_input_manifests_sha256'] or manifest['qualified_fundamental_leaf']!=str(audited['products']['qualified_fundamental_leaf']):
            raise ValueError('native audit qualified final financial/source binding differs')
        dependencies.update(spec['audit_standardized']['dependency_pins'])
        for pattern in ('universe/year=*/*.parquet','_tmp/panel_member/year=*.parquet'):
            dependencies.update(A.selected(lake,pattern))
    # All declared dependency manifests must have complete inventories; the
    # exact native row selectors below consume only these independently pinned bytes.
    A.check_pins(spec['dependency_pins'], dependencies)
    source = {'version': A.VERSION, 'stage': name, 'phase': phase, 'dependencies': dependencies,
              'documents': blobs.descriptors}
    if source_only:
        A.check_pins(spec['dependency_pins'], dependencies)
        return source
    if A.digest(source) != spec['snapshot_sha256']:
        raise ValueError('independent complete Task9 source snapshot differs')
    with A.scratch(lake, 'earnings-') as target:
        rows = parsed_rows(con, lake, catalog, blobs)
        releases = target/'releases.parquet'; write_rows(releases, rows, E.RELEASE_SCHEMA)
        sample = sorted((r for r in rows if 2020<=r['filing_date'].year<=2025), key=lambda r: hashlib.sha256(
            (str(r['cik'])+'/'+r['accession']+'/t9-precision-200-v1').encode()).hexdigest())[:200]
        if name == 'ea_actuals' and phase == 'parsed':
            write_rows(target/'precision_sample.parquet', sample, E.RELEASE_SCHEMA)
            for product in PHASES[name][phase]: A.compare(con, generation/product, target/product)
            check_parse_receipt(manifest['receipt'], rows, sample)
        elif name == 'earnings_calendar_v2':
            if fpi.exists():
                fpi_receipt=V.verify_fpi_parse()
                fpi_replayed=fpi_rows(catalog,blobs)
                counters={'documents':len(fpi_replayed),'results':sum(r['is_results'] for r in fpi_replayed),
                    'with_period':sum(r['period_end'] is not None for r in fpi_replayed),'text_date':sum(r['period_basis']=='text_date' for r in fpi_replayed)}
                if any(fpi_receipt.get(k)!=v for k,v in counters.items()) or manifest['fpi_parse']!=fpi_receipt:
                    raise ValueError('native FPI source population counters differ')
                write_rows(target/'fpi.parquet', fpi_replayed, V.FPI_SCHEMA)
                A.compare(con, fpi, target/'fpi.parquet')
            calendar_replay(con, lake, releases, target/'fpi.parquet' if fpi.exists() else None, target/'announcements.parquet')
            A.compare(con, generation/'announcements.parquet', target/'announcements.parquet')
            con.execute('CREATE OR REPLACE TEMP TABLE fpi_ciks AS SELECT * FROM t17_fpi_ciks')
            coverage=V.member_coverage(con,target/'announcements.parquet')
            per_year={str(y):dict(zip(('8k_202','6k_results','periodic_report','primary','fpi_rows'),map(int,values))) for y,*values in con.execute(
                'SELECT year(session_date),count(*) FILTER(WHERE source=\'8k_202\'),count(*) FILTER(WHERE source=\'6k_results\'),count(*) FILTER(WHERE source=\'periodic_report\'),count(*) FILTER(WHERE is_primary),count(*) FILTER(WHERE is_fpi) FROM '+A.sql([target/'announcements.parquet'])+" WHERE session_date>=DATE '2019-01-01' GROUP BY 1 ORDER BY 1").fetchall()}
            rows_count=pq.ParquetFile(target/'announcements.parquet').metadata.num_rows
            gate='PASS' if all(r['covered_share'] is not None and r['covered_share']>=.95 for y,r in coverage.items() if int(y)>=2020) else 'FAIL'
            if manifest['rows']!=rows_count or manifest['per_year_source']!=per_year or manifest['member_coverage']!=coverage or manifest['gate_status']['member_coverage']!=gate:
                raise ValueError('native calendar population/coverage gates differ')
        else:
            # Feature source is the separately pinned immutable parse generation,
            # not a mutable root Parquet or a complete parsed-phase feature claim.
            A.compare(con, parse/'releases.parquet', releases)
            check_parse_receipt(parsed['receipt'], rows, sample)
            groups = defaultdict(list)
            for row in rows: groups[row['cik']].append(row)
            features = [r for cik in sorted(groups) for r in E.sue_history(groups[cik])]
            write_rows(target/'ann_features.parquet', features, E.FEATURE_SCHEMA)
            A.compare(con, generation/'ann_features.parquet', target/'ann_features.parquet')
            filing = A.sql([lake/'sec_filings/filings.parquet'])
            lag_rows = C.copy_to_parquet(con, f"""WITH candidates AS (
            SELECT r.cik,r.accession,r.period_end,r.available_at AS announcement_available_at,f.accession AS filing_accession,
              f.available_at,date_diff('day',timezone('America/New_York',timezone('UTC',r.available_at))::DATE,
              timezone('America/New_York',timezone('UTC',f.available_at))::DATE) AS ea_lag_days,
              row_number() OVER(PARTITION BY r.cik,r.accession ORDER BY f.available_at,f.accession) rn
            FROM {A.sql([releases])} r JOIN {filing} f ON r.cik=f.cik AND r.period_end=f.report_date
              AND f.form IN ('10-Q','10-QT') AND NOT f.is_amendment AND f.available_at>=r.available_at
            WHERE r.available_at IS NOT NULL AND f.acceptance_clock NOT LIKE '%unresolved%'
              AND coalesce(f.vintage_risk,'') NOT LIKE '%unresolved%') SELECT * EXCLUDE(rn) FROM candidates WHERE rn=1""", target/'filing_lags.parquet')
            A.compare(con, generation/'filing_lags.parquet', target/'filing_lags.parquet')
            reaction = E.build_reactions(con, target, str(releases))
            A.compare(con, generation/'reaction_returns.parquet', target/'reaction_returns.parquet')
            recall = con.execute('SELECT year(filing_date),count(*),count(*) FILTER(WHERE saleq IS NOT NULL AND epsfiq IS NOT NULL AND niq IS NOT NULL),count(*) FILTER(WHERE available_at IS NULL) FROM '+A.sql([target/'ann_features.parquet'])+" WHERE filing_date BETWEEN DATE '2020-01-01' AND DATE '2025-12-31' GROUP BY 1 ORDER BY 1").fetchall()
            expected_receipt = {'rows':len(features), 'filing_lag_rows':lag_rows, 'recall':{str(y):{'releases':n,'all_actuals':a,'all_actuals_share':a/n,'unknown_clocks':u} for y,n,a,u in recall}, 'reaction_returns':reaction}
            if manifest['receipt'] != expected_receipt:
                raise ValueError('native reaction counters/window/clock evidence differs')
            if audited is not None:
                audit = audit_result(con,lake,generation,audited,manifest['audit_input_manifests_sha256'])
                if audit != C.read_json(generation/'validation.json') or manifest['validation'] != audit:
                    raise ValueError('original full earnings audit evidence differs')
                if any(manifest['gate_status'].get(k)!=v for k,v in audit['gate_status'].items()):
                    raise ValueError('original earnings audit gates differ')
    A.check_pins(spec['dependency_pins'], dependencies)
    if A.pin(generation/'manifest.json') != spec['manifest_pin']:
        raise ValueError('Task9 generation changed during replay')
    validation = {'stage': name, 'phase': phase, 'snapshot_sha256': A.digest(source),
                  'manifest': spec['manifest_pin'], 'checks': ['exact_native_source_replay', 'exact_schema', 'exact_typed_multiset',
                    'complete_phase_products', 'parse_lineage', 'original_clock_and_holdout_rules'],
                  'gate_status': manifest.get('gate_status', {}), 'source_quality_certified': False}
    return A.envelope(name, spec, generation, validation,
                      {p: generation/p for p in PHASES[name][phase]}, dependencies)


def audit_result(con, lake, features, standardized, inputs):
    """Original GC3/Task3 audit equations with the admitted immutable final leaf.

Only reader binding changes: original native coverage kernel, denominators,
cutoffs, all three populations, six years and precision gate remain intact.
"""
    from . import compustat_std as S
    out=Path(standardized['products']['fundq']).parent
    fund=Path(standardized['products']['qualified_fundamental_leaf'])
    S.member_ciks(con); S.coverage_gate(con,out)
    calendar_manifest=C.read_json(lake/'earnings_calendar_v2/manifest.json')
    ann=A.member(lake/'earnings_calendar_v2',calendar_manifest['products']['announcements.parquet'])
    con.execute('CREATE OR REPLACE TEMP TABLE t9_ann AS SELECT * FROM '+A.sql([ann])+" WHERE source='8k_202' AND is_primary AND announcement_utc IS NOT NULL AND available_at IS NOT NULL")
    con.execute('CREATE OR REPLACE TEMP TABLE t9_clocks AS SELECT DISTINCT cik,accession,clock_basis FROM '+A.sql([fund/'events.parquet']))
    rdq=con.execute("""WITH j AS (SELECT c.*, EXISTS(SELECT 1 FROM t9_ann a WHERE a.cik=c.cik
        AND abs(date_diff('day',a.fiscal_period_end,c.datadate))<=7
        AND timezone('America/New_York',timezone('UTC',a.available_at))::DATE <=
        CASE WHEN c.clock_utc IS NOT NULL AND fc.clock_basis='fsds_accepted_utc'
        THEN timezone('America/New_York',timezone('UTC',c.clock_utc))::DATE ELSE c.filed END) same_et_date,
        EXISTS(SELECT 1 FROM t9_ann a WHERE a.cik=c.cik AND abs(date_diff('day',a.fiscal_period_end,c.datadate))<=7
        AND a.available_at<=c.available_at) strict_timestamp FROM cov c LEFT JOIN t9_clocks fc USING(cik,accession)
        WHERE c.form IN ('10-Q','10-QT')) SELECT year,count(*),count(*) FILTER(WHERE same_et_date),
        count(*) FILTER(WHERE strict_timestamp) FROM j GROUP BY 1 ORDER BY 1""").fetchall()
    con.execute('CREATE OR REPLACE TEMP TABLE t9_cutoffs AS SELECT session_date,lag(session_date) OVER(ORDER BY session_date)::TIMESTAMP+INTERVAL 22 HOUR cutoff FROM '+A.sql([lake/'calendar.parquet']))
    coverage={}; feature=A.sql([features/'ann_features.parquet'])
    for stage,flags in (('panel',('member_equity',)),('universe',('univ_all_common','univ_r3000p'))):
        for year in range(2020,2026):
            files=sorted((lake/stage/f'year={year}').glob('*.parquet'))
            if not files: raise ValueError('missing final original audit population year: '+stage+'/'+str(year))
            source=A.sql(files)
            for flag in flags:
                n,e,r=con.execute(f"""WITH m AS (SELECT p.session_date,p.security_id,p.cik,c.cutoff
                    FROM {source} p JOIN t9_cutoffs c USING(session_date) WHERE {flag}),
                    j AS (SELECT m.*,f.period_end,f.sue_eps_ann,f.sue_rev_ann FROM m ASOF LEFT JOIN {feature} f
                    ON m.cik=f.cik AND m.cutoff>f.available_at) SELECT count(*),
                    count(*) FILTER(WHERE sue_eps_ann IS NOT NULL AND period_end<=session_date AND date_diff('day',period_end,session_date)<=400),
                    count(*) FILTER(WHERE sue_rev_ann IS NOT NULL AND period_end<=session_date AND date_diff('day',period_end,session_date)<=400) FROM j""").fetchone()
                issuers=con.execute(f"""WITH m AS (SELECT DISTINCT cik FROM {source} WHERE {flag} AND cik IS NOT NULL)
                    SELECT count(*),count(*) FILTER(WHERE EXISTS(SELECT 1 FROM {A.sql([ann])} a
                    WHERE a.cik=m.cik AND year(a.session_date)={year})) FROM m""").fetchone()
                coverage[f'{flag}/{year}']={'cells':n,'eps_sue_cells':e,'revenue_sue_cells':r,
                    'eps_sue_share':e/n if n else None,'revenue_sue_share':r/n if n else None,
                    'member_issuers':issuers[0],'calendar_covered_issuers':issuers[1],
                    'calendar_covered_share':issuers[1]/issuers[0] if issuers[0] else None}
    return {'input_integrity':inputs,'coverage':coverage,
        'rdq':{str(y):{'periods':n,'same_et_date':a,'same_et_date_share':a/n if n else None,
            'strict_timestamp':b,'strict_timestamp_share':b/n if n else None} for y,n,a,b in rdq},
        'rdq_rule':'Task3 first-reported member 10-Q/10-QT denominator; only source-backed primary 8-K 2.02; acceptance <= filing ET date, strict timestamp alongside',
        'gate_status':{'calendar_gc3':'PASS' if coverage and all(r['calendar_covered_share'] is not None and r['calendar_covered_share']>=.95 for r in coverage.values()) else 'FAIL',
            'sue_cells':'PASS' if coverage and all(r['eps_sue_share'] is not None and r['eps_sue_share']>=.9 for r in coverage.values()) else 'FAIL',
            'rdq_same_date':'PASS' if rdq and all(n and a/n>=.95 for _,n,a,b in rdq) else 'FAIL',
            'precision_200_manual':'FAIL_unreviewed'}}


def publish_audit(spec, standardized_spec, con, expected_sources_sha256):
    """Retained immutable validated phase; no original gates are waived."""
    from . import tier1_native_readers as T
    import shutil
    admitted=T.admit('ea_actuals',spec,con)
    if spec['required_phase']!='features': raise ValueError('native audit requires complete feature publication')
    standardized=T.admit('standardized_v11',standardized_spec,con)
    lake=A.root(spec);feature=Path(admitted['products']['ann_features.parquet']).parent
    dependencies={**spec['dependency_pins'],**standardized_spec['dependency_pins']}
    for pattern in ('universe/year=*/*.parquet','_tmp/panel_member/year=*.parquet'):
        dependencies.update(A.selected(lake,pattern))
    source={'version':A.VERSION,'dependencies':dependencies,'feature_manifest':spec['manifest_pin'],
            'standardized_manifest':standardized_spec['manifest_pin']}
    if A.digest(source)!=expected_sources_sha256: raise ValueError('independent complete audit source digest differs')
    A.check_pins(dependencies)
    inputs={str(Path(p).relative_to(lake)):{'path':p,'sha256':v['sha256']} for p,v in dependencies.items()
        if Path(p).is_relative_to(lake) and Path(p).name.endswith('manifest.json')}
    result=audit_result(con,lake,feature,standardized,inputs)
    work=P.create('ea_actuals')
    for product in PHASES['ea_actuals']['features']: shutil.copy2(feature/product,work/product)
    C.write_json_atomic(work/'validation.json',result)
    manifest=C.read_json(feature/'manifest.json')
    manifest.update(validation=result,audit_input_manifests_sha256=inputs,audit_interface_version=A.VERSION,
        audit_source_snapshot_sha256=A.digest(source),audit_source_snapshot=source,qualified_fundamental_leaf=str(standardized['products']['qualified_fundamental_leaf']))
    manifest['gate_status'].update(result['gate_status'])
    A.check_pins(dependencies)
    T.admit('ea_actuals',spec,con);T.admit('standardized_v11',standardized_spec,con)
    A.check_pins(dependencies)
    return P.publish('ea_actuals',work,'validated',manifest,PHASES['ea_actuals']['validated'])
