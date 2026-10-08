"""Immutable available-clock-year index v2 products and sealed file selection.

Only the source-admitted build may publish. Legacy monolithic/v1 files are not
modified. Consumer selection reads the pinned manifest before selecting files;
excluded years are not opened, statted or hashed by the sealed reader.
"""
from __future__ import annotations
import datetime as dt
import copy
import hashlib
import json
import os
from pathlib import Path
from .private_index_pit import RULES

SCHEMA='atx.index-v2-publication/v1'
PRODUCTS=('constituents','exits','recon_summary','membership')
VERSION='index-public-reader-v2'


def digest(value):
    return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':'),default=str).encode()).hexdigest()


def pin(path):
    path=Path(path)
    h=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(1<<20),b''):
            h.update(block)
    return {'sha256':h.hexdigest(),'bytes':path.stat().st_size}


def check(path,expected):
    if set(expected)!= {'sha256','bytes'} or pin(path)!=expected:
        raise ValueError('independently pinned physical index byte identity changed')


def schemas():
    import pyarrow as pa
    date,integer,number,text,clock=pa.date32(),pa.int64(),pa.float64(),pa.string(),pa.timestamp('us',tz='UTC')
    return {
      'constituents':pa.schema([('index_id',text),('security_id',integer),('cik',integer),
        ('effective_from',date),('effective_to',date),('weight',number),('company_rank',integer),
        ('rank_date',date),('recon_date',date),('basis',text),('available_at',clock)]),
      'exits':pa.schema([('index_id',text),('security_id',integer),('recon_date',date),
        ('last_session',date),('available_at',clock),('exit_basis',text)]),
      'recon_summary':pa.schema([('index_id',text),('rank_date',date),('recon_date',date),
        ('effective_from',date),('lines',integer),('companies',integer),('additions',integer),
        ('deletions',integer),('turnover_oneway',number),('turnover_cap',number),
        ('p1000_cum_share',number),('eligible_companies',integer),('available_at',clock)]),
      'membership':pa.schema([('session_date',date),('security_id',integer),('cik',integer),
        ('r1000',pa.bool_()),('r2000',pa.bool_()),('r3000',pa.bool_()),('sp500',pa.bool_()),
        ('available_at',clock)])}


def _write_product(generation,name,rows):
    import pyarrow as pa
    import pyarrow.parquet as pq
    writers={};buffers={};counts={}
    schema=schemas()[name]
    try:
        for row in rows:
            stamp=row.get('available_at')
            if not isinstance(stamp,dt.datetime) or stamp.tzinfo is None:
                raise ValueError('every index product requires an explicit UTC clock')
            stamp=stamp.astimezone(dt.timezone.utc)
            if stamp.hour!=22 or stamp.minute or stamp.second or stamp.microsecond:
                raise ValueError('index house clock must be exactly 22 UTC')
            row={**row,'available_at':stamp}
            year=stamp.year
            if year not in writers:
                path=generation/name/f'available_year={year}'/f'{name}.parquet'
                path.parent.mkdir(parents=True,exist_ok=False)
                writers[year]=pq.ParquetWriter(path,schema,compression='zstd')
                buffers[year]=[];counts[year]=0
            buffers[year].append(row);counts[year]+=1
            if len(buffers[year])==64:
                writers[year].write_table(pa.Table.from_pylist(buffers[year],schema=schema))
                buffers[year].clear()
        for year,buffer in buffers.items():
            if buffer:
                writers[year].write_table(pa.Table.from_pylist(buffer,schema=schema))
    finally:
        for writer in writers.values():
            writer.close()
    return {f'{name}/available_year={year}/{name}.parquet':
            {**pin(generation/name/f'available_year={year}'/f'{name}.parquet'),
             'product':name,'available_year':year,'rows':counts[year],
             'schema':str(schema)} for year in sorted(writers)}


