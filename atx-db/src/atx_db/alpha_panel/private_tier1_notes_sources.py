"""R26 bounded full native notes replay and R12 fixed-cell admission.

No frozen producer is modified. The source plan and review are independently
expected inputs; neither published labels nor a caller PASS certify a cell.
"""
from __future__ import annotations

import contextlib
import datetime as dt
import gzip
import importlib
import json
import math
import os
import re
import tempfile
import sys
import types
import zipfile
from pathlib import Path

from . import common as C, fund_notes as N, notes_fetch as S
from . import fund_notes_structure as FS, fund_notes_semantic as SEM
from . import golden_sources as G, private_tier1_family_sources as F

VERSION = 'tier1-notes-sources-v1'
FIELDS = ('bseg_n', 'bseg_rev_hhi', 'foreign_sales_share', 'tax_dtl')
SOURCE_FILES = ('sub.parquet', 'txt_dei.parquet', 'num_dei.parquet', 'num_items.parquet', 'dim.parquet')
PRODUCTS = ('segments.parquet', 'segment_recon.parquet', 'notes_wide.parquet',
            'segment_features.parquet', 'segment_census.parquet', 'segment_structure.parquet')
RAW_ROOT = Path('data/raw/sec_notes_t14_pilot')
TABLES = ('src', 'reg', 'regt', 'sub', 'business_census', 'business_census_original',
          'structure_evidence', 'cur', 'ny', 'dimy', 'miss', 'recon_ov')


def _json(path, maximum=16*1024*1024):
    return json.loads(F._blob(path, maximum))


def code_pins():
    modules = [C, N, S, FS, SEM, G, F, importlib.import_module(__name__)]
    modules += [importlib.import_module('.'+name, __package__) for name in
                ('fund_notes_disclosure', 'shortflow_common', 'privatefund_runtime_resources')]
    # Every actual native import is independently reviewed, including source
    # streaming, XML/semantic parsing, clock conversion and admission inventory.
    pins = {str(Path(m.__file__).resolve()): F.pin(m.__file__) for m in modules}
    inventory = Path(G.__file__).resolve().parents[3]/'docs/datasets/golden_notes_inventory.json'
    pins[str(inventory)] = F.pin(inventory)
    from . import distributions_native as runtime
    from . import distributions_qualification as clocks
    pins[str(Path(runtime.__file__).resolve())]=F.pin(runtime.__file__)
    pins[str(Path(clocks.__file__).resolve())]=F.pin(clocks.__file__)
    for path in runtime.code_pins():
        if not path.startswith('runtime:') and Path(path).suffix.lower() in ('.pyd', '.dll'):
            pins[str(F.absolute(path))] = F.pin(path)
    for path in runtime.code_pins():
        if not path.startswith('runtime:') and 'tzdata' in Path(path).parts:
            pins[str(F.absolute(path))] = F.pin(path)
    return pins


def _native_runtime():
    import duckdb
    import pyarrow
    from . import distributions_qualification as clocks
    def stable(value):
        if isinstance(value,types.CodeType):
            return {name:stable(getattr(value,name)) for name in ('co_code','co_consts','co_names','co_varnames',
                'co_freevars','co_cellvars','co_argcount','co_posonlyargcount','co_kwonlyargcount',
                'co_nlocals','co_stacksize','co_flags','co_exceptiontable')}
        if isinstance(value,bytes):return ['bytes',value.hex()]
        if isinstance(value,tuple):return ['tuple',[stable(item) for item in value]]
        return [type(value).__name__,repr(value)]
    modules=(N,S,FS,SEM,G,F,importlib.import_module(__name__))
    loaded={}
    for module in modules:
        for name,value in vars(module).items():
            if getattr(value,'__module__',None)!=module.__name__:
                continue
            methods=vars(value).items() if isinstance(value,type) else [(name,value)]
            for method,operation in methods:
                code=getattr(operation,'__code__',None)
                if code:
                    loaded[module.__name__+'.'+name+'.'+method]=F.digest(stable(code))
    return {'python':sys.version,'duckdb':duckdb.__version__,'pyarrow':pyarrow.__version__,
            'loaded_code':loaded,'clock_loaded_code':clocks.loaded_code_pins(),'native_constants_sha256':F.digest({'items':N.ITEMS,'segments':N.SEGMENT_METRICS,
                'columns':S.COLUMNS,'kinds':S.KINDS,'forms':S.PERIODIC_FORMS,'curated':FS.CORROBORATED})}


def _check(pins):
    for path, expected in pins.items():
        if F.pin(F.absolute(path)) != expected:
            raise ValueError('independently expected notes dependency changed: '+path)


def _binding(spec):
    return {k: spec[k] for k in ('interface_version', 'root', 'generation', 'manifest_pin', 'native')}


