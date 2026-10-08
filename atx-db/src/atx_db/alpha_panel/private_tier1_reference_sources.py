"""Fixed French and FRED original-byte admission and native publication.

The two complete fixed families retain original economic/unit/clock semantics.
Deleted French ZIPs and current-vintage FRED revision risk remain explicit;
engineering review is never historical source-vintage certification.
"""
import contextlib
import json
import os
import re
import shutil
import uuid
from pathlib import Path
from . import common as C, reference as R, fund_batch_release as F
from . import private_tier1_admission as A, private_tier1_reference_replay as N

VERSION='tier1-french-sources-v1'
PRODUCTS=tuple(sorted({key+'.parquet' for key in R.FRENCH_FACTOR_FILES}|{'siccodes.parquet'}))
FRED_VERSION='tier1-fred-sources-v1'
FRED_PRODUCTS=('fx_daily.parquet','rates_daily.parquet','series_catalog.parquet','vix_daily.parquet')
EXECUTION={'version':VERSION,'memory_limit':'128MB','threads':1,'timezone':'UTC',
           'file_backed':True,'process_guard_max_gib':1.0,'actual_initial_guard_gib':0.8,
           'disk_floor_gib':40,'full_original_replay':True,'source_quality_certified':False}


def _family(spec):
    version=spec['interface_version']
    if version==VERSION:return 'french',PRODUCTS,N.replay_french_sources
    if version==FRED_VERSION:return 'fred',FRED_PRODUCTS,N.replay_fred_sources
    raise ValueError('fixed original reference source family required')


def code_pins():
    return A.code(('private_tier1_reference_sources','private_tier1_reference_replay',
        'private_tier1_factor_replay','private_tier1_admission','reference',
        'classification','shortflow_common','common','fund_batch_release'))


def binding(spec):
    result={key:spec[key] for key in ('interface_version','plan_path','expected_plan_sha256',
        'execution_contract','expected_execution_contract_sha256','expected_code_sha256','stage_root')}
    for key in ('generation','manifest_pin','product_pins','expected_validation_sha256'):
        if key in spec:result[key]=spec[key]
    return result


def _source(spec,con):
    A.guard(con)
    family,required,_=_family(spec);version=spec['interface_version']
    plan_path=A.absolute(spec['plan_path']);execution=A.absolute(spec['execution_contract'])
    code=code_pins()
    if A.pin(execution)['sha256']!=spec['expected_execution_contract_sha256'] or C.read_json(execution)!={**EXECUTION,'version':version}:
        raise ValueError('independent French execution contract differs')
    if A.digest(code)!=spec['expected_code_sha256']:
        raise ValueError('independent actual French adapter code differs')
    if A.pin(plan_path)['sha256']!=spec['expected_plan_sha256']:
        raise ValueError('independent original French plan differs')
    plan=C.read_json(plan_path)
    if set(plan)!={'schema','source_root','ledger','product_root','native_dependency_pins'} or plan['schema']!='atx.tier1-'+family+'-source-plan/v1':
        raise ValueError('complete fixed French source plan required')
    source_root=A.absolute(plan['source_root']);ledger=A.member(source_root,plan['ledger'])
    product_root=A.absolute(plan['product_root'])
    products={name:product_root/name for name in required}
    review=spec['independent_review'];review_path=A.absolute(review['path'])
    if A.pin(review_path)['sha256']!=review['expected_sha256']:
        raise ValueError('independent French review evidence differs')
    expected={'schema':'atx.tier1-'+family+'-review/v1','status':'scoped_pass',
        'interface_version':version,'binding_sha256':A.digest(binding(spec)),'code_pins':code}
    if C.read_json(review_path)!=expected:
        raise ValueError('reviewed fixed French code/source binding required')
    complete={**plan['native_dependency_pins'],**code,str(plan_path):A.pin(plan_path),
        str(execution):A.pin(execution),str(review_path):A.pin(review_path)}
    if complete!=spec['dependency_pins']:
        raise ValueError('independent complete French source/code inventory differs')
    A.check_pins(complete)
    return plan,source_root,ledger,products,complete