def write_candidate(generation,products,*,source_binding,receipt,gates):
    """Internal bounded writer; publication is diagnostic until independent replay.

    The source binding is the complete result of the fixed source admission, not
    a caller's source PASS. Public activation must independently replay it.
    """
    generation=Path(generation)
    generation.mkdir(parents=True,exist_ok=False)
    if set(products)!=set(PRODUCTS):
        raise ValueError('all four complete native index products required')
    files={}
    for name in PRODUCTS:
        files.update(_write_product(generation,name,products[name]))
    manifest={'schema':SCHEMA,'rules':list(RULES),'files':files,'products':list(PRODUCTS),
       'source_binding':source_binding,'receipt':receipt,'gates':gates,
       'status':'diagnostic_pending_independent_replay','source_quality_certified':False,
       'partition':'available_at_UTC_year','summary_clock':'rank_date22UTC',
       'membership_clock':'max(rank_date22UTC,previous_exchange_session22UTC)'}
    path=generation/'manifest.json'
    with path.open('x',encoding='utf-8') as stream:
        json.dump(manifest,stream,sort_keys=True,indent=2,default=str)
    return manifest


def manifest_only(generation,expected_manifest_pin):
    path=Path(generation)/'manifest.json'
    check(path,expected_manifest_pin)
    manifest=json.loads(path.read_text(encoding='utf-8'))
    if manifest['schema']!=SCHEMA or manifest['rules']!=list(RULES) or manifest['products']!=list(PRODUCTS):
        raise ValueError('wrong original index v2 publication contract')
    for relative,item in manifest['files'].items():
        expected=f"{item['product']}/available_year={item['available_year']}/{item['product']}.parquet"
        if item['product'] not in PRODUCTS or relative!=expected:
            raise ValueError('unexpected or escaping index publication file')
    return manifest


def select_files(manifest,product,cutoff):
    if product not in PRODUCTS or cutoff.tzinfo is None:
        raise ValueError('fixed product and explicit UTC cutoff required')
    cutoff=cutoff.astimezone(dt.timezone.utc)
    # At Jan1 00UTC even that year's 22UTC rows cannot be eligible. Manifest
    # selection therefore opens no new-year file at the sealed cutoff.
    limit=cutoff.year-1 if cutoff==dt.datetime(cutoff.year,1,1,tzinfo=dt.timezone.utc) else cutoff.year
    return {key:item for key,item in manifest['files'].items()
            if item['product']==product and item['available_year']<=limit}


def consuming_files(generation,expected_manifest_pin,product,cutoff):
    """Before/after validation of only manifest-selected bytes; no excluded opens."""
    from contextlib import contextmanager
    generation=Path(generation)
    expected_manifest_pin=copy.deepcopy(expected_manifest_pin)
    @contextmanager
    def scope():
        manifest=manifest_only(generation,expected_manifest_pin)
        selected=select_files(manifest,product,cutoff)
        for key,item in selected.items():
            check(Path(generation)/key,{k:item[k] for k in ('sha256','bytes')})
        try:
            yield [Path(generation)/key for key in sorted(selected)]
        finally:
            manifest_only(generation,expected_manifest_pin)
            for key,item in selected.items():
                check(Path(generation)/key,{k:item[k] for k in ('sha256','bytes')})
    return scope()


