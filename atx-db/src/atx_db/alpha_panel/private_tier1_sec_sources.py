"""Immutable full-population original SEC inputs for downstream native replay.

This is a fixed source-phase publication, not an alias for sec_filings' complete
public stage. Every original main/history is replayed; unresolved native clocks
and snapshot profiles remain diagnostic. Accepted evidence is a separate product.
"""
import contextlib
import hashlib
import json
import math
import os
import re
import shutil
import uuid
from pathlib import Path
from . import common as C, fund_batch_release as F
from . import private_tier1_admission as A, private_tier1_event_replay as E

VERSION='tier1-sec-original-sources-v1'
PRODUCTS=('raw_filings.parquet','raw_profiles.parquet','filings.parquet',
          'eight_k_items.parquet','clock_files.parquet','original_accepted.parquet')
EXECUTION={'interface_version':VERSION,'memory_ceiling_bytes':128000000,'threads':1,
           'timezone':'UTC','file_backed':True,'disk_floor_gib':40,
           'source_quality_certified':False,'complete_main_and_history_replay':True}


def _json(path,expected):
    path=A.absolute(path)
    with path.open('rb') as stream:blob=stream.read(8*1024**2+1)
    if len(blob)>8*1024**2 or {'bytes':len(blob),'sha256':hashlib.sha256(blob).hexdigest()}!=expected:
        raise ValueError('consumed bounded SEC contract bytes differ')
    return json.loads(blob)


def _runtime(value):
    if (set(value)!={'threads','timezone','memory_limit','file_backed'}
            or type(value['threads']) is not int or value['threads']!=1
            or value['timezone']!='UTC' or value['file_backed'] is not True):
        raise ValueError('recorded actual SEC connection identity differs')
    quantity,unit=value['memory_limit'].split()
    scale={'B':1,'KB':1000,'MB':1000000,'GB':1000000000,
        'KiB':1024,'MiB':1048576,'GiB':1073741824}.get(unit)
    quantity=float(quantity)
    if scale is None or not math.isfinite(quantity) or quantity<=0 or quantity*scale>128000000:
        raise ValueError('recorded actual SEC connection exceeds128MB ceiling')


def code_pins():
    return {**E.code_pins(),**A.code(('private_tier1_sec_sources','filing_text_native',
        'filing_text','private_tier1_admission','fund_batch_release','common'))}


def binding(spec):
    return {key:spec[key] for key in ('interface_version','plan_path','plan_pin',
        'execution_contract','execution_pin','expected_code_sha256','stage_root')}


def _source(spec,con):
    A.guard(con)
    if spec['interface_version']!=VERSION:raise ValueError('fixed full original SEC interface required')
    plan_path=A.absolute(spec['plan_path']);execution=A.absolute(spec['execution_contract'])
    if A.pin(plan_path)!=spec['plan_pin'] or A.pin(execution)!=spec['execution_pin']:
        raise ValueError('independently pinned SEC plan/execution bytes differ')
    if _json(execution,spec['execution_pin'])!=EXECUTION or A.digest(code_pins())!=spec['expected_code_sha256']:
        raise ValueError('reviewed actual SEC source code/runtime contract differs')
    plan=_json(plan_path,spec['plan_pin'])
    if set(plan)!={'schema','archive_path','receipt_path','source_root','clock_plan',
        'clock_plan_pin','extraction_dependency_pins','clock_dependency_pins'} or plan['schema']!='atx.tier1-sec-original-plan/v1':
        raise ValueError('complete fixed SEC source plan required')
    clock_path=A.absolute(plan['clock_plan'])
    if A.pin(clock_path)!=plan['clock_plan_pin']:raise ValueError('independent original Accepted plan differs')
    review=spec['independent_review'];review_path=A.absolute(review['path'])
    if A.pin(review_path)!=review['pin']:raise ValueError('independent SEC source review bytes differ')
    expected={'schema':'atx.tier1-sec-original-review/v1','status':'scoped_pass',
        'interface_version':VERSION,'binding_sha256':A.digest(binding(spec)),'code_pins':code_pins()}
    if _json(review_path,review['pin'])!=expected:raise ValueError('independent actual SEC source binding review required')
    complete={**plan['extraction_dependency_pins'],**plan['clock_dependency_pins'],**code_pins(),
        str(plan_path):spec['plan_pin'],str(execution):spec['execution_pin'],
        str(clock_path):plan['clock_plan_pin'],str(review_path):review['pin']}
    if complete!=spec['source_dependency_pins']:
        raise ValueError('complete independent SEC original/code inventory differs')
    A.check_pins(complete)
    return plan,complete


