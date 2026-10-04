"""Complete native Task8 replication, bounded immutable monthly restart and replay."""
from __future__ import annotations
import argparse
import contextlib
import datetime as dt
import json
import math
import os
from pathlib import Path
import uuid
import numpy as np
from . import common as C
from . import release_inputs as R
from . import char_replication_native as N
from . import char_monthly_sources as S
from . import char_monthly_evidence as E
from .char_monthly_registry import FREEZE_SHA256,JKP153

STAGE='char_replication'
VERSION='char-replication-v1-r20'
CONFIG_PATH=Path(__file__).parents[3]/'docs/datasets/char_replication.config.json'
CONFIG=E.read_metadata(CONFIG_PATH)
CONFIG_SHA=R.digest(CONFIG)
MONTHS=tuple(f'{y:04d}-{m:02d}' for y in range(2013,2023) for m in range(1,13))


def code_paths():
    from . import fund_batch_release as F
    factor=[Path(__file__).parent/(name+'.py') for name in ('factors','risk_inputs','_factor_io','_factor_sources','_factor_native')]
    return sorted(set(F.code_paths()+S.code_paths()+factor+[Path(__file__),Path(N.__file__),CONFIG_PATH,Path(__file__).parents[1]/'research/benchmarks_load.py']+[CONFIG_PATH.with_name('char_monthly.'+s+'.json') for s in ('config','formulas','requirements','trials')]))


def statistics(values):
    """Monthly calendar-position mean, IID and calendar-lag HAC6 diagnostics."""
    usable=[(i,float(v)) for i,v in enumerate(values) if Kfinite(v)]
    if not usable:return {'n':0,'mean':None,'iid_se':None,'iid_t':None,'hac_se':None,'hac_t':None,'reason':'no_finite_pairs'}
    n=len(usable);mean=sum(v for _,v in usable)/n
    result={'n':n,'mean':mean,'iid_se':None,'iid_t':None,'hac_se':None,'hac_t':None,'reason':None}
    if n<CONFIG['minimum_pairs']:result['reason']='insufficient_pairs';return result
    if len({v for _,v in usable})==1:result['reason']='zero_variance_undefined_t';return result
    centered={i:v-mean for i,v in usable};variance=sum(v*v for v in centered.values())/(n-1)
    iid=math.sqrt(variance/n)
    longrun=sum(v*v for v in centered.values())/n
    for lag in range(1,7):longrun+=2*(1-lag/7)*sum(v*centered.get(i-lag,0) for i,v in centered.items())/n
    hac=math.sqrt(max(0,longrun)/n)
    for method,se in (('iid',iid),('hac',hac)):
        if se>0:result[method+'_se']=se;result[method+'_t']=mean/se
    if iid<=0:result['reason']='zero_variance_undefined_t'
    return result


def Kfinite(value):return value is not None and not isinstance(value,bool) and isinstance(value,(int,float)) and math.isfinite(value)