def _validate_rowsets(con,generation,manifest,replayed):
    """Internal complete duplicate-preserving replay, not a public admission API."""
    import pyarrow as pa
    import pyarrow.parquet as pq
    generation=Path(generation)
    expected_files=set(manifest['files'])|{'manifest.json'}
    actual_files={path.relative_to(generation).as_posix() for path in generation.rglob('*') if path.is_file()}
    if actual_files!=expected_files:
        raise ValueError('complete index generation file inventory differs')
    results={}
    for name in PRODUCTS:
        schema=schemas()[name]
        columns=', '.join('"'+field.name+'"' for field in schema)
        con.execute('DROP TABLE IF EXISTS index_expected')
        # Register only a typed empty batch to establish the native relation;
        # insert bounded batches so complete membership never collects in RAM.
        con.register('index_empty',pa.Table.from_pylist([],schema=schema))
        con.execute('CREATE TEMP TABLE index_expected AS SELECT * FROM index_empty')
        con.unregister('index_empty')
        buffer=[]
        def insert():
            con.register('index_batch',pa.Table.from_pylist(buffer,schema=schema))
            con.execute('INSERT INTO index_expected SELECT * FROM index_batch')
            con.unregister('index_batch')
            buffer.clear()
        for row in replayed[name]:
            buffer.append(row)
            if len(buffer)==64:
                insert()
        if buffer:
            insert()
        inventory={key:item for key,item in manifest['files'].items() if item['product']==name}
        paths=[]
        for key,item in inventory.items():
            path=generation/key
            check(path,{k:item[k] for k in ('sha256','bytes')})
            if pq.read_schema(path)!=schema:
                raise ValueError('exact native index typed schema differs')
            paths.append("'"+path.as_posix().replace("'","''")+"'")
            relation="read_parquet(["+','.join(paths[-1:])+"],hive_partitioning=false)"
            wrong=con.execute(f"SELECT count(*) FROM {relation} WHERE available_at IS NULL OR year(available_at)<>{int(item['available_year'])}").fetchone()[0]
            count=con.execute(f'SELECT count(*) FROM {relation}').fetchone()[0]
            if wrong or count!=item['rows']:
                raise ValueError('index row clock/year/count differs from complete manifest')
        actual=("SELECT "+columns+" FROM read_parquet(["+','.join(paths)+"],hive_partitioning=false)") if paths else 'SELECT * FROM index_expected WHERE false'
        differences=con.execute(f"SELECT count(*) FROM (({actual} EXCEPT ALL SELECT * FROM index_expected) UNION ALL (SELECT * FROM index_expected EXCEPT ALL {actual}))").fetchone()[0]
        if differences:
            raise ValueError('index original native full typed row population replay differs: '+name)
        keys={'constituents':'index_id,security_id,recon_date','exits':'index_id,security_id,recon_date,available_at',
              'recon_summary':'index_id,recon_date,available_at','membership':'session_date,security_id,available_at'}[name]
        collisions=con.execute(f'SELECT count(*) FROM (SELECT {keys},count(*) n FROM index_expected GROUP BY {keys} HAVING n>1)').fetchone()[0]
        if collisions:
            raise ValueError('ambiguous index vintage population; no deduplication')
        results[name]={'rows':con.execute('SELECT count(*) FROM index_expected').fetchone()[0],
                       'typed_duplicate_preserving_replay':True,'collisions':0}
        for key,item in inventory.items():
            check(generation/key,{k:item[k] for k in ('sha256','bytes')})
    con.execute('DROP TABLE index_expected')
    return results


def asof_sql(paths,product,cutoff):
    """Fixed native latest-vintage relation inside consuming_files scope.

    The caller must execute this SQL while the independently pinned selected
    files are held by that scope. It never discovers a filesystem glob.
    """
    if product not in PRODUCTS or cutoff.tzinfo is None:
        raise ValueError('fixed index product and UTC cutoff required')
    if not paths:
        # A typed empty native relation, not an invented source acceptance.
        types={'int64':'BIGINT','double':'DOUBLE','string':'VARCHAR','date32[day]':'DATE',
               'bool':'BOOLEAN','timestamp[us, tz=UTC]':'TIMESTAMPTZ'}
        return 'SELECT '+','.join(f'CAST(NULL AS {types[str(field.type)]}) AS "{field.name}"'
                      for field in schemas()[product])+' WHERE false'
    listed=','.join("'"+Path(path).as_posix().replace("'","''")+"'" for path in paths)
    clock=cutoff.astimezone(dt.timezone.utc).isoformat()
    key={'constituents':'index_id,security_id,recon_date','exits':'index_id,security_id,recon_date',
         'recon_summary':'index_id,recon_date','membership':'session_date,security_id'}[product]
    return (f"SELECT * FROM read_parquet([{listed}],hive_partitioning=false) "
            f"WHERE available_at<TIMESTAMPTZ '{clock}' QUALIFY "
            f"row_number() OVER(PARTITION BY {key} ORDER BY available_at DESC)=1")