def _prepare(spec, con):
    if spec.get('interface_version') != VERSION or set(spec['native']) != {'plan_path', 'expected_plan_sha256'}:
        raise ValueError('fixed reviewed native notes version/plan required')
    root, generation = F.absolute(spec['root']), F.absolute(spec['generation'])
    if not generation.is_relative_to(root):
        raise ValueError('native notes generation outside independent root')
    F._guard(con, root)
    _check(spec['dependency_pins'])
    code = code_pins()
    if any(spec['dependency_pins'].get(path) != expected for path, expected in code.items()):
        raise ValueError('complete independently reviewed notes code inventory required')
    path = F.absolute(spec['native']['plan_path'])
    plan = _json(path)
    if F.pin(path)['sha256'] != spec['native']['expected_plan_sha256']:
        raise ValueError('independent native notes source plan differs')
    if plan.get('schema') != 'atx.tier1-notes-source-plan/v1':
        raise ValueError('unsupported fixed native notes source plan')
    review_path = F.absolute(spec['independent_review']['path'])
    review = _json(review_path)
    if F.pin(review_path)['sha256'] != spec['independent_review']['expected_sha256'] or review != {
        'schema': 'atx.tier1-notes-review/v1', 'status': 'scoped_pass', 'interface_version': VERSION,
        'binding_sha256': F.digest(_binding(spec)), 'code_pins': code}:
        raise ValueError('native notes independent review pending/stale')
    if F.pin(generation/'manifest.json') != spec['manifest_pin']:
        raise ValueError('independent native notes manifest differs')
    source_root = F.absolute(plan['source_root'])
    source_manifest = F.absolute(plan['source_manifest'])
    manifest = _json(source_manifest)
    data_sets = manifest['data_sets']
    if set(plan['archives']) != set(data_sets):
        raise ValueError('complete original FSDS source inventory required')
    years = plan['years']
    if years != sorted(set(years)) or not years:
        raise ValueError('complete sorted native filing years required')
    names = set(PRODUCTS) | {f'items/year={year}/items.parquet' for year in years}
    if set(plan['native_products']) != names:
        raise ValueError('complete original native notes products required')
    products = {name: F.absolute(generation/name) for name in names}
    if any(not p.is_relative_to(generation) for p in products.values()):
        raise ValueError('native notes product escape')
    rawroot = F.absolute(plan['rawroot'])
    if rawroot != RAW_ROOT.resolve(strict=True) or F.absolute(plan['raw_receipts']) != rawroot/'receipts.jsonl':
        raise ValueError('unchanged native notes original proof path is binding')
    dependencies = {**code, str(path): F.pin(path), str(review_path): F.pin(review_path),
                    str(generation/'manifest.json'): spec['manifest_pin'], str(source_manifest): F.pin(source_manifest),
                    str(F.absolute(plan['source_receipts'])): F.pin(plan['source_receipts']),
                    str(rawroot/'receipts.jsonl'): F.pin(rawroot/'receipts.jsonl'),
                    str(F.absolute(plan['curated_inventory'])): F.pin(plan['curated_inventory'])}
    for name, p in products.items():
        if F.pin(p) != plan['native_products'][name]:
            raise ValueError('native notes product identity differs')
        dependencies[str(p)] = F.pin(p)
    # Native cached_structure reads the full exact ledger. Bound it and bind every
    # actual successful primary/linkbase body, not only the nine curated matches.
    ledger = F._blob(rawroot/'receipts.jsonl', 10*1024*1024).splitlines()
    if len(ledger)>1000:
        raise ValueError('native pilot proof ledger exceeds bounded original inventory')
    units={}
    for line in ledger:
        record = json.loads(line)
        if record.get('http_status') != 200 or record.get('kind') not in ('primary', 'linkbase'):
            continue
        body = (rawroot/record['path']).resolve(strict=True)
        if not body.is_relative_to(rawroot):
            raise ValueError('original notes document path escape')
        with gzip.open(body, 'rb') as stream:
            payload = stream.read(40*1024*1024+1)
        if len(payload)>40*1024*1024 or __import__('hashlib').sha256(payload).hexdigest()!=record['sha256']:
            raise ValueError('bounded original notes document digest differs')
        prior=units.get(record['adsh'],(0,0))
        units[record['adsh']]=(prior[0]+1,prior[1]+len(payload))
        if units[record['adsh']][0]>16 or units[record['adsh']][1]>64*1024*1024:
            raise ValueError('native filing original proof working set exceeds bounded64MiB/16documents')
        dependencies[str(body)] = F.pin(body)
    for key, record in plan['archives'].items():
        if set(record) != {'original', 'receipt', 'parts'} or set(record['parts']) != set(SOURCE_FILES):
            raise ValueError('complete original five-product source partition required')
        original, receipt = F.absolute(record['original']), F.absolute(record['receipt'])
        transport = _json(receipt)
        source = data_sets[key]
        if (not source['url'].startswith('https://www.sec.gov/files/dera/data/financial-statement-notes-data-sets/')
                or S.data_set(source['url'])['key']!=key):
            raise ValueError('fixed original SEC FSDS archive identity required')
        if (transport.get('url') != source['url'] or transport.get('http_status') != 200
                or transport.get('error') or transport.get('sha256') != source['sha256']
                or transport.get('bytes') != source['bytes'] or transport.get('status') not in ('parsed', 'downloaded')
                or F.pin(original) != {'sha256':source['sha256'], 'bytes':source['bytes']}):
            raise ValueError('original FSDS archive/receipt/source manifest identity differs')
        dependencies.update({str(original):F.pin(original), str(receipt):F.pin(receipt)})
        for name, expected in record['parts'].items():
            part = F.absolute(source_root/'parts'/f'source={key}'/name)
            if F.pin(part) != expected:
                raise ValueError('original source extracted product differs')
            dependencies[str(part)] = expected
    if dependencies != spec['dependency_pins']:
        raise ValueError('independently expected complete native notes dependency inventory differs')
    return plan, products, dependencies


def _table(con, name, schema):
    import pyarrow as pa
    con.register('_notes_empty', pa.Table.from_batches([], schema=schema))
    con.execute(f'CREATE TEMP TABLE {name} AS SELECT * FROM _notes_empty')
    con.unregister('_notes_empty')


def _insert(con, name, cols, schema):
    import pyarrow as pa
    con.register('_notes_batch', pa.table([cols[f.name] for f in schema], schema=schema))
    try:
        con.execute(f'INSERT INTO {name} SELECT * FROM _notes_batch')
    finally:
        con.unregister('_notes_batch')


def _compare(con, expected_sql, actual):
    query = f"SELECT * FROM read_parquet('{str(actual).replace(chr(39),chr(39)*2)}',hive_partitioning=false)"
    schema = con.execute('DESCRIBE ('+expected_sql+')').fetchall()
    if schema != con.execute('DESCRIBE ('+query+')').fetchall():
        raise ValueError('full native notes physical schema differs: '+str(actual))
    expected_sql='SELECT * FROM ('+expected_sql+')'
    count = con.execute(f'SELECT count(*) FROM (({expected_sql} EXCEPT ALL {query}) UNION ALL ({query} EXCEPT ALL {expected_sql}))').fetchone()[0]
    if count:
        raise ValueError('full duplicate-preserving native notes replay differs: '+str(actual))


