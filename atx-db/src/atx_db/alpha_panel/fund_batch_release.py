"""R15 independent fundamentals batch release; no legacy unit admission.

All runtime source IO requires an exclusive controller-granted guarded slot.
New computations retain the logical and physical fundamentals_v11 name. A
complete candidate remains NOT_PRODUCTION_READY and never certifies economics.
"""
from __future__ import annotations

import argparse
import contextlib
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import tempfile
import types
import uuid
import zipfile

import pyarrow as pa
import pyarrow.parquet as pq

from . import common as C
from . import fundamentals as F
from . import fund_extract as X
from . import fund_fx as FX
from . import fund_validate as V

STAGE = 'fundamentals_release_v1'
SEMANTIC_STAGE = 'fundamentals_v11'
VERSION = 'fund-batch-release-v1'
REQUIRED = tuple(range(85))
PREPARATION_UNITS = 144
TABLES = ('events','sic','history','catalog')
CONTRACT = {
    'version': VERSION, 'stage': STAGE, 'semantic_producer': SEMANTIC_STAGE,
    'required_batches': list(REQUIRED), 'preparation_units': PREPARATION_UNITS,
    'source_backed_only': True, 'readiness': 'NOT_PRODUCTION_READY',
    'qualification': 'SOURCE_CODE_PREPARATION_BYTES_AND_REPLAY_BOUND',
    'source_quality_certified': False, 'legacy_import': 'forbidden',
    'gate_years': list(V.FYEARS), 'item_target': V.TARGET, 'item_minimum':105,
    'item_stretch':110, 'top_n':V.TOP_N, 'fsds_cells':V.BENCH_CELLS,
    'fsds_tolerance':V.BENCH_TOL, 'balance_target':.985,
    'exit_years':list(range(2020,2026)), 'exit_targets':V.EXIT_FIELDS,
    'cutoff_events':V.CUTOFF_EVENTS,'cutoff_seed':V.CUTOFF_SEED,
    'return_statistics': 'none; financial coverage only, no return evaluation',
}


def digest(value):
    return hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest()


def _pairs(items):
    out={}
    for key,value in items:
        if key in out:
            raise ValueError('duplicate binding key: '+key)
        out[key]=value
    return out


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8'),object_pairs_hook=_pairs)


def identity(path):
    path=Path(path)
    before=path.stat()
    value={'sha256':C.sha256_file(path),'bytes':before.st_size}
    after=path.stat()
    if (before.st_size,before.st_mtime_ns)!=(after.st_size,after.st_mtime_ns):
        raise ValueError('file changed while hashing: '+str(path))
    return value


def _path(path):
    return Path(path).resolve()


def _member(root,relative):
    result=(root/relative).resolve()
    if Path(relative).is_absolute() or '..' in Path(relative).parts or not result.is_relative_to(root.resolve()):
        raise ValueError('path escape')
    return result


def sql(paths):
    if not paths:
        raise ValueError('empty reader product')
    return 'read_parquet(['+','.join("'"+Path(p).as_posix().replace("'","''")+"'" for p in paths)+'],union_by_name=true,hive_partitioning=false)'


def code_paths():
    directory=Path(F.__file__).parent
    paths=[directory/(name+'.py') for name in (*F.MODULES,'fund_validate','fund_asof','fund_batch_release')]
    return sorted(set(paths+[directory.parent/'reference_classifications.py',directory.parent/'statement_map_seed.py',contract_path()]))


def contract_path():
    return Path(__file__).parents[3]/'docs/datasets/fundamentals_release_v1.contract.json'


def reader_contract(root):
    """Actual calculation and native final validation readers, not parent declarations."""
    root=_path(root)
    from atx_db.statement_map_seed import STATEMENT_MAP_SEED_PATH
    return {
        'links_prepare': {'paths':[root/'identity/links.parquet']},
        'links_validation': {'paths':[root/'identity/link_table.parquet']},
        'calendar': {'paths':[root/'calendar.parquet']},
        'statement_seed': {'paths':[_path(STATEMENT_MAP_SEED_PATH)]},
        'fx': {'paths':None,'optional':True},
        'eight_k': {'paths':[root/'sec_filings/eight_k_items.parquet'],'optional':True},
        'prices': {'pattern':str(root/'prices/year=*/*.parquet')},
        'prices_history': {'pattern':str(root/'prices_history/year=*/prices.parquet')},
        'members': {'paths':[root/'_tmp/panel_member'/f'year={y}.parquet' for y in range(2020,2026)]},
        'identity_manifest': {'paths':[root/'identity/manifest.json']},
        'filings_manifest': {'paths':[root/'sec_filings/manifest.json']},
        'reference_manifest': {'paths':[root/'reference/manifest.json']},
    }


def _actual(rule):
    if 'pattern' in rule:
        import glob
        return sorted(_path(p) for p in glob.glob(rule['pattern']) if Path(p).is_file())
    return sorted(_path(p) for p in rule['paths']) if rule['paths'] is not None else None