def _generation(spec):
    from . import common as C, private_tier1_admission as A
    root=A.absolute(spec['lake_root'])
    if root!=C.build_root().resolve() or spec['interface_version']!=VERSION:
        raise ValueError('fixed native root and reviewed index public reader required')
    generation=A.member(root,spec['generation'])
    if generation.parent!=root/'indexes_v2'/'generations':
        raise ValueError('explicit immutable index generation required')
    return generation


def _source_binding(binding,previous=None):
    return {'source_plan_sha256':binding['spec']['expected_source_plan_sha256'],
            'source_spec_sha256':digest(binding['spec']),
            'dependency_pins':binding['dependency_pins'],
            'previous_spec_sha256':None if previous is None else digest(previous)}


def _prior_scope(spec,con,depth):
    import contextlib
    import pyarrow.parquet as pq
    @contextlib.contextmanager
    def scope():
        previous=spec.get('previous')
        if previous is None:
            yield []
            return
        admission=admit(previous,con,_depth=depth+1)
        if admission['generation']==str(_generation(spec)):
            raise ValueError('current/previous immutable index generation cycle')
        with consuming(admission,con):
            rows=[]
            for key,path in admission['products'].items():
                if admission['manifest']['files'][key]['product']=='exits':
                    for batch in pq.ParquetFile(path).iter_batches(batch_size=64):
                        rows.extend(batch.to_pylist())
            yield rows
    return scope()


def build(spec,con):
    """Root-controlled complete diagnostic candidate, never implicit activation."""
    from . import private_index_sources as S
    spec=copy.deepcopy(spec)
    generation=_generation(spec)
    with _prior_scope(spec,con,0) as prior, S.source_replay(spec['sources'],con) as binding:
        products,receipt,gates=S.replay_products(binding,con,prior_exits=prior)
        manifest=write_candidate(generation,products,source_binding=_source_binding(binding,spec.get('previous')),
                                 receipt=receipt,gates=gates)
        # Full public admission is separate and requires the independently
        # expected candidate identity/validation, never values derived here.
        return {'generation':str(generation),'manifest_pin':pin(generation/'manifest.json'),
                'receipt':receipt,'gates':gates,'status':manifest['status'],
                'source_quality_certified':False}


def admit(spec,con,*,_depth=0):
    """Full fixed original source replay; no manifest-only source admission.

    This engineering preflight examines complete construction inventory. The
    sealed consumer uses consuming_files after independent preflight; that
    selection never opens excluded available-clock years.
    """
    from . import private_index_sources as S, private_tier1_admission as A
    if _depth>32:
        raise ValueError('bounded immutable prior-generation chain exceeded')
    spec=copy.deepcopy(spec)
    A.guard(con)
    generation=_generation(spec)
    manifest=manifest_only(generation,spec['manifest_pin'])
    with _prior_scope(spec,con,_depth) as prior, S.source_replay(spec['sources'],con) as binding:
        if manifest['source_binding']!=_source_binding(binding,spec.get('previous')):
            raise ValueError('native immutable index complete source binding differs')
        products,receipt,gates=S.replay_products(binding,con,prior_exits=prior)
        if digest(receipt)!=digest(manifest['receipt']) or digest(gates)!=digest(manifest['gates']):
            raise ValueError('complete original native index census/gates replay differs')
        validation={'products':_validate_rowsets(con,generation,manifest,products),
                    'receipt':receipt,'gates':gates,'source_quality_certified':False}
        if digest(validation)!=spec['expected_validation_sha256']:
            raise ValueError('independently expected complete index validation differs')
        dependencies=dict(binding['dependency_pins'])
        if spec.get('previous') is not None:
            # Independently pinned prior admission is held by _prior_scope;
            # include every consumed original and immutable prior product.
            dependencies.update(spec['previous']['dependency_pins'])
        dependencies[str(generation/'manifest.json')]=spec['manifest_pin']
        for key,item in manifest['files'].items():
            dependencies[str(generation/key)]={name:item[name] for name in ('sha256','bytes')}
        if dependencies!=spec['dependency_pins']:
            raise ValueError('complete independently expected index consumption inventory differs')
        A.check_pins(dependencies)
        manifest_only(generation,spec['manifest_pin'])
    return {'status':'admitted_diagnostic','generation':str(generation),'manifest':manifest,
            'products':{key:generation/key for key in manifest['files']},
            'product_files':{name:[generation/key for key,item in manifest['files'].items()
                                  if item['product']==name] for name in PRODUCTS},
            'schemas':schemas(),'validation':validation,'dependency_pins':dependencies,
            'source_quality_certified':False,'spec':spec}