def replay_source(con, key, original, parts):
    """Native streaming/casts; SQL relations preserve every original source row."""
    import pyarrow as pa
    source = lambda n: pa.array([key]*n, pa.string())
    schemas = {t:S.out_schema(t) for t in ('sub','dim','num','txt')}
    schemas['sub'] = S.out_schema('sub', (('accepted_et_naive',pa.timestamp('us')),('accepted_utc',pa.timestamp('us',tz='UTC'))))
    for t, schema in schemas.items():
        _table(con, 'ns_'+t, schema)
    stats, unparsed = {}, {}
    try:
        with zipfile.ZipFile(original) as archive:
            members={Path(n.replace('\\','/')).name.lower():n for n in archive.namelist() if not n.endswith('/')}
            if not set(S.MEMBERS)<=set(members) or len(members)!=len([n for n in archive.namelist() if not n.endswith('/')]):
                raise ValueError('complete unambiguous original FSDS ZIP members required')
            for t in ('sub','dim','txt','num'):
                st, bad = {}, {}
                for batch in S.stream_member(archive, members[t+'.tsv'], t, st):
                    if t=='sub':
                        cols=S.cast_batch(batch.drop_columns(['accepted']), t, bad)
                        naive, utc=S.accepted_clocks(batch.column('accepted').to_pylist())
                        cols.update(accepted_et_naive=pa.array(naive,pa.timestamp('us')),
                                    accepted_utc=pa.array(utc,pa.timestamp('us',tz='UTC')))
                    else:
                        cols=S.cast_batch(batch,t,bad)
                    cols['source']=source(batch.num_rows)
                    _insert(con,'ns_'+t,cols,schemas[t])
                stats[t+'.tsv'],unparsed[t]=st,bad
        periodic=','.join(repr(x) for x in S.PERIODIC_FORMS)
        axes='(^|;)('+'|'.join(S.SEGMENT_AXES)+')='
        con.execute(f"CREATE TEMP TABLE ns_used AS SELECT DISTINCT dimh FROM ns_txt WHERE starts_with(version,'dei/') UNION SELECT DISTINCT dimh FROM ns_num WHERE starts_with(version,'dei/') OR (adsh IN(SELECT adsh FROM ns_sub WHERE form IN({periodic})) AND ((version<>adsh AND regexp_matches(tag,'{S.ITEM_TAG_RE}')) OR dimh IN(SELECT dimhash FROM ns_dim WHERE regexp_matches(segments,'{axes}'))))")
        queries={'sub.parquet':'SELECT * FROM ns_sub',
                 'txt_dei.parquet':"SELECT * FROM ns_txt WHERE starts_with(version,'dei/')",
                 'num_dei.parquet':"SELECT * FROM ns_num WHERE starts_with(version,'dei/')",
                 'num_items.parquet':f"SELECT * FROM ns_num WHERE adsh IN(SELECT adsh FROM ns_sub WHERE form IN({periodic})) AND ((version<>adsh AND regexp_matches(tag,'{S.ITEM_TAG_RE}')) OR dimh IN(SELECT dimhash FROM ns_dim WHERE regexp_matches(segments,'{axes}')))",
                 'dim.parquet':f"SELECT * FROM ns_dim WHERE dimhash IN(SELECT dimh FROM ns_used WHERE dimh IS NOT NULL AND dimh<>'{S.NO_DIM}')"}
        counts={}
        for filename, sql in queries.items():
            _compare(con,sql,parts/filename)
            counts[filename]=con.execute('SELECT count(*) FROM ('+sql+')').fetchone()[0]
        return {'members':stats,'unparsed':unparsed,'rows':counts}
    finally:
        for table in ('ns_used','ns_sub','ns_dim','ns_txt','ns_num'):
            con.execute('DROP TABLE IF EXISTS '+table)