def check_products(root,products):
    contract=reader_contract(root)
    if set(products)!=set(contract):
        raise ValueError('incomplete/extra actual reader inventory')
    pinned={}
    for name,rule in contract.items():
        item=products[name]
        actual=_actual(rule)
        if item is None:
            if not rule.get('optional') or actual and any(p.exists() for p in actual) or name=='fx' and (Path(root)/'reference/fx_daily.parquet').exists():
                raise ValueError('missing mandatory or present optional product: '+name)
            pinned[name]=None
            continue
        if not all(isinstance(item.get(k),str) and item[k].strip() for k in ('source','clock','admission')):
            raise ValueError('independent reader provenance required: '+name)
        files=item.get('files',{})
        selected=sorted(_path(p) for p in files)
        if not selected or len(selected)!=len(set(selected)) or actual is not None and selected!=actual:
            raise ValueError('missing/substituted/extra selected reader files: '+name)
        if name=='fx' and len(selected)!=1:
            raise ValueError('one explicit FX product required')
        for path in selected:
            if identity(path)!=files[str(path)]:
                raise ValueError('reader bytes changed: '+str(path))
        if name.endswith('_manifest'):
            doc=read(selected[0]);members=doc.get('files')
            if not isinstance(members,dict) or not members:
                raise ValueError('empty producer inventory: '+name)
            for relative,pin in members.items():
                if identity(_member(selected[0].parent,relative))!={'sha256':pin.get('sha256'),'bytes':pin.get('bytes')}:
                    raise ValueError('producer member mismatch')
        pinned[name]=item
    return pinned


def _clone(module,overrides):
    """Private globals; never mutate imported modules, environment or common root."""
    namespace={**vars(module),**overrides}
    for name,value in vars(module).items():
        if isinstance(value,types.FunctionType) and value.__module__==module.__name__ and name not in overrides:
            fn=types.FunctionType(value.__code__,namespace,name,value.__defaults__,value.__closure__)
            fn.__kwdefaults__=value.__kwdefaults__
            namespace[name]=fn
    return types.SimpleNamespace(**namespace)


def native_adapter(binding,output):
    root=_path(binding['root']);prep=_path(binding['prep_root']);extract=_path(binding['extract_root'])
    output=_path(output)
    def product(name):
        item=binding['products'][name]
        return None if item is None else _path(next(iter(item['files'])))
    common=types.SimpleNamespace(**vars(C))
    common.build_root=lambda:root
    common.FSDS_DIR=_path(binding['fsds_root'])
    common.calendar_path=lambda:product('calendar')
    # Writers supplied explicitly: native finalize/run/write_stage_manifest are unused.
    common.connect=lambda **kw:C.connect(**{**kw,'threads':1})
    extract_proxy=types.SimpleNamespace(**vars(X))
    extract_proxy.stage_name=lambda:SEMANTIC_STAGE
    extract_proxy.out_dir=lambda:extract
    extract_proxy.ARCHIVE=_path(binding['archive']['path'])
    extract_proxy.batch_files=lambda:[extract/f'batch-{i:04d}.parquet' for i in REQUIRED]
    extract_proxy.archive_identity=lambda:identity(extract_proxy.ARCHIVE)
    fx_proxy=types.SimpleNamespace(**vars(FX))
    fx_proxy.fx_path=lambda:product('fx')
    fx_proxy.load_optional=lambda:None if product('fx') is None else FX.load_optional(product('fx'))
    native=_clone(F,{'common':common,'fx':extract_proxy,'ffx':fx_proxy,
        'fund_dir':lambda:output,'work_dir':lambda:prep,'parts_dir':lambda:output,
        'links_path':lambda:product('links_prepare'),'eight_k_path':lambda:product('eight_k')})
    validator=_clone(V,{'common':common,'fu':native,'fx':extract_proxy,'ffx':fx_proxy,
        '_stage':lambda:output,'out_path':lambda:output.parent/'validation/fundamentals_v11.json',
        '_prices_glob':lambda:sorted(p for name in ('prices_history','prices') for p in binding['products'][name]['files'])})
    return native,validator