def summarize(units,jkp_entries,osap_entries):
    if set(units)!=set(MONTHS) or any(set(u.get('outcomes',{}))!=set(JKP153) or u.get('holding_month')!=m for m,u in units.items()):raise ValueError('complete120month x original153 evidence required')
    outcomes={};osap_names=[n for n,item in osap_entries.items() if item['status']=='mapped']
    if set(jkp_entries)!=set(JKP153) or set(osap_entries)!=set(JKP153) or not osap_names:raise ValueError('complete independent mapping inventory/nonempty OSAP overlap required')
    for name in JKP153:
        monthly=[units[m]['outcomes'][name] for m in MONTHS];row={'requested_months':120,'own_months':sum(Kfinite(v['own']) for v in monthly),'jkp_mapping':jkp_entries[name],'osap_mapping':osap_entries[name]}
        for benchmark in ('jkp','osap'):
            own=[v['own'] if Kfinite(v['own']) and Kfinite(v[benchmark]) else None for v in monthly]
            other=[v[benchmark] if own[i] is not None else None for i,v in enumerate(monthly)]
            pairs=[(x,y) for x,y in zip(own,other) if x is not None]
            rho=E._pearson(pairs) if len(pairs)>=60 and len({x for x,y in pairs})>1 and len({y for x,y in pairs})>1 else None
            if not Kfinite(rho):rho=None
            ours,theirs=statistics(own),statistics(other)
            row[benchmark]={'pairs':len(pairs),'rho':rho,'own':ours,'benchmark':theirs,
                'mean_sign_agrees':len(pairs)>=60 and ours['mean'] is not None and theirs['mean'] is not None and ours['mean']*theirs['mean']>0,
                'status':'testable' if rho is not None else 'missing_or_insufficient_or_constant; denominator_retained'}
        row['missing_reasons']={m:units[m]['outcomes'][name]['reason'] for m in MONTHS if units[m]['outcomes'][name]['reason']}
        outcomes[name]=row
    good=sum(v['jkp']['rho'] is not None and v['jkp']['rho']>=.7 for v in outcomes.values())
    signs=sum(v['jkp']['mean_sign_agrees'] for v in outcomes.values())
    values=[outcomes[n]['osap']['rho'] for n in osap_names if outcomes[n]['osap']['rho'] is not None]
    median=float(np.median(values)) if values else None
    gates={'jkp_matched_rho_share':{'pass':good/153>=.7,'measured':good/153,'numerator':good,'denominator':153,'required':.7,'rho_threshold':.7},
        'jkp_mean_sign_share':{'pass':signs/153>=.9,'measured':signs/153,'numerator':signs,'denominator':153,'required':.9},
        'osap_median_rho':{'pass':len(values)==len(osap_names) and median is not None and median>=.6,'measured':median,'matched':len(values),'denominator':len(osap_names),'required':.6}}
    return {'stage':STAGE,'version':VERSION,'registry_sha256':FREEZE_SHA256,'config_sha256':CONFIG_SHA,'readiness':'NOT_PRODUCTION_READY','source_quality_certified':False,
        'requested_slots':[{'holding_month':m,'formation':units[m]['formation'],'population':units[m]['population'],'benchmark_counts':units[m]['benchmark_counts']} for m in MONTHS],
        'outcomes':outcomes,'gates':gates,'gate_status':{n:'PASS' if v['pass'] else 'FAIL' for n,v in gates.items()},
        'interpretation':'monthly benchmark close-to-close replication only; no executable trade, net-profitability or Task16 predictive significance claim'}


def immutable(path,value):
    path.parent.mkdir(parents=True,exist_ok=True);tmp=path.with_name(path.name+'.'+uuid.uuid4().hex+'.partial')
    with tmp.open('x',encoding='utf-8') as stream:
        json.dump(value,stream,sort_keys=True,indent=2,default=str,allow_nan=False);stream.write('\n');stream.flush();os.fsync(stream.fileno())
    os.link(tmp,path);tmp.unlink()


@contextlib.contextmanager
def writer_lock(stage):
    stage.mkdir(parents=True,exist_ok=True);path=stage/'.writer.lock'
    try:fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_WRONLY)
    except FileExistsError:raise RuntimeError('active/interrupted Task8 writer; controller inspection required') from None
    try:os.write(fd,str(os.getpid()).encode());yield
    finally:os.close(fd);path.unlink()


def generation_path(root,name):
    if not isinstance(name,str) or not name or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-' for c in name):raise ValueError('safe explicit generation required')
    return root/STAGE/'generations'/name