def item_sql(con, parts, years):
    """Small original connection-taking projection authorized by controller R26.

    This is only build436–519's item projection. All subsequent economics call
    unchanged native SQL/structure/semantic kernels.
    """
    con.execute('CREATE TEMP TABLE src(source VARCHAR,ord INTEGER)')
    keys=sorted([p.name.split('=',1)[1] for p in parts.glob('source=*')],key=S.order_key)
    con.executemany('INSERT INTO src VALUES (?,?)',[(k,i) for i,k in enumerate(keys)])
    con.execute('CREATE TEMP TABLE reg(tag VARCHAR,item VARCHAR,item_group VARCHAR,kind VARCHAR,prio INTEGER)')
    con.executemany('INSERT INTO reg VALUES (?,?,?,?,?)',N._registry_rows())
    con.execute('CREATE TEMP TABLE regt AS SELECT tag,first(item ORDER BY prio,item) item,first(item_group ORDER BY prio,item) item_group FROM reg GROUP BY tag')
    forms=','.join(repr(f) for f in S.PERIODIC_FORMS)
    source=(parts/'source=*'/'sub.parquet').as_posix().replace("'","''")
    con.execute(f"CREATE TEMP TABLE sub AS SELECT s.adsh,s.cik,s.form,s.period,s.fy,s.fp,s.filed,s.source,s.countryba,coalesce(s.accepted_utc,CAST(s.filed AS TIMESTAMP) AT TIME ZONE 'UTC'+INTERVAL {N.FC1_HOURS} HOUR) available_at,CASE WHEN s.accepted_utc IS NULL THEN 'filed_plus_46h' ELSE 'accepted_utc' END clock_basis FROM read_parquet('{source}',hive_partitioning=false) s JOIN src USING(source) WHERE s.form IN({forms}) QUALIFY row_number() OVER(PARTITION BY s.adsh ORDER BY src.ord)=1")
    for year in years:
        keys=[r[0] for r in con.execute('SELECT DISTINCT source FROM sub WHERE year(filed)=? ORDER BY 1',[year]).fetchall()]
        nums=[(parts/f'source={key}'/'num_items.parquet').as_posix() for key in keys]
        dims=[(parts/f'source={key}'/'dim.parquet').as_posix() for key in keys]
        if not nums:
            raise ValueError('native filing year has no independently inventoried source')
        con.execute(f"CREATE OR REPLACE TEMP TABLE ny AS SELECT x.* FROM read_parquet({repr(nums)},hive_partitioning=false) x JOIN sub s ON s.adsh=x.adsh AND s.source=x.source WHERE year(s.filed)={year} AND x.value IS NOT NULL AND ((x.version<>x.adsh AND x.tag IN(SELECT tag FROM regt)) OR(x.version=x.adsh AND regexp_full_match(x.tag,'{N.CUSTOM_REVENUE_RE}'))) QUALIFY row_number() OVER(PARTITION BY x.adsh,x.tag,x.version,x.ddate,x.qtrs,x.uom,x.dimh,coalesce(x.coreg,'') ORDER BY x.iprx)=1")
        con.execute(f'CREATE OR REPLACE TEMP TABLE dimy AS SELECT d.dimhash,d.segments,d.source FROM read_parquet({repr(dims)},hive_partitioning=false) d SEMI JOIN(SELECT DISTINCT dimh,source FROM ny) u ON u.dimh=d.dimhash AND u.source=d.source')
        b,g,c=N.AXES['business'],N.AXES['geographic'],N.CUSTOMER_AXIS
        sql=f"""SELECT s.cik,n.adsh,s.form,s.period,s.fy,s.fp,s.filed,s.available_at,s.clock_basis,
          coalesce(r.item_group,'segment') item_group,coalesce(r.item,'revenue') item,n.tag,n.version=n.adsh is_custom,n.segments,n.dimn,
          CASE WHEN n.segments LIKE '%{b}=%' THEN 'business' WHEN n.segments LIKE '%{g}=%' THEN 'geographic' WHEN n.segments LIKE '%{c}=%' THEN 'customer' END axis_kind,
          coalesce(nullif(regexp_extract(n.segments,'(^|;){b}=([^;]*)',2),''),nullif(regexp_extract(n.segments,'(^|;){g}=([^;]*)',2),''),nullif(regexp_extract(n.segments,'(^|;){c}=([^;]*)',2),'')) member,
          nullif(regexp_extract(n.segments,'(^|;)ConsolidationItems=([^;]*)',2),'') consolidation_member,
          n.ddate,CAST(n.ddate-CAST(round(coalesce(n.datp,0)) AS INTEGER) AS DATE) end_date,n.qtrs,n.value,n.uom,n.coreg,n.source
          FROM(SELECT x.*,d.segments FROM ny x LEFT JOIN dimy d ON d.dimhash=x.dimh AND d.source=x.source) n
          JOIN sub s ON s.adsh=n.adsh AND s.source=n.source LEFT JOIN regt r ON r.tag=n.tag AND n.version<>n.adsh
          WHERE(r.tag IS NOT NULL AND(r.item_group<>'segment' OR n.segments IS NULL OR n.segments LIKE '%{b}=%' OR n.segments LIKE '%{g}=%' OR n.segments LIKE '%ConsolidationItems=%' OR(r.item='revenue' AND n.segments LIKE '%{c}=%'))) OR(r.tag IS NULL AND n.segments LIKE '%{b}=%')
          ORDER BY s.cik,n.adsh,item_group,item,n.tag,n.ddate,n.qtrs,n.segments NULLS FIRST,n.uom,n.coreg NULLS FIRST,n.value"""
        yield year,sql


def _copy(con, sql, path):
    C.copy_to_parquet(con,sql,path,row_group_size=4096)
    return path.as_posix()


def _concat(paths):
    return 'SELECT * FROM read_parquet('+repr([p.as_posix() for p in paths])+',hive_partitioning=false)'


def _overlay(con, items, segments, strict, scratch):
    """Call the unchanged source/semantic overlay one filing at a time."""
    import pyarrow.parquet as pq
    cursor=con.cursor().execute(f"SELECT adsh FROM read_parquet('{strict}') WHERE multi_segment AND n_revenue_members>=1 AND consolidated_revenue IS NOT NULL AND consolidated_revenue<>0 AND recon_class NOT IN('direct','with_reconciling') ORDER BY adsh")
    target=scratch/'overlay.parquet'
    with pq.ParquetWriter(target,N._overlay_schema(),compression='zstd') as writer:
        index=0
        while row:=cursor.fetchone():
            adsh,=row
            unit=scratch/f'filing-{index:08d}'
            unit.mkdir()
            index+=1
            paths=[]
            for filename,source in (('items',items),('segments',segments),('strict',strict)):
                predicate="adsh='"+adsh.replace("'","''")+"'"
                count=con.execute(f"SELECT count(*) FROM read_parquet('{source}') WHERE {predicate}").fetchone()[0]
                if count>100000:
                    raise ValueError('native notes filing working set exceeds bounded100000rows')
                paths.append(_copy(con,f"SELECT * FROM read_parquet('{source}') WHERE {predicate}",unit/(filename+'.parquet')))
            table=N.recon_overlay(con,*paths)
            writer.write_table(table,row_group_size=64)
    return target