def check_extract(binding,con,verify_keys=True):
    """Every planned archive member and all 85 source batches remain explicit."""
    root=_path(binding['extract_root']);archive=binding['archive']
    if identity(archive['path'])!={k:archive[k] for k in ('sha256','bytes')}:
        raise ValueError('archive drift')
    plan=read(root/'plan.json')
    if plan.get('batches')!=len(REQUIRED) or plan.get('code_version')!=X.CODE_VERSION:
        raise ValueError('wrong full extraction plan')
    with zipfile.ZipFile(archive['path']) as z:
        planned=X.plan(z)
    if len(planned)!=len(REQUIRED):
        raise ValueError('archive plan differs from required 85 batches')
    expected={'plan.json'}|{f'batch-{i:04d}{suffix}' for i in REQUIRED for suffix in ('.parquet','.json','.stats.parquet')}
    if {p.name for p in root.iterdir() if p.is_file()}!=expected:
        raise ValueError('incomplete/unsupported extraction inventory')
    pins={}
    for name in sorted(expected):
        pins[name]=identity(root/name)
    for bid,members in enumerate(planned):
        receipt=read(root/f'batch-{bid:04d}.json');path=root/f'batch-{bid:04d}.parquet'
        required_ciks=[int(X.MEMBER_RE.search(m.filename).group(1)) for m in members]
        rows=receipt.get('member_rows',[])
        if receipt.get('batch_id')!=bid or receipt.get('code_version')!=X.CODE_VERSION or receipt.get('file')!=path.name:
            raise ValueError('unsupported extract receipt')
        if [r.get('cik') for r in rows]!=required_ciks or len(required_ciks)!=len(set(required_ciks)):
            raise ValueError('extract member population mismatch')
        if pins[path.name]!={'sha256':receipt.get('parquet_sha256'),'bytes':receipt.get('parquet_bytes')}:
            raise ValueError('extract payload mismatch')
        if any(type(r.get('rows')) is not int or r['rows']<0 for r in rows):
            raise ValueError('invalid extract member counters')
        if verify_keys:
            con.execute('CREATE OR REPLACE TEMP VIEW release_cf AS SELECT * FROM '+sql([path]))
            counts=dict(con.execute('SELECT cik,count(*) FROM release_cf GROUP BY 1').fetchall())
            if any(counts.get(r['cik'],0)!=r['rows'] for r in rows) or set(counts)-set(required_ciks):
                raise ValueError('extract typed member rows mismatch')
        if sum(r['rows'] for r in rows)!=receipt.get('rows') or pq.ParquetFile(path).schema_arrow!=X.SCHEMA or pq.ParquetFile(path).metadata.num_rows!=receipt.get('rows'):
            raise ValueError('extract schema/rows mismatch')
        # Archive size/mtime is not sufficient; current full SHA is independently pinned.
        stat=Path(archive['path']).stat()
        if any(receipt.get('archive',{}).get(k)!=value for k,value in {'bytes':stat.st_size,'mtime_ns':stat.st_mtime_ns}.items()):
            raise ValueError('extract archive identity mismatch')
        if verify_keys:
            # The legacy extractor's size/mtime receipt is not source admission.
            # Reconstruct both typed source products directly from pinned ZIP bytes.
            scratch=Path(binding['root'])/'_tmp/fund-release-extract-verify'
            scratch.mkdir(parents=True,exist_ok=True)
            with tempfile.TemporaryDirectory(prefix='extract-',dir=scratch) as tmp:
                target=Path(tmp)
                extractor=_clone(X,{'out_dir':lambda:target,
                    'archive_identity':lambda:{'bytes':stat.st_size,'mtime_ns':stat.st_mtime_ns}})
                with zipfile.ZipFile(archive['path']) as z:
                    replay=extractor.extract_batch(z,bid,members)
                for suffix in ('.parquet','.stats.parquet'):
                    original=root/f'batch-{bid:04d}{suffix}';peer=target/original.name
                    if pq.ParquetFile(original).schema_arrow!=pq.ParquetFile(peer).schema_arrow:
                        raise ValueError('extract archive replay schema mismatch')
                    difference=con.execute(f'SELECT count(*) FROM ((SELECT * FROM {sql([original])} EXCEPT ALL SELECT * FROM {sql([peer])}) UNION ALL (SELECT * FROM {sql([peer])} EXCEPT ALL SELECT * FROM {sql([original])}))').fetchone()[0]
                    if difference:raise ValueError('extract archive replay mismatch')
                if replay['member_rows']!=rows or replay['counts']!=receipt.get('counts') or replay['members']!=receipt.get('members'):
                    raise ValueError('extract archive member/error population mismatch')
    return {'plan':plan,'files':pins,'archive':archive}


def freeze(binding_path,con,verify_keys=True):
    if read(contract_path())!=CONTRACT:raise ValueError('frozen contract differs from implementation')
    binding_path=_path(binding_path);binding=read(binding_path)
    if binding.get('version')!=VERSION or binding.get('semantic_producer')!=SEMANTIC_STAGE:
        raise ValueError('unsupported binding/semantic producer')
    products=check_products(binding['root'],binding['products'])
    native,_=native_adapter(binding,Path(binding['prep_root']).parent/'fundamentals_v11')
    preparation=native._verify_preparation()
    if len(preparation['units'])!=PREPARATION_UNITS:
        raise ValueError('required preparation inventory is not 144 units')
    extracted=check_extract(binding,con,verify_keys)
    return {'version':VERSION,'contract':CONTRACT,'binding_path':str(binding_path),'binding_identity':identity(binding_path),
        'binding':binding,'products':products,'extract':extracted,'preparation':preparation,
        'prepare_summary':identity(Path(binding['prep_root'])/'prepare.json'),
        'code':{str(p.resolve()):identity(p) for p in code_paths()}}


def recheck(snapshot,con,full=False):
    require_snapshot(snapshot)
    if freeze(snapshot['binding_path'],con,verify_keys=full)!=snapshot:
        raise ValueError('source/code/preparation snapshot changed')


def require_snapshot(snapshot):
    if snapshot.get('version')!=VERSION or snapshot.get('contract')!=CONTRACT:
        raise ValueError('unsupported/abbreviated frozen contract')
    if set(snapshot.get('code',{}))!={str(p.resolve()) for p in code_paths()}:
        raise ValueError('incomplete actual code inventory')
    if set(snapshot.get('products',{}))!=set(reader_contract(snapshot['binding']['root'])):
        raise ValueError('incomplete actual reader inventory')
    if len(snapshot.get('preparation',{}).get('units',{}))!=PREPARATION_UNITS:
        raise ValueError('incomplete preparation inventory')


def schemas():
    return {'events':F.EVENT_SCHEMA,'sic':F.SIC_PART_SCHEMA,'history':F.HISTORY_SCHEMA,'catalog':F.CATALOG_SCHEMA}


def payloads(directory,bid):
    return {name:Path(directory)/f'{name}-{bid:04d}.parquet' for name in TABLES}


def receipt_semantics(path):
    row=read(path)
    return {k:v for k,v in row.items() if k not in ('seconds','created_utc')}