def _native(con,plan,source_root,ledger,products,scratch):
    # The unchanged source phase independently derives and compares the entire
    # fourteen-source/seven-product inventory, including last failed receipts.
    replay=N.replay_french_sources if plan['schema']=='atx.tier1-french-source-plan/v1' else N.replay_fred_sources
    return replay(con,source_root,ledger,products,scratch,plan['native_dependency_pins'])


def materialize(spec,con,generation_id):
    """Controller-only complete source replay then immutable native output.

    No active-pointer change or source gate promotion. A failed attempt is kept;
    an existing generation requires independently pinned reader admission and
    is never blindly reused/resealed.
    """
    plan,source_root,ledger,products,complete=_source(spec,con)
    family,required,_=_family(spec);version=spec['interface_version']
    if not re.fullmatch('[A-Za-z0-9_-]+',generation_id):raise ValueError('invalid immutable French generation')
    stage=A.absolute(spec['stage_root']);generation=A.member(stage,'generations/'+generation_id)
    with F.writer_lock(stage):
        try:
            manifest_path=generation/'manifest.json'
            if generation.exists():
                raise ValueError('preserve existing French generation; admit with independent final pins')
            generation.mkdir(parents=True)
            C.write_json_atomic(generation/'source-plan.json',plan)
            replay=_native(con,plan,source_root,ledger,products,stage/'_scratch'/generation_id/'source')
            # These files were computed from original members by unchanged
            # native kernels, with full typed comparison to retained outputs.
            for name in required:
                expected=stage/'_scratch'/generation_id/'source'/name
                partial=generation/(name+'.partial')
                shutil.copyfile(expected,partial)
                if A.pin(partial)!=replay['validation'][name]['expected']:
                    raise ValueError('new immutable French output changed during copy')
                os.replace(partial,generation/name)
            files={name:A.pin(generation/name) for name in required}
            manifest={'stage':'reference','schema':'atx.tier1-'+family+'-generation/v1',
                'status':'complete','interface_version':version,'source_plan_sha256':spec['expected_plan_sha256'],
                'source_dependencies':{p:v for p,v in complete.items() if p!=str(A.absolute(spec['independent_review']['path']))},
                'producer_review':{'path':str(A.absolute(spec['independent_review']['path'])),
                    'identity':complete[str(A.absolute(spec['independent_review']['path']))]},
                'files':files,'code':code_pins(),
                'metadata_files':{'source-plan.json':A.pin(generation/'source-plan.json')},
                'source_quality_certified':False,'readiness':'NOT_PRODUCTION_READY',
                'gate_status':{'native_full_replay':'PASS','original_source_vintage':'FAIL'},
                'source_facts':replay['sources']}
            # Reconstruct and compare candidate rows again before sealing.
            _candidate_replay(con,plan,source_root,ledger,products,generation,files,stage/'_scratch'/generation_id/'candidate')
            A.check_pins(complete)
            C.write_json_atomic(manifest_path,manifest)
            return {'generation':str(generation),'manifest_pin':A.pin(manifest_path),
                'product_pins':files,'source_quality_certified':False,'publication':False}
        finally:
            A.check_pins(complete)


def _candidate_replay(con,plan,source_root,ledger,original_products,generation,files,scratch):
    native=dict(plan['native_dependency_pins'])
    for path in original_products.values():native.pop(str(path.resolve()))
    products={name:generation/name for name in original_products}
    native.update({str(products[name]):files[name] for name in products})
    replay=N.replay_french_sources if plan['schema']=='atx.tier1-french-source-plan/v1' else N.replay_fred_sources
    return replay(con,source_root,ledger,products,scratch,native)