def replay_native(spec, con, plan, products, scratch):
    """Whole source/product replay; no native population is dropped or deduped."""
    source_root=F.absolute(plan['source_root'])
    actual_keys={p.name.removeprefix('source=') for p in (source_root/'parts').iterdir() if p.is_dir()}
    if actual_keys!=set(plan['archives']):
        raise ValueError('orphan/missing native source partitions')
    source_validation={}
    for key in sorted(plan['archives'],key=S.order_key):
        source_validation[key]=replay_source(con,key,F.absolute(plan['archives'][key]['original']),source_root/'parts'/f'source={key}')
    existing={r[0] for r in con.execute('SHOW TABLES').fetchall()}
    if existing.intersection(TABLES):
        raise ValueError('native notes replay requires isolated consumer connection table namespace')
    generated={name:[] for name in ('segments','recon','wide','features')}
    try:
        iterator=item_sql(con,source_root/'parts',plan['years'])
        con.execute('CREATE TEMP TABLE business_census(adsh VARCHAR,member VARCHAR)')
        # item_sql creates sub on first iteration; then census needs the complete
        # filing population, including every year present in the native products.
        first=True
        for year,query in iterator:
            if first:
                first=False
                years=[r[0] for r in con.execute('SELECT DISTINCT year(filed) FROM sub ORDER BY 1').fetchall()]
                if years!=plan['years']:
                    raise ValueError('original complete notes filing-year population differs')
                N.collect_business_census(con,source_root/'parts',years)
                con.execute('CREATE TEMP TABLE business_census_original AS SELECT * FROM business_census')
                con.execute('CREATE TEMP TABLE structure_evidence(adsh VARCHAR,tag VARCHAR,proof_json VARCHAR)')
            items=products[f'items/year={year}/items.parquet'].as_posix()
            _compare(con,query,items)
            unit=scratch/str(year)
            unit.mkdir()
            con.execute(f"CREATE OR REPLACE TEMP TABLE cur AS SELECT adsh,arg_max(uom,n) currency FROM(SELECT adsh,uom,count(*) n FROM read_parquet('{items}') WHERE uom NOT IN('shares','pure') AND regexp_full_match(uom,'[A-Z]{{3}}') GROUP BY 1,2) GROUP BY 1")
            segment=_copy(con,N.segments_sql(items),unit/'segments.parquet')
            strict=_copy(con,N.recon_sql(items,segment,year),unit/'strict.parquet')
            overlay=_overlay(con,items,segment,strict,unit)
            con.execute(f"CREATE OR REPLACE TEMP VIEW recon_ov AS SELECT * FROM read_parquet('{overlay.as_posix()}')")
            recon=_copy(con,N.recon_final_sql(strict),unit/'recon.parquet')
            wide=_copy(con,N.wide_sql(items),unit/'wide.parquet')
            feature=_copy(con,N.features_sql(items,segment,wide,recon),unit/'features.parquet')
            for name,path in (('segments',segment),('recon',recon),('wide',wide),('features',feature)):
                generated[name].append(Path(path))
        for name,product in (('segments','segments.parquet'),('recon','segment_recon.parquet'),('wide','notes_wide.parquet')):
            _compare(con,_concat(generated[name]),products[product])
        raw=_copy(con,_concat(generated['features']),scratch/'features_raw.parquet')
        _compare(con,N.carry_sql(raw),products['segment_features.parquet'])
        census="""SELECT s.cik,s.adsh,s.form,s.period,s.filed,s.available_at,
          count(DISTINCT b.member) n_members,count(DISTINCT b.member)>=2 multi_segment,
          count(DISTINCT raw.member) n_members_source_tagged,
          CASE WHEN max(e.adsh) IS NULL THEN 'current_period_numeric_tags' ELSE 'source_role_structure' END member_basis
          FROM sub s LEFT JOIN business_census b USING(adsh) LEFT JOIN business_census_original raw USING(adsh)
          LEFT JOIN structure_evidence e USING(adsh) GROUP BY ALL"""
        _compare(con,census,products['segment_census.parquet'])
        structure='SELECT e.*,s.cik,s.period,s.filed,s.available_at,s.form FROM structure_evidence e JOIN(SELECT DISTINCT adsh,cik,period,filed,available_at,form FROM sub) s USING(adsh)'
        _compare(con,structure,products['segment_structure.parquet'])
        counts={name:con.execute(f"SELECT count(*) FROM read_parquet('{path.as_posix()}')").fetchone()[0] for name,path in products.items()}
        # Existing R12 namespace/version collisions are measured on the original
        # public key and preserved, not repaired by row-number keys or row drops.
        item_paths=[products[f'items/year={year}/items.parquet'] for year in plan['years']]
        collisions=con.execute('SELECT count(*) FROM(SELECT cik,adsh,item,tag,ddate,qtrs,uom,segments,coreg,count(*) n FROM('+_concat(item_paths)+') GROUP BY ALL HAVING n>1)').fetchone()[0]
        population=con.execute('SELECT year(filed),count(*) FROM sub GROUP BY 1 ORDER BY 1').fetchall()
        return {'schema':'atx.tier1-notes-native-validation/v1','source_replay':source_validation,
                'native_product_rows':counts,'filing_population':population,'legacy_collision_groups':collisions,
                'native_replay':'PASS','r12_parser_acceptance':'NEEDS_FIXES_CAP5',
                'source_quality_certified':False,'original_gates':_json(Path(spec['generation'])/'manifest.json').get('gate_status',{})}
    finally:
        for table in reversed(TABLES):
            con.execute('DROP VIEW IF EXISTS '+table) if table=='recon_ov' else con.execute('DROP TABLE IF EXISTS '+table)


def cell_schema():
    import pyarrow as pa
    return pa.schema([('cik',pa.int64()),('source_accession',pa.string()),('period',pa.date32()),
        ('field',pa.string()),('available_at',pa.timestamp('us',tz='UTC')),('value',pa.float64()),
        ('diagnostic_value',pa.float64()),('qualified',pa.bool_()),('reason',pa.string()),
        ('source_sha256',pa.string()),('proof_sha256',pa.string()),('bseg_partition_source_adsh',pa.string()),
        ('form',pa.string()),('is_amendment',pa.bool_()),('proof_method',pa.string()),
        ('native_provenance',pa.string())])