def validate_payloads(con,directory,bid,source,expected=None):
    """Native schemas and source populations; nullable fiscal_period is retained."""
    paths=payloads(directory,bid)
    con.execute('CREATE OR REPLACE TEMP VIEW release_source AS SELECT * FROM '+sql([source]))
    result={}
    keys={'events':['cik','accession'],'catalog':['cik','accession'],'sic':['cik','accession'],
          'history':['cik','item','period_end','fiscal_period','accession']}
    for name,path in paths.items():
        if pq.ParquetFile(path).schema_arrow!=schemas()[name]:
            raise ValueError('unsupported native output schema: '+name)
        con.execute('CREATE OR REPLACE TEMP VIEW release_'+name+' AS SELECT * FROM '+sql([path]))
        key=','.join(keys[name]);nonnull=[k for k in keys[name] if k!='fiscal_period']
        if con.execute('SELECT 1 FROM release_'+name+' WHERE '+' OR '.join(k+' IS NULL' for k in nonnull)+' LIMIT 1').fetchone():
            raise ValueError('null output key: '+name)
        if con.execute(f'SELECT 1 FROM release_{name} GROUP BY {key} HAVING count(*)>1 LIMIT 1').fetchone():
            raise ValueError('duplicate output key: '+name)
        clock='clock_utc' if name=='sic' else 'available_at'
        bad=f"SELECT 1 FROM release_{name} o WHERE {clock} IS NULL OR NOT EXISTS(SELECT 1 FROM release_source s WHERE s.cik=o.cik AND s.accession_number=o.accession)"
        if name in ('events','catalog'):
            bad+=f" OR clock_utc IS NULL OR available_at<clock_utc OR clock_utc<TIMESTAMP '{F.EMIT_FROM.isoformat()}' OR period_end IS NULL"
        if con.execute(bad+' LIMIT 1').fetchone():
            raise ValueError('unsupported source population/clock: '+name)
        if name=='history' and con.execute("SELECT 1 FROM release_history WHERE item NOT IN ("+','.join("'"+k+"'" for k in F.fi.HISTORY_ITEMS)+") LIMIT 1").fetchone():
            raise ValueError('unsupported history item')
        if expected is not None:
            peer=payloads(expected,bid)[name]
            differences=con.execute(f'SELECT count(*) FROM ((SELECT * FROM release_{name} EXCEPT ALL SELECT * FROM {sql([peer])}) UNION ALL (SELECT * FROM {sql([peer])} EXCEPT ALL SELECT * FROM release_{name}))').fetchone()[0]
            if differences:
                raise ValueError('source-backed replay mismatch: '+name)
        result[name]={'file':path.name,**identity(path),'rows':pq.ParquetFile(path).metadata.num_rows}
    if con.execute('SELECT count(*) FROM ((SELECT cik,accession FROM release_events EXCEPT SELECT cik,accession FROM release_catalog) UNION ALL (SELECT cik,accession FROM release_catalog EXCEPT SELECT cik,accession FROM release_events))').fetchone()[0]:
        raise ValueError('event/catalog population mismatch')
    return result


@contextlib.contextmanager
def writer_lock(stage):
    stage.mkdir(parents=True,exist_ok=True);path=stage/'.writer.lock'
    try:fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_WRONLY)
    except FileExistsError:raise RuntimeError('writer active/interrupted; inspect retained lock') from None
    try:
        os.write(fd,str(os.getpid()).encode());yield
    finally:os.close(fd);path.unlink()


def _generation(stage,name):
    if not name or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-' for c in name):
        raise ValueError('invalid generation id')
    return stage/'generations'/name


def _replay(snapshot,con,bid,target):
    native,_=native_adapter(snapshot['binding'],target)
    native._register_static(con)
    native.process_batch(con,Path(snapshot['binding']['extract_root'])/f'batch-{bid:04d}.parquet',native.ffx.load_optional())


def admit_unit(snapshot,generation,con,bid):
    path=generation/'units'/f'batch-{bid:04d}'/'admission.json'
    if not path.exists():return None
    row=read(path)
    if set(row)!={'version','batch','snapshot_sha256','attempt','start_identity','files','native_receipt'} or type(row.get('batch')) is not int or row.get('version')!=VERSION or row.get('batch')!=bid or row.get('snapshot_sha256')!=digest(snapshot):
        raise ValueError('stale/unsupported unit receipt')
    attempt=_member(path.parent,row['attempt'])
    start=read(attempt/'start.json')
    if start.get('snapshot_sha256')!=digest(snapshot) or start.get('batch')!=bid or start.get('version')!=VERSION or row.get('start_identity')!=identity(attempt/'start.json'):
        raise ValueError('missing at-compute provenance; legacy admission forbidden')
    native_dir=attempt/'native'
    expected_files={p.name for p in payloads(native_dir,bid).values()}|{f'batch-{bid:04d}.json'}
    if {p.name for p in native_dir.iterdir()}!=expected_files or row.get('native_receipt')!=identity(native_dir/f'batch-{bid:04d}.json'):
        raise ValueError('unsupported/corrupt native unit inventory')
    # Reconstruct from the pinned sources, not from recorded key/counter claims.
    temp_root=Path(snapshot['binding']['root'])/'_tmp/fund-release-verify';temp_root.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='check-',dir=temp_root) as tmp:
        target=Path(tmp);_replay(snapshot,con,bid,target)
        files=validate_payloads(con,native_dir,bid,Path(snapshot['binding']['extract_root'])/f'batch-{bid:04d}.parquet',target)
        if receipt_semantics(native_dir/f'batch-{bid:04d}.json')!=receipt_semantics(target/f'batch-{bid:04d}.json'):
            raise ValueError('native failure/counter population differs from source replay')
    if row.get('files')!=files:
        raise ValueError('unit output identity changed')
    return row