def freeze(root,binding_path,con,expected_bindings_sha256):
    from . import factors
    if E.read_metadata(CONFIG_PATH)!=CONFIG or CONFIG['registry_sha256']!=FREEZE_SHA256 or CONFIG['original153']!=list(JKP153):raise ValueError('frozen Task8/Task7 semantics differ')
    if factors.config_hash()!=CONFIG['upstream_factor_config_sha256']:raise ValueError('frozen Task6 historical predictor semantics differ')
    if not isinstance(expected_bindings_sha256,str) or len(expected_bindings_sha256)!=64 or any(c not in '0123456789abcdef' for c in expected_bindings_sha256):raise ValueError('independent caller expected binding SHA256 required')
    if not binding_path.is_absolute() or '..' in binding_path.parts:raise ValueError('absolute unambiguous controller binding path required')
    binding_path=binding_path.resolve();binding_pin=R.identity(binding_path)
    if binding_pin['sha256']!=expected_bindings_sha256:raise ValueError('controller binding differs from external caller admission')
    binding=E.read_metadata(binding_path)
    code={str(p.resolve()):R.identity(p) for p in code_paths()}
    context=N.admit(root,binding,con,CONFIG_SHA)
    # Exact native products, contracts and every independently admitted source/proof
    # member are pinned before any portfolio/statistical work.
    pins={}
    def producer_members(target):
        if target.suffix!='.json':return
        for relative,pin in E.read_metadata(target).get('files',{}).items():
            if not isinstance(pin,dict) or not {'sha256','bytes'}<=set(pin):raise ValueError('abbreviated producer member identity')
            member=R.safe_path(target.parent,relative);identity={'sha256':pin['sha256'],'bytes':pin['bytes']}
            if R.identity(member)!=identity:raise ValueError('producer member bytes changed')
            pins[str(member)]=identity
    def collect(value):
        if not isinstance(value,dict):return
        if {'path','identity','expected_sha256'}<=set(value):
            target=N.review_file(root,value);pins[str(target)]=value['identity']
            if target.suffix=='.json':collect(E.read_metadata(target))
            return
        if set(value)=={'path','sha256','bytes'}:
            target=N.path(root,value['path']);pin={k:value[k] for k in ('sha256','bytes')}
            if R.identity(target)!=pin:raise ValueError('external source manifest bytes changed')
            pins[str(target)]=pin;producer_members(target);return
        for key,item in value.items():
            if key in ('files','metadata','manifests') and isinstance(item,dict):
                for relative,pin in item.items():
                    target=N.path(root,relative)
                    if R.identity(target)!=pin:raise ValueError('source/proof inventory bytes changed')
                    pins[str(target)]=pin
                    if key=='manifests':producer_members(target)
            else:
                if isinstance(item,dict):collect(item)
                elif isinstance(item,list):
                    for member in item:collect(member)
    collect(binding)
    for name in ('manifest.json','snapshot.json','validation.json'):
        target=context['generation']/name;pins[str(target)]=R.identity(target)
    collect(E.read_metadata(context['generation']/'snapshot.json'))
    for relative,pin in context['feature'].get('receipts',{}).items():pins[str(context['generation']/relative)]=pin
    for relative,pin in context['feature']['files'].items():pins[str(context['generation']/relative)]=pin
    for file,pin in pins.items():
        if R.identity(Path(file))!=pin:raise ValueError('source changed during admission')
    if binding_pin!=R.identity(binding_path) or code!={str(p.resolve()):R.identity(p) for p in code_paths()}:raise ValueError('binding/code changed during admission')
    snapshot={'version':VERSION,'config':CONFIG,'config_sha256':CONFIG_SHA,'registry_sha256':FREEZE_SHA256,'binding_path':str(binding_path),'binding_identity':binding_pin,'expected_bindings_sha256':expected_bindings_sha256,'binding':binding,'feature_manifest':binding['feature_manifest'],'code':code,'inputs':pins}
    return snapshot,context