def _original_context_values(payload, concept, period, currency, cik):
    """Bounded original inline context/unit/value evidence, without inferred basis."""
    text=payload.decode('utf-8','replace')
    contexts={}
    seen_contexts=set()
    for match in re.finditer(r'<(?:\w+:)?context\b[^>]*\bid=["\']([^"\']+)["\'][^>]*>(.*?)</(?:\w+:)?context>',text,re.I|re.S):
        ident,body=match.groups()
        if ident in seen_contexts:
            contexts.pop(ident,None)
            continue
        seen_contexts.add(ident)
        end=re.findall(r'<(?:\w+:)?endDate>\s*([^<]+)\s*</',body,re.I)
        start=re.findall(r'<(?:\w+:)?startDate>\s*([^<]+)\s*</',body,re.I)
        entity=re.findall(r'<(?:\w+:)?identifier\b[^>]*>\s*(\d+)\s*</',body,re.I)
        if end!=[str(period)] or len(start)!=1 or entity!=[str(cik)] and (len(entity)!=1 or int(entity[0])!=cik):
            continue
        if not 300<=(dt.date.fromisoformat(str(period))-dt.date.fromisoformat(start[0])).days<=380:
            continue
        if re.search(r'<(?:\w+:)?typedMember\b',body,re.I):
            continue
        dimensions=re.findall(r'<(?:\w+:)?explicitMember\b[^>]*dimension=["\']([^"\']+)["\'][^>]*>\s*([^<]+)\s*</',body,re.I)
        segments=[]
        supported=True
        for axis,member in dimensions:
            if axis not in ('us-gaap:StatementBusinessSegmentsAxis','srt:ConsolidationItemsAxis'):
                supported=False
            short_axis='BusinessSegments' if axis=='us-gaap:StatementBusinessSegmentsAxis' else 'ConsolidationItems'
            segments.append(short_axis+'='+FS.local_member(member.strip())+';')
        if supported:
            contexts[ident]=''.join(sorted(segments)) or None
    units={}
    seen_units=set()
    for match in re.finditer(r'<(?:\w+:)?unit\b[^>]*\bid=["\']([^"\']+)["\'][^>]*>(.*?)</(?:\w+:)?unit>',text,re.I|re.S):
        ident,body=match.groups()
        if ident in seen_units:
            units.pop(ident,None)
            continue
        seen_units.add(ident)
        measures=re.findall(r'<(?:\w+:)?measure>\s*([^<]+)\s*</',body,re.I)
        if measures==['iso4217:'+currency] and not re.search(r'<(?:\w+:)?divide\b',body,re.I):
            units[ident]=currency
    values={}
    for match in re.finditer(r'<ix:nonFraction\b([^>]+)>(.*?)</ix:nonFraction>',text,re.I|re.S):
        attrs={k.lower():v for k,v in re.findall(r'([\w:]+)=["\']([^"\']*)["\']',match.group(1))}
        context=attrs.get('contextref')
        if attrs.get('name')!=concept.replace('_',':',1) or context not in contexts or attrs.get('unitref') not in units:
            continue
        if attrs.get('format') or attrs.get('xsi:nil') in ('true','1'):
            continue  # No unreviewed number-format conversion.
        number=re.sub(r'<[^>]*>','',match.group(2)).strip().replace(',','')
        if not re.fullmatch(r'[+-]?\d+(?:\.\d+)?',number):
            continue
        scale=attrs.get('scale','0')
        if not re.fullmatch(r'-?\d+',scale) or not -8<=int(scale)<=12 or attrs.get('sign') not in (None,'-'):
            continue
        value=float(number)*10**int(scale)*(-1 if attrs.get('sign')=='-' else 1)
        if not math.isfinite(value):
            continue
        values.setdefault(context,[]).append(value)
    return {ident:(contexts[ident],numbers[0]) for ident,numbers in values.items() if len(set(numbers))==1}


def _context(row, proof, inventory, con, products):
    """Independent context admission resolves actual native rows, never booleans."""
    admitted=inventory.get(row['bseg_partition_source_adsh'],{}).get('revenue_context_admission')
    if not admitted:
        return {}
    fields={'concept','currency','qtrs','member_contexts','total_context','bridge_contexts','basis'}
    if set(admitted)!=fields or admitted['basis']!='reported_segment_measure':
        return {}
    tag=admitted['concept'].removeprefix('us-gaap_')
    if admitted['concept']!=proof.get('concept') or set(admitted['member_contexts'])!=set(proof.get('members',[])):
        return {}
    if admitted['qtrs']!=4:
        return {}  # Native source-bound annual segment-measure HHI only.
    primary=[source for source in proof.get('sources',[]) if source.get('kind')=='primary']
    if len(primary)!=1 or primary[0]['sha256']!=inventory[row['bseg_partition_source_adsh']]['primary_sha256']:
        return {}
    with gzip.open(RAW_ROOT/primary[0]['path'],'rb') as stream:
        payload=stream.read(40*1024*1024+1)
    if len(payload)>40*1024*1024 or __import__('hashlib').sha256(payload).hexdigest()!=primary[0]['sha256']:
        raise ValueError('original revenue context source identity changed')
    originals=_original_context_values(payload,admitted['concept'],row['period'],admitted['currency'],row['cik'])
    cursor=con.cursor()
    items=products[f"items/year={row['source_year']}/items.parquet"].as_posix()
    values=[]
    def value(context):
        if set(context)!= {'segments','context_ids'} or not context['context_ids']:
            return None
        if any(ident not in originals or originals[ident][0]!=context['segments'] for ident in context['context_ids']):
            return None
        result=cursor.execute(f"SELECT DISTINCT value FROM read_parquet('{items}',hive_partitioning=false) WHERE adsh=? AND tag=? AND ddate=? AND qtrs=? AND uom=? AND coreg IS NULL AND segments IS NOT DISTINCT FROM ?",[row['source_accession'],tag,row['period'],admitted['qtrs'],admitted['currency'],context['segments']]).fetchall()
        actual=result[0][0] if len(result)==1 else None
        if actual is None or not math.isfinite(actual) or any(abs(originals[ident][1]-actual)>1e-8*max(1,abs(actual)) for ident in context['context_ids']):
            return None
        return actual
    for member,context in admitted['member_contexts'].items():
        plain='BusinessSegments='+member+';'
        if context['segments'] not in (plain,plain+'ConsolidationItems=OperatingSegments;'):
            return {}
        values.append(value(context))
    total=value(admitted['total_context'])
    bridges=[value(context) for context in admitted['bridge_contexts']]
    if total is None or any(v is None for v in bridges) or any(v is None for v in values):
        return {}
    return {'revenue_context_admission':admitted,'member_values':values,'total':total,'bridge':sum(bridges)}