def _replay(con,plan,target):
    """Full original extraction, unchanged native clock resolution and Accepted join."""
    source=E.extract_sec_raw(con,plan['archive_path'],plan['receipt_path'],target/'raw',
        plan['extraction_dependency_pins'])
    for name in PRODUCTS[:2]:
        shutil.copyfile(target/'raw'/name,target/name)
    relation='t17_full_sec_raw'
    if relation in {r[0] for r in con.execute('SHOW TABLES').fetchall()}:
        raise ValueError('SEC original replay relation namespace occupied')
    con.execute('CREATE VIEW '+relation+' AS SELECT * FROM '+A.sql([target/'raw_filings.parquet']))
    try:
        with E.sec_filings_scope(con,relation,plan['extraction_dependency_pins']) as queries:
            for name,key in (('filings.parquet','filings'),('eight_k_items.parquet','items'),('clock_files.parquet','clock_files')):
                C.copy_to_parquet(con,queries[key],target/name)
            clock_plan=_json(plan['clock_plan'],plan['clock_plan_pin'])
            with E.accepted_clock_scope(con,plan['source_root'],'filings',clock_plan,
                    A.digest(clock_plan),plan['clock_dependency_pins']) as accepted:
                C.copy_to_parquet(con,accepted,target/'original_accepted.parquet')
    finally:con.execute('DROP VIEW '+relation)
    counts={name:con.execute('SELECT count(*) FROM '+A.sql([target/name])).fetchone()[0] for name in PRODUCTS}
    return {'original_population':source['population'],'rows':counts,'source_quality_certified':False,
        'profile_semantics':'original_snapshot_non_pit','clock_semantics':'native_clock_and_separate_original_accepted',
        'full_event_body_market_identity_admission':'separate_required_dependencies'}


def materialize(spec,con,generation_id):
    """Controller-only immutable source products; failed attempts remain retained."""
    copied=json.loads(json.dumps(spec));plan,pins=_source(copied,con)
    if not re.fullmatch('[A-Za-z0-9_-]+',generation_id):raise ValueError('invalid immutable SEC generation')
    stage=A.absolute(copied['stage_root']);generation=A.member(stage,'generations/'+generation_id)
    with F.writer_lock(stage):
        if generation.exists():raise ValueError('preserve existing SEC generation; use independently pinned reader')
        generation.mkdir(parents=True)
        try:
            settings=con.execute("SELECT current_setting('threads'),current_setting('TimeZone'),current_setting('memory_limit')").fetchone()
            start={'schema':'atx.tier1-sec-original-start/v1','binding_sha256':A.digest(binding(copied)),
                'source_dependencies':pins,'actual_connection':{'threads':settings[0],
                    'timezone':settings[1],'memory_limit':settings[2],'file_backed':True}}
            C.write_json_atomic(generation/'start.json',start)
            scratch=stage/'_scratch'/('build-'+generation_id)
            if scratch.exists():raise ValueError('preserve previous SEC source replay attempt')
            scratch.mkdir(parents=True)
            validation=_replay(con,plan,scratch)
            for name in PRODUCTS:
                partial=generation/(name+'.partial')
                expected=A.pin(scratch/name);shutil.copyfile(scratch/name,partial)
                if A.pin(partial)!=expected:raise ValueError('SEC native output changed while copying')
                os.replace(partial,generation/name)
                A.compare(con,generation/name,scratch/name)
            replay=stage/'_scratch'/('candidate-'+generation_id)
            if replay.exists():raise ValueError('preserve previous SEC candidate replay attempt')
            replay.mkdir(parents=True)
            independently_replayed=_replay(con,plan,replay)
            if independently_replayed!=validation:
                raise ValueError('complete SEC independent replay population differs')
            for name in PRODUCTS:A.compare(con,generation/name,replay/name)
            manifest={'schema':'atx.tier1-sec-original-generation/v1','interface_version':VERSION,
                'status':'complete_diagnostic','files':{name:A.pin(generation/name) for name in PRODUCTS},
                'metadata_files':{'start.json':A.pin(generation/'start.json')},
                'source_dependencies':pins,'validation':validation,'validation_sha256':A.digest(validation),
                'source_quality_certified':False,'readiness':'NOT_PRODUCTION_READY'}
            A.check_pins(pins)
            C.write_json_atomic(generation/'manifest.json',manifest)
            return {'generation':str(generation),'manifest_pin':A.pin(generation/'manifest.json'),
                'product_pins':manifest['files'],'metadata_pins':manifest['metadata_files'],
                'validation_sha256':manifest['validation_sha256'],
                'source_quality_certified':False}
        finally:
            A.check_pins(pins);A.guard(con)
            if spec!=copied:raise ValueError('SEC publication source binding changed')