def compute_new(snapshot,generation,con,bid):
    root=generation/'units'/f'batch-{bid:04d}'
    if (root/'admission.json').exists():raise ValueError('new-only unit already admitted')
    attempt=root/('attempt-'+uuid.uuid4().hex);native_dir=attempt/'native'
    native_dir.mkdir(parents=True,exist_ok=False)
    start={'version':VERSION,'batch':bid,'snapshot_sha256':digest(snapshot),
           'attempt':attempt.name,'started_utc':dt.datetime.now(dt.UTC).isoformat()}
    C.write_json_atomic(attempt/'start.json',start)
    recheck(snapshot,con)
    _replay(snapshot,con,bid,native_dir)
    recheck(snapshot,con)
    source=Path(snapshot['binding']['extract_root'])/f'batch-{bid:04d}.parquet'
    files=validate_payloads(con,native_dir,bid,source)
    row={'version':VERSION,'batch':bid,'snapshot_sha256':digest(snapshot),'attempt':attempt.name,
         'start_identity':identity(attempt/'start.json'),'files':files,
         'native_receipt':identity(native_dir/f'batch-{bid:04d}.json')}
    # Seal only after independent replay; completed legacy files never enter here.
    temp_root=Path(snapshot['binding']['root'])/'_tmp/fund-release-verify';temp_root.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='seal-',dir=temp_root) as tmp:
        _replay(snapshot,con,bid,Path(tmp));validate_payloads(con,native_dir,bid,source,Path(tmp))
        if receipt_semantics(native_dir/f'batch-{bid:04d}.json')!=receipt_semantics(Path(tmp)/f'batch-{bid:04d}.json'):
            raise ValueError('native failure/counter population differs from source replay')
    recheck(snapshot,con)
    C.write_json_atomic(root/'admission.json',row)
    return row


def native_evidence(snapshot,con,leaf):
    """Owned orchestration: every native checker receives explicit candidate paths."""
    native,validator=native_adapter(snapshot['binding'],leaf)
    evidence={'contract':CONTRACT,'coverage':validator.coverage(con,leaf),
        'benchmark':validator.benchmark(con,leaf),'balance':validator.balance(con,leaf),
        'cutoff':validator.cutoff(con,leaf),'fx':validator.fx_check(con,leaf/'events.parquet'),
        'ttm_gaps':validator.ttm_gaps(con,leaf/'events.parquet'),
        'exit':validator.exit_measure(con,leaf,tuple(CONTRACT['exit_years'])),
        'native_stats':native._stats(con,leaf/'events.parquet',leaf/'sic_events.parquet',leaf/'quarterly_history.parquet'),
        'event_coverage':native.coverage_by_year(con,leaf/'events.parquet')}
    # Financial validation only. No return statistic or factor/evaluation helper runs.
    # Wall time is operational evidence, never a deterministic admission field.
    evidence['cutoff'].pop('seconds',None)
    # Native stats contain tuples; compare the exact persisted JSON representation.
    return json.loads(json.dumps(evidence,allow_nan=False))


def merged_queries(snapshot,generation,con,units):
    leaf=generation/SEMANTIC_STAGE
    native,_=native_adapter(snapshot['binding'],leaf)
    table_paths={name:[generation/'units'/f'batch-{bid:04d}'/units[str(bid)]['attempt']/'native'/units[str(bid)]['files'][name]['file'] for bid in REQUIRED] for name in TABLES}
    ek=snapshot['binding']['products']['eight_k']
    events='SELECT *,CAST(NULL AS TIMESTAMP) AS nonreliance_402_at FROM '+sql(table_paths['events'])
    if ek is not None:
        events=f"SELECT e.*,CASE WHEN k.t402>e.available_at-INTERVAL {F.NONRELIANCE_DAYS} DAY THEN k.t402 END AS nonreliance_402_at FROM {sql(table_paths['events'])} e ASOF LEFT JOIN (SELECT cik,available_at AS t402 FROM {sql(list(ek['files']))} WHERE item='4.02' AND available_at IS NOT NULL) k ON e.cik=k.cik AND e.available_at>=k.t402"
    sics=[r[0] for r in con.execute('SELECT DISTINCT sic FROM '+sql(table_paths['sic'])+' WHERE sic IS NOT NULL ORDER BY 1').fetchall()]
    con.register('release_ff',native._ff_lookup(sics))
    return {
        'events.parquet':events+' ORDER BY cik,clock_utc,accession',
        'catalog.parquet':'SELECT * FROM '+sql(table_paths['catalog'])+' ORDER BY cik,clock_utc,accession',
        'quarterly_history.parquet':'SELECT * FROM '+sql(table_paths['history'])+' ORDER BY cik,item,period_end,available_at,accession',
        'sic_events.parquet':'SELECT s.cik,s.clock_utc,s.sic,f.sic2,f.ff12,f.ff49,s.accession,s.sic_basis FROM '+sql(table_paths['sic'])+' s LEFT JOIN release_ff f USING(sic) ORDER BY cik,clock_utc,accession'}


def merge(snapshot,generation,con,units):
    leaf=generation/SEMANTIC_STAGE;leaf.mkdir(exist_ok=True)
    for file,query in merged_queries(snapshot,generation,con,units).items():
        C.copy_to_parquet(con,query,leaf/file,row_group_size=32768)
    for name,file,key in [('events','events.parquet','cik,accession'),('catalog','catalog.parquet','cik,accession'),('history','quarterly_history.parquet','cik,item,period_end,fiscal_period,accession'),('sic','sic_events.parquet','cik,accession')]:
        if con.execute('SELECT 1 FROM '+sql([leaf/file])+' GROUP BY '+key+' HAVING count(*)>1 LIMIT 1').fetchone():
            raise ValueError('cross-batch duplicate key: '+name)
    return leaf