def mapped_rows(plan, products, con):
    """Fixed four-field population, all original filing/vintage exclusions kept."""
    inventory=_json(plan['curated_inventory'])
    if set(inventory)!=set(G.curated_notes()):
        raise ValueError('independently admitted exact nine curated inventory required')
    features=products['segment_features.parquet'].as_posix()
    wide=products['notes_wide.parquet'].as_posix()
    structure=products['segment_structure.parquet'].as_posix()
    # Native census contains all SUB periodic filings, including no feature row.
    census=products['segment_census.parquet'].as_posix()
    cursor=con.cursor().execute(f"SELECT c.cik,c.adsh,c.period,c.available_at,c.form,year(c.filed) source_year,f.bseg_n,f.bseg_rev_hhi,f.foreign_sales_share,w.tax_dtl,f.bseg_partition_source_adsh FROM read_parquet('{census}') c LEFT JOIN read_parquet('{features}') f USING(cik,adsh,period) LEFT JOIN read_parquet('{wide}') w USING(cik,adsh,period) ORDER BY c.cik,c.period,c.available_at,c.adsh")
    names=[d[0] for d in cursor.description]
    while batch:=cursor.fetchmany(32):
        for values in batch:
            native=dict(zip(names,values))
            accession=native['bseg_partition_source_adsh']
            proof_rows=con.execute(f"SELECT proof_json,period,available_at,year(filed) FROM read_parquet('{structure}') WHERE adsh=?",[accession]).fetchall() if accession else []
            proof=json.loads(proof_rows[0][0]) if len(proof_rows)==1 else {}
            identity=proof.get('corroboration',{})
            base={'cik':native['cik'],'source_accession':native['adsh'],'period':native['period'],
                  'available_at':native['available_at'],'bseg_partition_source_adsh':accession,
                  'source_sha256':identity.get('primary_sha256'),'form':native['form'],
                  'is_amendment':native['form'].endswith('/A'), 'proof_method':proof.get('status'),
                  'proof_sha256':G.digest(proof) if proof else None}
            # Carried partitions retain their original accession/SHA and current
            # publication vintage clock; the cell key remains the current filing.
            qualification={**base,'period':str(base['period']) if base['period'] else None,
                           'source_accession':accession,'source_year':proof_rows[0][3] if len(proof_rows)==1 else native['source_year']}
            context=_context(qualification,proof,inventory,con,products) if accession else {}
            for field in FIELDS:
                row={**qualification,'field':field,**context}
                cutoff=base['available_at']+dt.timedelta(microseconds=1) if base['available_at'] else dt.datetime.min.replace(tzinfo=dt.timezone.utc)
                value,reason=G.notes_admission(row,proof,inventory,cutoff)
                if len(proof_rows)!=1:
                    value,reason=None,'missing_or_ambiguous_native_structure_proof'
                elif (not proof_rows[0][2] or not base['available_at'] or proof_rows[0][2]>base['available_at']
                      or str(proof_rows[0][1])!=str(base['period'])):
                    value,reason=None,'native_partition_period_or_clock_mismatch'
                # Namespace collisions and geographic selection never qualify.
                if field in ('tax_dtl','foreign_sales_share'):
                    value,reason=None,'ambiguous_note_or_unverified_geography'
                yield {**base,'field':field,'value':value,'diagnostic_value':native[field],
                       'qualified':value is not None,'reason':reason,
                       'native_provenance':json.dumps({'partition_source':accession,'proof':proof,
                                                      'field_definition':'fund_notes native '+field},sort_keys=True,default=str)}


def _mapped(plan, products, con, target):
    import pyarrow as pa
    import pyarrow.parquet as pq
    count=qualified=0
    reasons={}
    buffer=[]
    with pq.ParquetWriter(target,cell_schema(),compression='zstd') as writer:
        for row in mapped_rows(plan,products,con):
            count+=1
            qualified+=row['qualified']
            reasons[row['reason']]=reasons.get(row['reason'],0)+1
            buffer.append(row)
            if len(buffer)==32:
                writer.write_table(pa.Table.from_pylist(buffer,schema=cell_schema()),row_group_size=32)
                buffer=[]
        if buffer:
            writer.write_table(pa.Table.from_pylist(buffer,schema=cell_schema()),row_group_size=32)
    collision=con.execute(f"SELECT count(*) FROM(SELECT cik,source_accession,period,field,available_at,count(*) n FROM read_parquet('{target.as_posix()}') GROUP BY ALL HAVING n>1)").fetchone()[0]
    if collision:
        raise ValueError('mapped original filing/vintage key collisions; no deduplication allowed')
    return {'rows':count,'qualified_cells':qualified,'null_reasons':reasons,'fields':list(FIELDS),
            'keys':['cik','source_accession','period','field','available_at'],
            'clock':'original native notes available_at; later amendments are separate rows',
            'global_source_quality_certified':False}


def _run(spec, con):
    plan, products, dependencies=_prepare(spec,con)
    runtime=_native_runtime()
    # Retain isolated private replay artifacts for controller/reviewer inspection;
    # no warehouse scratch cleanup or native producer invocation.
    scratch=Path(tempfile.mkdtemp(prefix='atx-notes-native-replay-'))
    validation=replay_native(spec,con,plan,products,scratch)
    validation['native_runtime']=runtime
    mapped=scratch/'notes_cells.parquet'
    validation['mapping']=_mapped(plan,products,con,mapped)
    if validation['mapping']['rows']!=4*validation['native_product_rows']['segment_census.parquet']:
        raise ValueError('full notes mapped census denominator differs')
    _check(dependencies)
    if _native_runtime()!=runtime:
        raise ValueError('loaded notes native runtime changed during replay')
    F._guard(con,F.absolute(spec['root']))
    return plan,products,dependencies,validation,mapped,scratch