def admit(spec,con):
    """Reconstruct every immutable source product from independently pinned originals."""
    import pyarrow.parquet as pq
    copied=json.loads(json.dumps(spec));plan,pins=_source(copied,con)
    generation=A.absolute(copied['generation']);stage=A.absolute(copied['stage_root'])
    if generation.parent!=stage/'generations':raise ValueError('fixed immutable SEC generation required')
    if {p.name for p in generation.iterdir()}!=set(PRODUCTS)|{'manifest.json','start.json'}:
        raise ValueError('complete exact immutable SEC generation inventory required')
    manifest_path=generation/'manifest.json'
    if A.pin(manifest_path)!=copied['manifest_pin']:raise ValueError('independent SEC manifest differs')
    manifest=_json(manifest_path,copied['manifest_pin'])
    if set(copied['metadata_pins'])!={'start.json'} or manifest['metadata_files']!=copied['metadata_pins']:
        raise ValueError('independent at-compute SEC provenance pins differ')
    start_pin=manifest['metadata_files']['start.json'];start=_json(generation/'start.json',start_pin)
    if (start.get('schema')!='atx.tier1-sec-original-start/v1'
            or start.get('binding_sha256')!=A.digest(binding(copied)) or start.get('source_dependencies')!=pins
            or start.get('actual_connection',{}).get('threads')!=1
            or start['actual_connection'].get('timezone')!='UTC' or start['actual_connection'].get('file_backed') is not True):
        raise ValueError('at-compute complete SEC source/runtime provenance differs')
    _runtime(start['actual_connection'])
    products={name:generation/name for name in PRODUCTS}
    files={name:A.pin(path) for name,path in products.items()}
    if files!=copied['product_pins']:raise ValueError('independent complete SEC source products differ')
    required={**pins,str(manifest_path):copied['manifest_pin'],str(generation/'start.json'):start_pin,
        **{str(products[k]):v for k,v in files.items()}}
    if required!=copied['dependency_pins']:raise ValueError('complete immutable SEC dependency inventory differs')
    scratch=stage/'_scratch'/('reader-'+uuid.uuid4().hex);scratch.mkdir(parents=True)
    try:
        validation=_replay(con,plan,scratch)
        for name,path in products.items():A.compare(con,path,scratch/name)
        expected={'schema':'atx.tier1-sec-original-generation/v1','interface_version':VERSION,
            'status':'complete_diagnostic','files':files,'source_dependencies':pins,
            'metadata_files':{'start.json':start_pin},
            'validation':validation,'validation_sha256':A.digest(validation),
            'source_quality_certified':False,'readiness':'NOT_PRODUCTION_READY'}
        if manifest!=expected or A.digest(validation)!=copied['expected_validation_sha256']:
            raise ValueError('complete independently expected SEC validation differs')
        return {'status':'admitted_diagnostic','products':products,
            'schemas':{k:str(pq.ParquetFile(v).schema_arrow) for k,v in products.items()},
            'validation':validation,'dependency_pins':required,'spec':copied,'source_quality_certified':False}
    finally:
        A.check_pins(required);A.guard(con)
        if spec!=copied:raise ValueError('SEC reader source binding changed during full replay')


@contextlib.contextmanager
def consuming(admission,con):
    """Whole-consumption immutable byte checks, including early close and errors."""
    copied=json.loads(json.dumps(admission['spec']));_,pins=_source(copied,con)
    generation=A.absolute(copied['generation'])
    required={**pins,str(generation/'manifest.json'):copied['manifest_pin'],
        str(generation/'start.json'):copied['metadata_pins']['start.json'],
        **{str(generation/name):pin for name,pin in copied['product_pins'].items()}}
    if required!=admission['dependency_pins'] or set(copied['product_pins'])!=set(PRODUCTS):
        raise ValueError('complete previously admitted SEC consumption binding differs')
    if admission['products']!={name:generation/name for name in PRODUCTS}:
        raise ValueError('SEC source product redirection refused')
    A.check_pins(required)
    if {p.name for p in generation.iterdir()}!=set(PRODUCTS)|{'manifest.json','start.json'}:
        raise ValueError('immutable SEC generation inventory changed')
    try:yield admission
    finally:
        A.check_pins(required);A.guard(con)
        if admission['spec']!=copied or admission['dependency_pins']!=required:
            raise ValueError('SEC original consumption binding changed')
        if admission['products']!={name:generation/name for name in PRODUCTS} or {p.name for p in generation.iterdir()}!=set(PRODUCTS)|{'manifest.json','start.json'}:
            raise ValueError('SEC original product inventory changed during consumption')