def _output_identities(generation,with_manifest=True):
    leaf=generation/SEMANTIC_STAGE
    expected={'events.parquet','catalog.parquet','quarterly_history.parquet','sic_events.parquet'}
    if {p.name for p in leaf.iterdir()}!=expected|({'manifest.json'} if with_manifest else set()):
        raise ValueError('unsupported final output inventory')
    return {SEMANTIC_STAGE+'/'+name:identity(leaf/name) for name in sorted(expected)}


def leaf_manifest(snapshot,generation):
    files=_output_identities(generation,with_manifest=(generation/SEMANTIC_STAGE/'manifest.json').exists())
    return {'stage':SEMANTIC_STAGE,'schema':F.SCHEMA,'status':'complete','source_backed_only':True,
        'rule':F.RULE,'code_version':F.CODE_VERSION,'readiness':'NOT_PRODUCTION_READY','source_quality_certified':False,
        'release_foundation':{'stage':STAGE,'generation':generation.name,'snapshot_sha256':digest(snapshot)},
        'files':{Path(k).name:v for k,v in files.items()},'catalog':F.catalog_manifest(),
        'preparation_binding':snapshot['preparation'],'sources':snapshot,
        'rule_text':F.RULE_TEXT+' V11 source-backed-only; missing values remain missing; no balance plugs.',
        'template_rule':F.fi.TEMPLATE_RULE,'structural_na':F.structural_na(),
        'class_share_rule':F.CLASS_RULE,'pre_flag_rule':F.PRE_RULE,'pos_cogs_rule':F.POS_RULE,
        'legacy_item_rules':F.ITEM_RULES,'nil_zero_rule':'disabled: absence/nil does not certify an explicit source zero',
        'item_rules':{'missing_values':'Only explicit source zeros are zero. Missing/PRE omission/nil omissions stay NULL.',
            'balance':'Reported source stocks; no assets-minus-liabilities balancing plugs; catalog temporary equity retained.',
            'debt':'DebtCurrent else current LTD plus reported other short borrowing; total LTD less reported current LTD.',
            'history':'Core discrete quarters plus direct source annual flows and tax/debt/RE/temp-equity comparatives at disclosure clocks.',
            'sales':'Presented contractual revenue only when own FSDS IS/CI contractual caption exists and generic Revenues caption does not.'},
        'emit_from_clock':F.EMIT_FROM.isoformat(),'items_v1':list(F.fi.ITEM_COLUMNS),'items_v2':list(F.fi.ITEM_COLUMNS_V2),
        'history_items':list(F.fi.HISTORY_ITEMS),'nonreliance_rule':F.NONRELIANCE_RULE,
        'staleness_rule':F.STALENESS_RULE,'currency_rule':F.CURRENCY_RULE,'fx_rule':FX.RULE,
        'gate_status':{'source_quality':'FAIL_UNQUALIFIED','whole_dataset_acceptance':'FAIL_PENDING'}}


def check_merged(snapshot,generation,con,units):
    leaf=generation/SEMANTIC_STAGE
    expected_schema={'events.parquet':pa.schema([*F.EVENT_SCHEMA,('nonreliance_402_at',pa.timestamp('us'))]),
        'catalog.parquet':F.CATALOG_SCHEMA,'quarterly_history.parquet':F.HISTORY_SCHEMA,
        'sic_events.parquet':pa.schema([('cik',pa.int64()),('clock_utc',pa.timestamp('us')),('sic',pa.int32()),
            ('sic2',pa.int32()),('ff12',pa.string()),('ff49',pa.string()),('accession',pa.string()),('sic_basis',pa.string())])}
    for file,query in merged_queries(snapshot,generation,con,units).items():
        if pq.ParquetFile(leaf/file).schema_arrow!=expected_schema[file]:
            raise ValueError('unsupported final schema: '+file)
        differences=con.execute(f'SELECT count(*) FROM ((SELECT * FROM {sql([leaf/file])} EXCEPT ALL ({query})) UNION ALL (({query}) EXCEPT ALL SELECT * FROM {sql([leaf/file])}))').fetchone()[0]
        if differences:raise ValueError('final output differs from admitted units: '+file)


def gate_status(evidence):
    coverage=evidence['coverage'];balance=evidence['balance'];cutoff=evidence['cutoff']
    years=set(coverage.get('observations_by_fyear',{}))
    items=years=={str(y) for y in V.FYEARS} and all(n>0 for n in coverage['observations_by_fyear'].values()) and coverage.get('items_pass_covered',0)>=CONTRACT['item_minimum']
    cutoff_ok=cutoff.get('pass') is True and cutoff.get('events_sampled')==CONTRACT['cutoff_events'] and cutoff.get('compared_rows',0)>0 and cutoff.get('differences')==0
    exit=evidence['exit'];exit_years={str(y) for y in CONTRACT['exit_years']}
    exit_ok=exit.get('years')==CONTRACT['exit_years'] and exit.get('targets')==CONTRACT['exit_targets'] and set(exit.get('by_year',{}))==exit_years
    exit_ok=exit_ok and all(row.get('member_cells',0)>0 and all(row.get(field,{}).get('member_all_cells',0)>0 and row.get(field,{}).get('linked_usd_cells',0)>0 for field in CONTRACT['exit_targets']) for row in exit.get('by_year',{}).values())
    exit_ok=exit_ok and set(exit.get('pooled',{}))==set(CONTRACT['exit_targets']) and all(exit['pooled'][field].get('target')==target and exit['pooled'][field].get('met') is True for field,target in CONTRACT['exit_targets'].items())
    benchmark=evidence['benchmark']
    return {'item_depth':'PASS' if items else 'FAIL','balance':'PASS' if balance.get('issuer_periods',0)>0 and (balance.get('ok_share') or 0)>=CONTRACT['balance_target'] else 'FAIL',
        'native_balance_99':'PASS' if balance.get('issuer_periods',0)>0 and balance.get('pass_99') is True else 'FAIL','FSDS':'PASS' if benchmark.get('pass') is True and benchmark.get('cells')==CONTRACT['fsds_cells'] else 'FAIL',
        'cutoff':'PASS' if cutoff_ok else 'FAIL','exit':'PASS' if exit_ok else 'FAIL',
        'whole_dataset_acceptance':'FAIL_PENDING','Task4':'FAIL_OPEN_0_of_6','economic_source_quality':'FAIL_UNQUALIFIED'}