def materialize(spec, con):
    """Root-slot only: produce a private immutable map after full native replay.

    This creates no source quality PASS and changes no native stage/activation.
    Actual execution remains controller-serialized; fixtures invoke this only
    with synthetic original source plans and independently captured expectations.
    """
    _,_,dependencies,validation,mapped,scratch=_run(spec,con)
    if F.digest(validation)!=spec['expected_validation_sha256']:
        raise ValueError('independently expected native notes validation differs')
    generation=Path(spec['mapped_generation'])
    if not generation.is_absolute() or not generation.parent.resolve(strict=True).is_relative_to(F.absolute(spec['root'])):
        raise ValueError('private mapped generation must be inside independent root')
    mapped_pin=F.pin(mapped)
    manifest={'schema':'atx.tier1-notes-cells/v1','source_binding':_binding(spec),
              'dependency_pins':dependencies,'validation':validation,'files':{'notes_cells.parquet':mapped_pin}}
    manifest=json.loads(json.dumps(manifest,sort_keys=True,default=str))
    original=json.dumps(manifest,sort_keys=True,separators=(',',':'),default=str).encode()
    if generation.exists():
        if _json(generation/'manifest.json')!=manifest or F.pin(generation/'notes_cells.parquet')!=mapped_pin:
            raise ValueError('existing immutable notes mapping differs')
        return {'generation':str(generation),'manifest_pin':F.pin(generation/'manifest.json'),'product_pins':manifest['files'],'validation':validation}
    pending=generation.with_name(generation.name+'.partial')
    if pending.exists():
        raise ValueError('retained incomplete notes mapping requires controller reconciliation')
    pending.mkdir()
    # Bounded copy retains the private replay originals, never moves source bytes.
    with mapped.open('rb') as source,(pending/'notes_cells.parquet').open('xb') as out:
        while block:=source.read(1024*1024):
            out.write(block)
    (pending/'manifest.json').write_bytes(original)
    _check(dependencies)
    if F.pin(pending/'notes_cells.parquet')!=mapped_pin:
        raise ValueError('private mapped notes bytes changed during publication')
    os.rename(pending,generation)
    return {'generation':str(generation),'manifest_pin':F.pin(generation/'manifest.json'),'product_pins':manifest['files'],'validation':validation}


def admit(spec, con):
    """Read-only full native plus physical mapped product replay."""
    plan,products,dependencies,validation,mapped,_=_run(spec,con)
    generation=F.absolute(spec['mapped_generation'])
    if not generation.is_relative_to(F.absolute(spec['root'])) or {p.name for p in generation.iterdir()}!={'manifest.json','notes_cells.parquet'}:
        raise ValueError('complete private immutable notes generation required')
    if F.pin(generation/'manifest.json')!=spec['mapped_manifest_pin']:
        raise ValueError('independent mapped notes manifest differs')
    manifest=_json(generation/'manifest.json')
    target=generation/'notes_cells.parquet'
    if (manifest.get('schema')!='atx.tier1-notes-cells/v1' or manifest.get('source_binding')!=_binding(spec)
            or manifest.get('dependency_pins')!=dependencies or manifest.get('validation')!=json.loads(json.dumps(validation,default=str))
            or manifest.get('files')!=spec['mapped_product_pins']
            or set(spec['mapped_product_pins'])!={'notes_cells.parquet'}
            or F.pin(target)!=spec['mapped_product_pins']['notes_cells.parquet']
            or F.digest(validation)!=spec['expected_validation_sha256']):
        raise ValueError('independently expected native/mapped notes identities differ')
    _compare(con,f"SELECT * FROM read_parquet('{mapped.as_posix()}')",target)
    dependencies={**dependencies,str(target):F.pin(target),str(generation/'manifest.json'):F.pin(generation/'manifest.json')}
    _check(dependencies)
    products={**products,'notes_cells.parquet':target}
    import pyarrow.parquet as pq
    schemas={key:{field.name:str(field.type) for field in pq.read_schema(path)} for key,path in products.items()}
    return {'status':'admitted_diagnostic','products':products,'validation':validation,
            'dependency_pins':dependencies,'source_quality_certified':False,'schemas':schemas,
            'views':{'notes_cells.parquet':{'view':'fundamentals_notes_qualified_cells','keys':validation['mapping']['keys'],
                      'clock':'available_at','qualification':'qualified AND reason; R12 exclusions retained'}},
            'native_definitions':{'items':N.ITEMS,'segment_metrics':N.SEGMENT_METRICS,'features':N.FEATURE_COLS},
            'spec':spec}


def _read_check(admission,con):
    spec=admission['spec']
    F._guard(con,F.absolute(spec['root']))
    if (admission['source_quality_certified'] is not False
            or admission['products']['notes_cells.parquet']!=F.absolute(spec['mapped_generation'])/'notes_cells.parquet'
            or F.digest(admission['validation'])!=spec['expected_validation_sha256']
            or admission['validation']['native_runtime']!=_native_runtime()):
        raise ValueError('mutable notes consumer admission differs')
    plan,products,dependencies=_prepare(spec,con)
    generation=F.absolute(spec['mapped_generation'])
    dependencies.update({str(generation/'manifest.json'):spec['mapped_manifest_pin'],
                         str(generation/'notes_cells.parquet'):spec['mapped_product_pins']['notes_cells.parquet']})
    if dependencies!=admission['dependency_pins'] or {**products,'notes_cells.parquet':generation/'notes_cells.parquet'}!=admission['products']:
        raise ValueError('complete native notes consumer inventory differs')
    _check(dependencies)


def iter_rows(admission,product,con,batch_size=32):
    if product not in admission['products'] or not 1<=batch_size<=64:
        raise ValueError('fixed notes product and bounded batch required')
    import pyarrow.parquet as pq
    _read_check(admission,con)
    try:
        for batch in pq.ParquetFile(admission['products'][product]).iter_batches(batch_size=batch_size):
            yield from batch.to_pylist()
    finally:
        _read_check(admission,con)


@contextlib.contextmanager
def consuming(admission,con):
    _read_check(admission,con)
    try:
        yield admission['products']
    finally:
        _read_check(admission,con)