def admit(spec,con):
    plan,source_root,ledger,products,complete=_source(spec,con)
    family,required,_=_family(spec);version=spec['interface_version']
    stage=A.absolute(spec['stage_root']);generation=A.absolute(spec['generation'])
    if generation.parent!=stage/'generations':raise ValueError('fixed immutable French generation required')
    path=generation/'manifest.json'
    if A.pin(path)!=spec['manifest_pin']:raise ValueError('independent French manifest differs')
    manifest=C.read_json(path)
    if (manifest.get('schema')!='atx.tier1-'+family+'-generation/v1' or manifest.get('stage')!='reference'
            or manifest.get('status')!='complete' or manifest.get('interface_version')!=version
            or manifest.get('source_quality_certified') is not False
            or manifest.get('source_dependencies')!={p:v for p,v in complete.items() if p!=str(A.absolute(spec['independent_review']['path']))}
            or manifest.get('source_plan_sha256')!=spec['expected_plan_sha256'] or manifest.get('code')!=code_pins()
            or manifest.get('files')!=spec['product_pins'] or set(spec['product_pins'])!=set(required)
            or C.read_json(generation/'source-plan.json')!=plan
            or A.pin(generation/'source-plan.json')!=manifest.get('metadata_files',{}).get('source-plan.json')):
        raise ValueError('complete original French publication binding differs')
    consumed={**complete,str(path):spec['manifest_pin'],str(generation/'source-plan.json'):manifest['metadata_files']['source-plan.json'],
        **{str(generation/name):spec['product_pins'][name] for name in required}}
    producer_review=manifest['producer_review'];review_path=A.absolute(producer_review['path'])
    consumed[str(review_path)]=producer_review['identity']
    # Preserve and authenticate the original pre-publication source/code review
    # as well as the separate caller-pinned final generation review.
    producer_spec={key:value for key,value in spec.items() if key not in ('generation','manifest_pin','product_pins','expected_validation_sha256')}
    if C.read_json(review_path)!={'schema':'atx.tier1-'+family+'-review/v1','status':'scoped_pass',
            'interface_version':version,'binding_sha256':A.digest(binding(producer_spec)),'code_pins':code_pins()}:
        raise ValueError('original French producer review binding differs')
    A.check_pins(consumed)
    try:
        attempt=stage/'_scratch'/generation.name/('reader-'+uuid.uuid4().hex)
        source=_native(con,plan,source_root,ledger,products,attempt/'source')
        validation=_candidate_replay(con,plan,source_root,ledger,products,generation,spec['product_pins'],attempt/'candidate')
        if manifest['source_facts']!=source['sources'] or A.digest(validation)!=spec['expected_validation_sha256']:
            raise ValueError('independent full French reader replay differs')
        return {'status':'admitted_diagnostic','products':{name:generation/name for name in required},
            'validation':validation,'dependency_pins':consumed,'source_quality_certified':False,'spec':spec}
    finally:
        A.check_pins(consumed)


@contextlib.contextmanager
def consuming(admission,con):
    """Whole consumption integrity; never source-sized hashing per microbatch."""
    A.guard(con)
    _,required,_=_family(admission['spec'])
    expected={name:A.absolute(admission['spec']['generation'])/name for name in required}
    if admission['products']!=expected:raise ValueError('redirected fixed French product')
    spec=admission['spec']
    # Final review binds every externally expected candidate identity. Do not
    # let an invented mutable admission redirect bytes under a source-only review.
    _,_,_,_,source_pins=_source(spec,con)
    pins=dict(admission['dependency_pins'])
    for name,path in expected.items():
        if pins.get(str(path))!=spec['product_pins'][name]:raise ValueError('French consumed product identity differs')
    if any(pins.get(path)!=identity for path,identity in source_pins.items()):
        raise ValueError('French consumption omits complete native source identity')
    if A.digest(admission['validation'])!=spec['expected_validation_sha256']:
        raise ValueError('mutable French validation binding differs')
    sealed_binding=A.digest(binding(spec))
    sealed_validation=A.digest(admission['validation'])
    fixed_products=dict(expected)
    A.check_pins(pins)
    try:yield dict(fixed_products)
    finally:
        A.check_pins(pins)
        A.guard(con)
        if (A.digest(binding(admission['spec']))!=sealed_binding
                or A.digest(admission['validation'])!=sealed_validation
                or admission['products']!=fixed_products
                or admission['dependency_pins']!=pins):
            raise ValueError('French consumption binding changed')