def consuming(admission,con):
    """Authenticate all construction dependencies once before/finally use."""
    import contextlib
    from . import private_tier1_admission as A
    binding=copy.deepcopy(admission)
    @contextlib.contextmanager
    def scope():
        A.guard(con)
        A.check_pins(binding['dependency_pins'])
        try:
            yield binding['products']
        finally:
            try:
                A.check_pins(binding['dependency_pins'])
            finally:
                A.guard(con)
    return scope()


def resolve_current(spec,con):
    """Resolve an independently pinned explicit reference, never scan candidates."""
    from . import private_tier1_admission as A
    spec=copy.deepcopy(spec)
    root=A.absolute(spec['lake_root'])
    reference=root/'indexes_v2'/'current.json'
    check(reference,spec['current_pin'])
    if reference.stat().st_size>1<<20:
        raise ValueError('bounded native index current reference exceeded')
    record=json.loads(reference.read_text())
    expected={'schema':'atx.index-current/v2','generation':spec['generation'],
              'manifest_pin':spec['manifest_pin'],'status':'admitted_diagnostic',
              'source_quality_certified':False}
    if record!=expected:
        raise ValueError('independently expected native index current reference differs')
    result=admit(spec,con)
    check(reference,spec['current_pin'])
    return result


def activate_diagnostic(spec,con,*,expected_current_pin,action='activate'):
    """Controller-only atomic diagnostic selection or explicit rollback.

    Both directions fully replay the target native generation. This does not
    certify economics or consumer acceptance, and never rewrites legacy v1.
    Expected prior reference is independent compare-and-swap authority.
    """
    import uuid
    if action not in ('activate','rollback'):
        raise ValueError('explicit native diagnostic transition required')
    spec=copy.deepcopy(spec)
    generation=_generation(spec)
    stage=generation.parent.parent
    stage.mkdir(parents=True,exist_ok=True)
    reference=stage/'current.json'
    lock=stage/'activation.lock'
    token=uuid.uuid4().hex
    with lock.open('x',encoding='utf-8') as stream:
        json.dump({'transition':token,'pid':os.getpid(),'action':action},stream)
    old=None;changed=False
    def replace(payload):
        temporary=stage/f'current.{token}.partial'
        with temporary.open('xb') as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary,reference)
    try:
        if expected_current_pin is None:
            if reference.exists():
                raise ValueError('native current reference appeared; no blind overwrite')
        else:
            check(reference,expected_current_pin)
            if reference.stat().st_size>1<<20:
                raise ValueError('bounded prior index current reference exceeded')
            old=reference.read_bytes()
        admission=admit(spec,con)
        with consuming(admission,con):
            if expected_current_pin is None:
                if reference.exists():
                    raise ValueError('native current reference changed during full replay')
            else:
                check(reference,expected_current_pin)
            record={'schema':'atx.index-current/v2','generation':spec['generation'],
                    'manifest_pin':spec['manifest_pin'],'status':'admitted_diagnostic',
                    'source_quality_certified':False}
            payload=json.dumps(record,sort_keys=True,indent=2).encode()
            expected_reference={'sha256':hashlib.sha256(payload).hexdigest(),'bytes':len(payload)}
            replace(payload)
            changed=True
        check(reference,expected_reference)
        return {'action':action,'current_pin':expected_reference,'reference':record,
                'validation_sha256':digest(admission['validation']),
                'source_quality_certified':False,'consumer_acceptance':'PENDING'}
    except BaseException:
        if changed:
            if old is None:
                reference.unlink()
            else:
                replace(old)
        raise
    finally:
        # The exact lock was exclusively created by this transition; no other
        # process's stale lock or shared scratch is removed.
        if json.loads(lock.read_text()).get('transition')==token:
            lock.unlink()