def recheck(snapshot):
    if snapshot.get('version')!=VERSION or snapshot.get('config')!=CONFIG or snapshot.get('registry_sha256')!=FREEZE_SHA256 or set(snapshot.get('code',{}))!={str(p.resolve()) for p in code_paths()}:raise ValueError('abbreviated/stale Task8 snapshot')
    if R.identity(Path(snapshot['binding_path']))!=snapshot['binding_identity']:raise ValueError('binding changed')
    for inventory in ('code','inputs'):
        for file,pin in snapshot[inventory].items():
            if R.identity(Path(file))!=pin:raise ValueError('source/code snapshot changed')


def monthly_receipt(root,snapshot,context,con,holding):
    if holding not in MONTHS:raise ValueError('exact120monthly replication axis required')
    recheck(snapshot)
    result=N.month(root,snapshot['binding'],context,con,holding)
    result.update(version=VERSION,snapshot_sha256=R.digest(snapshot))
    recheck(snapshot)
    return result


def require_guard():
    if not os.environ.get('ATX_GUARD_JOB'):raise RuntimeError('data/evaluation requires root exclusive memory guard')


def build(root,binding_path,name,max_new_months=None,expected_bindings_sha256=None):
    require_guard();root=root.resolve()
    if max_new_months is not None and (type(max_new_months) is not int or max_new_months<1):raise ValueError('positive bounded monthly budget required')
    generation=generation_path(root,name);stage=root/STAGE
    with writer_lock(stage):
        if (generation/'manifest.json').exists():raise ValueError('immutable completed Task8 generation')
        con=C.connect(memory='128MB',threads=1,temp_dir=stage/'_spill',db_file='char-replication-'+uuid.uuid4().hex+'.duckdb')
        try:
            snapshot,context=freeze(root,binding_path,con,expected_bindings_sha256);snap_path=generation/'snapshot.json'
            if snap_path.exists():
                if R.digest(E.read_metadata(snap_path))!=R.digest(snapshot):raise ValueError('stale Task8 restart snapshot')
            else:
                if generation.exists() and any(generation.iterdir()):raise ValueError('unsealed candidate without source snapshot')
                immutable(snap_path,snapshot)
            units={};new=0
            for holding in MONTHS:
                receipt=generation/'months'/holding/'receipt.json'
                if not receipt.exists() and max_new_months is not None and new>=max_new_months:
                    return {'status':'partial_cache_only','published':False,'complete_months':len(units),'required_months':120}
                if not receipt.exists():
                    attempt=receipt.parent/'attempts'/uuid.uuid4().hex
                    immutable(attempt/'start.json',{'version':VERSION,'snapshot_sha256':R.digest(snapshot),'holding_month':holding})
                unit=monthly_receipt(root,snapshot,context,con,holding)
                if receipt.exists():
                    if R.digest(E.read_metadata(receipt))!=R.digest(unit):raise ValueError('resumed monthly receipt differs from native economic replay')
                else:
                    immutable(receipt,unit);new+=1
                units[holding]=unit
            report=summarize(units,context['jkp_doc']['entries'],context['osap_doc']['entries'])
            recheck(snapshot);immutable(generation/'char_replication.json',report)
            manifest={'stage':STAGE,'version':VERSION,'status':'complete','config_sha256':CONFIG_SHA,'registry_sha256':FREEZE_SHA256,'readiness':'NOT_PRODUCTION_READY','source_quality_certified':False,
                'snapshot':R.identity(snap_path),'report':R.identity(generation/'char_replication.json'),'months':{m:R.identity(generation/'months'/m/'receipt.json') for m in MONTHS},'gate_status':report['gate_status']}
            immutable(generation/'manifest.json',manifest)
            validate_generation(root,generation,con,expected_bindings_sha256)
            C.write_json_atomic(stage/'current.json',{'generation':name,'manifest':R.identity(generation/'manifest.json')})
            C.write_json_atomic(root/'validation/char_replication.json',{'generation':name,'manifest':R.identity(generation/'manifest.json'),'report':report})
            return {'status':'candidate_published','published':True,'gate_status':manifest['gate_status'],'readiness':'NOT_PRODUCTION_READY'}
        finally:con.close()