def build(binding_path,generation_id,selected=REQUIRED,max_new_batches=None):
    if not os.environ.get('ATX_GUARD_JOB'):raise RuntimeError('data command requires external memory guard')
    if not set(selected)<=set(REQUIRED) or max_new_batches is not None and (type(max_new_batches) is not int or max_new_batches<0):
        raise ValueError('invalid bounded work selection')
    binding=read(binding_path);stage=Path(binding['root'])/STAGE
    with writer_lock(stage):
        if (stage/'pointer-transaction.json').exists():raise ValueError('unfinished pointer transaction; recover first')
        generation=_generation(stage,generation_id)
        if (generation/'manifest.json').exists():raise ValueError('immutable completed generation')
        con=C.connect(memory='300MB',threads=1,temp_dir=stage/'_spill',db_file='fund-release-'+uuid.uuid4().hex+'.duckdb')
        try:
            snapshot=freeze(binding_path,con)
            if (generation/'snapshot.json').exists():
                if read(generation/'snapshot.json')!=snapshot:raise ValueError('stale generation')
            elif generation.exists() and any(generation.iterdir()):raise ValueError('unbound candidate directory')
            else:C.write_json_atomic(generation/'snapshot.json',snapshot)
            units={};new=0
            for bid in REQUIRED:
                unit=admit_unit(snapshot,generation,con,bid)
                if unit is None and bid in selected and (max_new_batches is None or new<max_new_batches):
                    unit=compute_new(snapshot,generation,con,bid);new+=1
                if unit is not None:units[str(bid)]=unit
            recheck(snapshot,con)
            if set(units)!={str(i) for i in REQUIRED}:
                return {'status':'partial_cache_only','published':False,'missing_batches':[i for i in REQUIRED if str(i) not in units]}
            leaf=merge(snapshot,generation,con,units)
            check_merged(snapshot,generation,con,units)
            evidence=native_evidence(snapshot,con,leaf)
            C.write_json_atomic(generation/'validation.json',evidence)
            C.write_json_atomic(leaf/'manifest.json',leaf_manifest(snapshot,generation))
            recheck(snapshot,con)
            for bid in REQUIRED:admit_unit(snapshot,generation,con,bid)
            manifest={'stage':STAGE,'schema':'atx.fundamentals-release/v1','status':'complete',
                'contract':CONTRACT,'semantic_producer':SEMANTIC_STAGE,'source_backed_only':True,
                'readiness':'NOT_PRODUCTION_READY','source_quality_certified':False,
                'snapshot_sha256':digest(snapshot),'snapshot':identity(generation/'snapshot.json'),
                'files':_output_identities(generation),'validation':identity(generation/'validation.json'),
                'native_manifest':identity(leaf/'manifest.json'),
                'units':{str(i):identity(generation/'units'/f'batch-{i:04d}'/'admission.json') for i in REQUIRED},
                'gate_status':gate_status(evidence)}
            C.write_json_atomic(generation/'manifest.json',manifest)
            manifest_pin=identity(generation/'manifest.json')
            validate_generation(stage,generation,con)
            _publish_pointer(stage,generation,manifest_pin)
            return {'status':'candidate_published','published':True,'readiness':'NOT_PRODUCTION_READY'}
        finally:con.close()