def validate_generation(root,generation,con,expected_bindings_sha256=None):
    require_guard();root=root.resolve();generation=generation.resolve()
    if generation!=generation_path(root,generation.name).resolve():raise ValueError('Task8 generation escapes exact stage')
    manifest_pin=R.identity(generation/'manifest.json');manifest=E.read_metadata(generation/'manifest.json');snapshot=E.read_metadata(generation/'snapshot.json')
    if {p.name for p in generation.iterdir()}!={'manifest.json','snapshot.json','char_replication.json','months'} or {p.name for p in (generation/'months').iterdir()}!=set(MONTHS):raise ValueError('incomplete/extra full Task8 generation inventory')
    current,context=freeze(root,Path(snapshot['binding_path']),con,expected_bindings_sha256)
    if R.digest(current)!=R.digest(snapshot):raise ValueError('Task8 source/config/code snapshot stale')
    expected={'stage':STAGE,'version':VERSION,'status':'complete','config_sha256':CONFIG_SHA,'registry_sha256':FREEZE_SHA256,'readiness':'NOT_PRODUCTION_READY','source_quality_certified':False}
    if any(manifest.get(k)!=v for k,v in expected.items()) or manifest.get('source_quality_certified') is not False:raise ValueError('unsupported Task8 qualification')
    if set(manifest.get('months',{}))!=set(MONTHS) or manifest.get('snapshot')!=R.identity(generation/'snapshot.json') or manifest.get('report')!=R.identity(generation/'char_replication.json'):raise ValueError('incomplete/tampered Task8 metadata')
    units={}
    for holding in MONTHS:
        path=generation/'months'/holding/'receipt.json'
        if manifest['months'][holding]!=R.identity(path):raise ValueError('Task8 monthly receipt bytes changed')
        unit=monthly_receipt(root,snapshot,context,con,holding)
        if R.digest(E.read_metadata(path))!=R.digest(unit):raise ValueError('Task8 monthly economic evidence differs from native sources')
        units[holding]=unit
    report=summarize(units,context['jkp_doc']['entries'],context['osap_doc']['entries'])
    if R.digest(E.read_metadata(generation/'char_replication.json'))!=R.digest(report) or manifest['gate_status']!=report['gate_status']:raise ValueError('Task8 original report/gates differ from full native replay')
    recheck(snapshot)
    if manifest_pin!=R.identity(generation/'manifest.json') or manifest['snapshot']!=R.identity(generation/'snapshot.json') or manifest['report']!=R.identity(generation/'char_replication.json') or any(manifest['months'][m]!=R.identity(generation/'months'/m/'receipt.json') for m in MONTHS):raise ValueError('Task8 changed during public validation')
    return manifest


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command',choices=('contract','build','validate'));parser.add_argument('--root',type=Path);parser.add_argument('--bindings',type=Path);parser.add_argument('--expected-bindings-sha256');parser.add_argument('--generation');parser.add_argument('--max-new-months',type=int)
    args=parser.parse_args(argv)
    if args.command=='contract':print(json.dumps(CONFIG,indent=2));return
    require_guard()
    if args.root is None or args.generation is None or args.expected_bindings_sha256 is None:parser.error('explicit root/generation and independently pinned binding SHA256 required')
    if args.command=='build':
        if args.bindings is None:parser.error('explicit reviewed bindings required')
        print(build(args.root,args.bindings,args.generation,args.max_new_months,args.expected_bindings_sha256));return
    stage=args.root.resolve()/STAGE;con=C.connect(memory='128MB',threads=1,temp_dir=stage/'_spill',db_file='char-replication-check-'+uuid.uuid4().hex+'.duckdb')
    try:print(validate_generation(args.root,generation_path(args.root.resolve(),args.generation),con,args.expected_bindings_sha256))
    finally:con.close()


if __name__=='__main__':main()