def validate_generation(stage,generation,con):
    stage=_path(stage);generation=_path(generation)
    if stage.name!=STAGE or generation!=_generation(stage,generation.name).resolve():
        raise ValueError('unsupported generation location')
    manifest_identity=identity(generation/'manifest.json')
    manifest=read(generation/'manifest.json');snapshot=read(generation/'snapshot.json')
    if {p.name for p in generation.iterdir()}!={'manifest.json','snapshot.json','validation.json','units',SEMANTIC_STAGE} or {p.name for p in (generation/'units').iterdir()}!={f'batch-{i:04d}' for i in REQUIRED}:
        raise ValueError('unsupported full generation inventory')
    require_snapshot(snapshot)
    if any(manifest.get(k)!=v for k,v in {'stage':STAGE,'schema':'atx.fundamentals-release/v1','status':'complete','contract':CONTRACT,'semantic_producer':SEMANTIC_STAGE,'source_backed_only':True,'readiness':'NOT_PRODUCTION_READY','source_quality_certified':False}.items()):
        raise ValueError('unsupported release qualification/contract')
    if manifest.get('source_backed_only') is not True or manifest.get('source_quality_certified') is not False:
        raise ValueError('unsupported typed release qualification')
    if manifest.get('snapshot_sha256')!=digest(snapshot) or manifest.get('snapshot')!=identity(generation/'snapshot.json'):
        raise ValueError('changed snapshot')
    recheck(snapshot,con,full=True)
    if set(manifest.get('units',{}))!={str(i) for i in REQUIRED}:raise ValueError('incomplete required 85-unit inventory')
    units={}
    for bid in REQUIRED:
        units[str(bid)]=admit_unit(snapshot,generation,con,bid)
        if units[str(bid)] is None or manifest['units'][str(bid)]!=identity(generation/'units'/f'batch-{bid:04d}'/'admission.json'):
            raise ValueError('changed required unit')
    if manifest.get('files')!=_output_identities(generation) or manifest.get('validation')!=identity(generation/'validation.json'):
        raise ValueError('changed final output/evidence')
    native_path=generation/SEMANTIC_STAGE/'manifest.json'
    if manifest.get('native_manifest')!=identity(native_path) or read(native_path)!=leaf_manifest(snapshot,generation):
        raise ValueError('unsupported/changed native consumer manifest')
    check_merged(snapshot,generation,con,units)
    evidence=native_evidence(snapshot,con,generation/SEMANTIC_STAGE)
    if read(generation/'validation.json')!=evidence or manifest.get('gate_status')!=gate_status(evidence):
        raise ValueError('original gate/coverage evidence differs from sources')
    recheck(snapshot,con)
    if manifest['files']!=_output_identities(generation) or manifest['validation']!=identity(generation/'validation.json') or manifest['native_manifest']!=identity(native_path) or manifest['snapshot']!=identity(generation/'snapshot.json') or manifest_identity!=identity(generation/'manifest.json'):
        raise ValueError('release changed during validation')
    for bid in REQUIRED:
        if manifest['units'][str(bid)]!=identity(generation/'units'/f'batch-{bid:04d}'/'admission.json'):
            raise ValueError('unit receipt changed during validation')
        row=units[str(bid)];attempt=generation/'units'/f'batch-{bid:04d}'/row['attempt']
        if row['start_identity']!=identity(attempt/'start.json') or row['native_receipt']!=identity(attempt/'native'/f'batch-{bid:04d}.json'):
            raise ValueError('unit provenance changed during validation')
        for item in row['files'].values():
            if identity(attempt/'native'/item['file'])!={k:item[k] for k in ('sha256','bytes')}:
                raise ValueError('unit payload changed during validation')
    return manifest


def _publish_pointer(stage,generation,manifest_pin):
    # Called only after full admission by the owned build/rollback orchestration.
    if identity(generation/'manifest.json')!=manifest_pin:raise ValueError('manifest changed before publication')
    target={'generation':generation.name,'manifest':manifest_pin}
    prior=read(stage/'current.json') if (stage/'current.json').exists() else None
    if prior is not None and identity(_generation(stage,prior['generation'])/'manifest.json')!=prior['manifest']:
        raise ValueError('current pointer identity mismatch before publication')
    transaction={'version':VERSION,'current':target,'previous':prior}
    C.write_json_atomic(stage/'pointer-transaction.json',transaction)
    _finish_pointer(stage,transaction)


def _finish_pointer(stage,transaction):
    if transaction['previous'] is not None:C.write_json_atomic(stage/'previous.json',transaction['previous'])
    C.write_json_atomic(stage/'current.json',transaction['current'])
    (stage/'pointer-transaction.json').unlink()


def resolve_current(stage):
    if (stage/'pointer-transaction.json').exists():raise ValueError('unfinished pointer transaction')
    pointer=read(stage/'current.json');generation=_generation(stage,pointer['generation'])
    if identity(generation/'manifest.json')!=pointer['manifest']:raise ValueError('pointer identity mismatch')
    return generation


def rollback(stage,con):
    with writer_lock(stage):
        if (stage/'pointer-transaction.json').exists():raise ValueError('unfinished pointer transaction')
        prior=read(stage/'previous.json');generation=_generation(stage,prior['generation'])
        if identity(generation/'manifest.json')!=prior['manifest']:raise ValueError('rollback identity mismatch')
        validate_generation(stage,generation,con)
        _publish_pointer(stage,generation,prior['manifest'])


def recover(stage,con):
    with writer_lock(stage):
        tx=read(stage/'pointer-transaction.json')
        if tx.get('version')!=VERSION:raise ValueError('unsupported pointer transaction')
        for target in (tx['current'],tx['previous']):
            if target is not None:
                generation=_generation(stage,target['generation'])
                if identity(generation/'manifest.json')!=target['manifest']:raise ValueError('transaction identity mismatch')
                validate_generation(stage,generation,con)
        _finish_pointer(stage,tx)


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command',choices=('contract','build','validate','rollback','recover'))
    parser.add_argument('--bindings',type=Path);parser.add_argument('--generation');parser.add_argument('--root',type=Path)
    parser.add_argument('--max-new-batches',type=int)
    args=parser.parse_args(argv)
    if args.command=='contract':print(json.dumps(CONTRACT,indent=2));return
    if not os.environ.get('ATX_GUARD_JOB'):raise RuntimeError('data command requires external memory guard')
    if args.command=='build':
        if args.bindings is None or args.generation is None:parser.error('build requires explicit bindings and generation')
        print(build(args.bindings,args.generation,max_new_batches=args.max_new_batches));return
    if args.root is None:parser.error('explicit root required')
    stage=args.root/STAGE
    con=C.connect(memory='300MB',threads=1,temp_dir=stage/'_spill',db_file='fund-release-check-'+uuid.uuid4().hex+'.duckdb')
    try:
        if args.command=='rollback':rollback(stage,con)
        elif args.command=='recover':recover(stage,con)
        else:validate_generation(stage,resolve_current(stage),con)
    finally:con.close()


if __name__=='__main__':main()
