"""Strict Task17 catalog integration over quiescent, actual-byte publications.

Source qualification and economic gates remain separate from catalog integrity.
No old generation glob, parser-phase feature substitution or skipped output passes.
"""
from __future__ import annotations
import argparse
from dataclasses import replace
import importlib
from pathlib import Path
import os
import uuid

from atx_db.stagelake import registry, catalog, verify
from atx_db.stagelake.contract import Stage, Output, output_files, recorded_files
from . import common as C, private_tier1_admission as A, tier1_native_readers as T

VERSION = 'tier1-catalog-v1'
DATASETS = Path(__file__).resolve().parents[3]/'docs/datasets'
STUBS = ('market_liquidity','market_index','share_exchange_history','universe','compustat_std',
         'ratios','buyback','char_monthly','char_replication','risk_inputs','factors',
         'earnings_calendar_v2','ea_actuals','thirteenf_quality','thirteenf_cohorts',
         'thirteenf_native','thirteenf_features','nport','nport_vintages','mna_v2',
         'delisting_v2','distributions','fundamentals_notes','guidance','insider_ext',
         'golden_panel','golden_labels','golden_evaluation','golden_export')
LANES = {'compustat_std':'FUND','ratios':'FUND','buyback':'FUND','fundamentals_notes':'FUND',
         'char_monthly':'CHAR','char_replication':'CHAR','risk_inputs':'CHAR','factors':'CHAR',
         'earnings_calendar_v2':'EVT','ea_actuals':'EVT','mna_v2':'EVT','delisting_v2':'EVT',
         'distributions':'EVT','share_exchange_history':'ID','universe':'ID'}


def code_pins():
    names = ('atx_db.alpha_panel.tier1_catalog','atx_db.stagelake.catalog',
             'atx_db.stagelake.registry','atx_db.stagelake.contract','atx_db.stagelake.verify')
    result = {str(Path(importlib.import_module(n).__file__).resolve()): A.pin(importlib.import_module(n).__file__) for n in names}
    result.update({str(DATASETS/(s+'.registry.json')):A.pin(DATASETS/(s+'.registry.json')) for s in STUBS})
    return result


def declarations():
    """Fold all plan family stubs into an explicit serving registry, without imports."""
    existing = {s.name:s for s in registry.STAGES}
    for stage in registry.discover():
        if stage.name in existing and existing[stage.name] != stage:
            raise ValueError('conflicting native registry declaration: '+stage.name)
        existing[stage.name] = stage
    for name in STUBS:
        stub = C.read_json(DATASETS/(name+'.registry.json'))
        if name in existing: continue
        outputs = tuple(Output.from_value({k:v for k,v in o.items() if k in Output.__dataclass_fields__}) for o in stub.get('outputs', ()))
        if not outputs:
            path = stub.get('path')
            if name == 'compustat_std':
                outputs = tuple(Output(name+'/'+p+'.parquet', name+'_'+p, keys=('cik','datadate'),order=('accession',),vintage='vintage') for p in ('fundq','funda'))
            elif name == 'ratios':
                outputs = (Output('ratios/monthly/year=*/ratios_monthly.parquet','ratios',keys=('security_id','session_date'),vintage='daily'),)
            elif name=='guidance':
                outputs=tuple(Output('events_guidance_v2/'+p+'.parquet','guidance_'+p,keys=('cik','accession','candidate_id')) for p in ('guidance','guidance_features'))
            elif name.startswith('golden_'):
                outputs=(Output(name+'/generations/<explicit-g>/*.parquet',name,clock='source_available_at' if name=='golden_panel' else 'available_at',keys=tuple(stub['keys'])),)
            elif path:
                if path.startswith('data/alpha_panel/v1/'): path = path[len('data/alpha_panel/v1/'):]
                clock = None if not path.endswith('.parquet') else stub.get('clock','available_at')
                if name == 'ea_actuals': path='ea_actuals/ann_features.parquet'
                if name == 'earnings_calendar_v2': path='earnings_calendar_v2/announcements.parquet'
                keys = tuple(stub.get('keys', ())) if isinstance(stub.get('keys'),list) else ()
                outputs = (Output(path,name,clock=clock,keys=keys),)
        existing[name] = Stage(name,LANES.get(name,'OWN' if name.startswith(('thirteenf','nport')) else 'MKT'),
            stub.get('schema',stub.get('schema_id')),None,outputs=outputs,
            manifest=stub.get('manifest',stub.get('manifest_path',('events_guidance_v2' if name=='guidance' else name)+'/manifest.json')),
            guard_gb=.8,doc='Task17 data-only integration; producer/source gates remain binding')
    # New accounting aliases intentionally coexist with original preserved v10.
    existing['compustat_std_v11'] = Stage('compustat_std_v11','FUND',None,None,doc='Built by tier1_native_readers fixed native orchestration',
        outputs=tuple(Output('compustat_std_v11/'+p+'.parquet','standardized_v11_'+p,keys=('cik','datadate'),order=('accession',),vintage='vintage') for p in ('fundq','funda')))
    existing['ratios_v11'] = Stage('ratios_v11','FUND',None,None,doc='Built by tier1_native_readers fixed native orchestration',
        outputs=(Output('ratios_v11/monthly/year=*/ratios_monthly.parquet','ratios_v11',keys=('session_date','security_id'),vintage='daily'),))
    existing['fundamentals_v11'] = Stage('fundamentals_v11','FUND',None,None,doc='Built by independently reviewed R23 full native runtime',
        outputs=tuple(Output('fundamentals_v11/'+p+'.parquet','fundamentals_v11_'+p,keys=('cik',),vintage='event') for p in ('events','catalog','sic','quarterly_history')))
    existing['text_native'] = Stage('text_native','TXT','atx.text-native-generation/v1',None,doc='Built by full original-byte text_features_native publication',
        outputs=(Output('text_native/text/features.parquet','text_native',keys=('cik','accession')),
                 Output('text_native/classification_tnic/pairs.parquet','tnic_native_pairs',keys=('year','cik','peer_cik')),
                 Output('text_native/classification_tnic/firms.parquet','tnic_native_firms',keys=('year','cik')),
                 Output('text_native/classification_tnic/years.parquet','tnic_native_years',keys=('year',))))
    return registry.validate(list(existing.values()))


def _immutable(root, stage, manifest):
    directory = (root/stage.manifest).parent
    if directory.parent.name=='generations':directory=directory.parent.parent
    if stage.name=='fundamentals_v11':
        from . import fund_batch_release as F
        if (root/F.STAGE/'.writer.lock').exists():raise ValueError('financial producer is not quiescent')
    for flag in ('.writer.lock','writer.lock','activation.json','pointer-transaction.json'):
        if (directory/flag).exists(): raise ValueError('producer is not quiescent: '+stage.name+'/'+flag)
    if stage.name in ('compustat_std_v11','ratios_v11'):
        generation = T.resolve_current(directory)
    elif stage.name in ('ea_actuals','earnings_calendar_v2'):
        from . import ea_actuals as E
        phases = ('features','validated') if stage.name=='ea_actuals' else ('calendar',)
        if root != C.build_root().resolve(): raise ValueError('Task9 catalog root differs from native reader root')
        E.verify_input(stage.name,required_phase=phases)
        generation = A.member(directory,manifest['generation'])
        if generation.parent != directory/'generations' or C.read_json(generation/'manifest.json') != manifest:
            raise ValueError('Task9 activated immutable manifest differs')
    elif manifest.get('generation'):
        generation = A.member(directory,manifest['generation'])
        if not generation.is_dir(): raise ValueError('manifest generation absent: '+stage.name)
    else: return stage
    products = manifest.get('products')
    if products:
        resolved=[]
        for out in stage.outputs:
            leaf = Path(out.glob).name
            candidates = [p for p in products.values() if Path(p).name==leaf]
            if not candidates: raise ValueError('required phase product absent: '+stage.name+'/'+leaf)
            if len(candidates)>1:
                glob=(generation/Path(out.glob).relative_to(directory.relative_to(root))).relative_to(root).as_posix()
                actual={str(p.resolve()) for p in root.glob(glob)}
                expected={str(A.member(directory,p)) for p in candidates}
                if actual!=expected: raise ValueError('immutable product glob differs from full manifest')
                resolved.append(replace(out,glob=glob))
                continue
            for i,p in enumerate(sorted(candidates)):
                path=A.member(directory,p)
                if not path.is_relative_to(generation): raise ValueError('manifest product escapes activated generation')
                resolved.append(replace(out,glob=path.relative_to(root).as_posix(),view=out.view if len(candidates)==1 else out.view+'_'+str(i)))
        return replace(stage,outputs=tuple(resolved))
    files=recorded_files(manifest,directory)
    if not files: raise ValueError('immutable stage lacks actual product inventory')
    paths=sorted(p for p in files if p.endswith('.parquet'))
    if len(stage.outputs)==1 and any(mark in stage.outputs[0].glob for mark in ('{manifest.generation}','<explicit-g>','explicit-generation')):
        # Explicit immutable file inventories, never an unbounded generations glob.
        return replace(stage,outputs=tuple(replace(stage.outputs[0],glob=(directory/p).relative_to(root).as_posix(),
            view=stage.outputs[0].view+'_'+Path(p).stem) for p in paths))
    return stage


def resolve(root, stages, native_text=None,publications=None):
    result=[]
    for stage in stages:
        if stage.name in (publications or {}):
            binding=publications[stage.name]
            path=A.member(root,binding['manifest'])
            if A.pin(path)!=binding['manifest_pin']:raise ValueError('independent publication manifest identity differs')
            manifest=C.read_json(path);files=recorded_files(manifest,path.parent)
            if not files:raise ValueError('complete immutable publication inventory absent')
            outputs=[]
            for out in stage.outputs:
                leaf=Path(out.glob).name
                matching=[p for p in files if Path(p).name==leaf or (leaf=='*.parquet' and p.endswith('.parquet'))]
                if not matching:raise ValueError('required explicit publication product missing')
                for i,p in enumerate(sorted(matching)):
                    outputs.append(replace(out,glob=A.member(path.parent,p).relative_to(root).as_posix(),view=out.view if len(matching)==1 else out.view+'_'+str(i)))
            stage=replace(stage,manifest=path.relative_to(root).as_posix(),outputs=tuple(outputs))
        if stage.name=='text_native':
            if native_text is None:raise ValueError('complete native text admission contract required')
            generation=A.member(root,native_text['generation'])
            stage=replace(stage,manifest=(generation/'manifest.json').relative_to(root).as_posix(),
                outputs=tuple(replace(o,glob=(generation/Path(o.glob).relative_to('text_native')).relative_to(root).as_posix()) for o in stage.outputs))
        if stage.name == 'char_monthly':
            from . import char_monthly as M
            generation=M.resolve_current(root)
            stage=replace(stage,manifest=(generation/'manifest.json').relative_to(root).as_posix(),
                outputs=tuple(replace(o,glob=(generation/'month=*'/'char_monthly.parquet').relative_to(root).as_posix()) for o in stage.outputs))
        if stage.name == 'fundamentals_v11':
            from . import fund_batch_release as F
            leaf=F.resolve_current(root/F.STAGE)/F.SEMANTIC_STAGE
            stage=replace(stage,manifest=(leaf/'manifest.json').relative_to(root).as_posix(),
                outputs=tuple(replace(o,glob=(leaf/Path(o.glob).name).relative_to(root).as_posix()) for o in stage.outputs))
        manifest_path=A.member(root,stage.manifest)
        if not manifest_path.is_file(): raise ValueError('required catalog stage manifest missing: '+stage.name)
        result.append(_immutable(root,stage,C.read_json(manifest_path)))
    return result


def snapshot(root, stages,native_text=None):
    """Complete actual SHA/size sets and selected glob inventory, including partitions."""
    identities={}; selected={}
    for stage in stages:
        path=A.member(root,stage.manifest); identities[str(path)]=A.pin(path)
        for pointer in ('current.json','previous.json'):
            selected_pointer=root/stage.name/pointer
            if selected_pointer.is_file(): identities[str(selected_pointer.resolve())]=A.pin(selected_pointer)
        manifest=C.read_json(path)
        base=path.parent
        files=recorded_files(manifest,base)
        if not files: raise ValueError('missing full actual inventory: '+stage.name)
        for relative,expected in files.items():
            actual=A.member(base,relative); value=A.pin(actual)
            if value != {k:expected.get(k) for k in ('sha256','bytes')}: raise ValueError('actual stage file changed: '+str(actual))
            identities[str(actual)]=value
        if stage.name=='text_native':
            for relative,expected in {**manifest.get('metadata_files',{}),**manifest.get('units',{})}.items():
                extra=A.member(base,relative)
                if A.pin(extra)!=expected:raise ValueError('native text metadata/unit bytes changed')
                identities[str(extra)]=expected
            plan=C.read_json(base/'source-plan.json')
            if A.pin(base/'source-plan.json')['sha256']!=native_text['expected_plan_sha256']:raise ValueError('native text plan identity differs')
            for relative,expected in plan['files'].items():
                extra=A.member(A.absolute(native_text['source_root']),relative)
                if A.pin(extra)!=expected:raise ValueError('native text actual source bytes changed')
                identities[str(extra)]=expected
            A.check_pins(plan['code']);identities.update(plan['code'])
        selected[stage.name]={view:[str(p.resolve()) for p in paths] for view,paths in output_files(root,stage).items()}
        bound={str(A.member(base,p)) for p in files}
        for paths in selected[stage.name].values():
            if not paths or not set(paths)<=bound: raise ValueError('missing/unbound selected catalog partition: '+stage.name)
    return {'version':VERSION,'identities':identities,'selected':selected,'code':code_pins()}


def verify_notes_keys(root,stages):
    """Retain namespace/version collisions as invalid keys; never deduplicate."""
    for stage in stages:
        if stage.name!='fundamentals_notes':continue
        for out in stage.outputs:
            if out.view!='fundamentals_notes_items':continue
            paths=sorted(root.glob(out.glob))
            if not paths:raise ValueError('missing whole-stage notes item population')
            with C.connect(memory='128MB',threads=1,temp_dir=root/'_tmp'/('tier1-key-audit-'+uuid.uuid4().hex),db_file='keys.duckdb') as con:
                A.guard(con)
                keys=','.join(catalog._q(k) for k in out.keys)
                groups,rows=con.execute('SELECT count(*),coalesce(sum(n),0) FROM (SELECT count(*) n FROM '+A.sql(paths)+' GROUP BY '+keys+' HAVING count(*)>1)').fetchone()
                if groups:raise ValueError(f'invalid notes item key: {groups} collision groups/{rows} rows; namespace/version and ambiguous tax_dtl remain unqualified')


def preflight(root, stages=None, native_text=None,publications=None):
    root=A.absolute(root); stages=resolve(root,declarations() if stages is None else stages,native_text,publications)
    verify_notes_keys(root,stages)
    ordinary=[s for s in stages if s.name!='text_native']
    results=verify.verify(root,ordinary,hash_files=True)
    if any(s.name=='text_native' for s in stages):
        from . import text_features_native as N
        admission=N.read_native_generation(A.absolute(native_text['source_root']),A.member(root,native_text['generation']),
            native_text['expected_manifest_sha256'],native_text['expected_plan_sha256'])
        # Full native replay is authority for this different producer schema.
        # Catalog integrity can pass while its original source gates remain FAIL.
        result=verify.StageResult('text_native',status='OK')
        result.files_checked=len(admission['products'])
        result.binding_basis='complete_native_original_byte_replay'
        results.append(result)
    failures=[r for r in results if r.status in ('FAIL','STALE','PLANNED')]
    if failures: raise ValueError('lake verify incomplete: '+verify.render(failures))
    return snapshot(root,stages,native_text),stages,results


def build_database(root,stages,destination):
    """Reuse native Parquet catalog; add explicit static JSON evidence views."""
    import duckdb
    parquet_stages=[replace(s,outputs=tuple(o for o in s.outputs if o.glob.endswith('.parquet'))) for s in stages]
    receipt=catalog.build(root,parquet_stages,destination,memory='128MB')
    with duckdb.connect(str(destination),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute("SET TimeZone='UTC'")
        for stage in stages:
            for out in stage.outputs:
                if out.glob.endswith('.parquet'):continue
                if not out.glob.endswith(('.json','.jsonl')):raise ValueError('unsupported explicit catalog evidence format')
                source="read_json_auto("+catalog._lit((root/out.glob).as_posix())+", format="+catalog._lit('newline_delimited' if out.glob.endswith('.jsonl') else 'auto')+")"
                view=catalog._q(out.view)
                con.execute('CREATE VIEW '+view+' AS SELECT * FROM '+source)
                types={r[0]:r[1] for r in con.execute('DESCRIBE '+view).fetchall()}
                clock=catalog.clock_expr(out,types)
                if out.clock is not None and clock is None:raise ValueError('JSON catalog native availability clock is not typed')
                con.execute('INSERT INTO lake_outputs VALUES (?,?,?,?,?,?,?,?,?)',[stage.name,out.view,out.glob,out.clock,clock,
                    stage.output_vintage(out),list(out.keys),len(list(root.glob(out.glob))),out.doc])
                if clock:
                    query='SELECT * FROM '+view+' WHERE '+clock+' < CAST(ts AS TIMESTAMP)'
                    if stage.output_vintage(out)=='vintage':
                        if not out.keys:raise ValueError('native vintage keys absent')
                        query+=' QUALIFY row_number() OVER(PARTITION BY '+','.join(catalog._q(k) for k in out.keys)+' ORDER BY '+clock+' DESC'+''.join(', '+catalog._q(k)+' DESC' for k in out.order)+')=1'
                    con.execute('CREATE MACRO '+catalog._q(out.view+'_as_of')+'(ts) AS TABLE '+query)
                    receipt['macros'].append(out.view+'_as_of')
                con.execute('COMMENT ON VIEW '+view+' IS '+catalog._lit('Task17 explicit native evidence; stage '+stage.name+'; source qualification remains separate'))
                receipt['views'].append(out.view)
    receipt['sha256']=A.pin(destination)['sha256']
    return receipt


def build(root, destination, expected_snapshot_sha256, expected_code_pins, admissions=None, stages=None,native_text=None,publications=None):
    """Build off to the side; publish only after two-pass full file verification."""
    import duckdb
    root=A.absolute(root); destination=A.absolute(destination)
    A.check_pins(expected_code_pins,code_pins())
    with C.connect(memory='128MB',threads=1,temp_dir=destination.parent/('admission-'+uuid.uuid4().hex),db_file='admission.duckdb') as con:
        A.guard(con)
        for name,spec in (admissions or {}).items(): T.admit(name,spec,con)
    before,resolved,results=preflight(root,stages,native_text,publications)
    if A.digest(before)!=expected_snapshot_sha256: raise ValueError('independent full catalog snapshot differs')
    if stages is None and set(admissions or {}) != set(T.NAMES):
        raise ValueError('all four complete native reader admissions required for full Task17 catalog')
    # Unique candidate directory avoids replacing a prior catalog on any failure.
    work=destination.parent/('catalog-attempt-'+uuid.uuid4().hex); work.mkdir(parents=True)
    candidate=work/destination.name
    receipt=build_database(root,resolved,candidate)
    after,again,checks=preflight(root,stages,native_text,publications)
    if before!=after or resolved!=again or receipt['skipped']:
        raise ValueError('catalog publication inputs changed or outputs were skipped; attempt retained')
    with C.connect(memory='128MB',threads=1,temp_dir=destination.parent/('admission-'+uuid.uuid4().hex),db_file='admission.duckdb') as con:
        A.guard(con)
        for name,spec in (admissions or {}).items(): T.admit(name,spec,con)
    if snapshot(root,resolved,native_text)!=before: raise ValueError('native admission inputs changed before catalog activation')
    A.check_pins(expected_code_pins,code_pins())
    os.replace(candidate,destination)
    receipt.update(version=VERSION,snapshot_sha256=A.digest(before),verification=verify.render(checks),
        catalog_identity=A.pin(destination),source_quality_certified=False,readiness='NOT_PRODUCTION_READY')
    C.write_json_atomic(destination.with_suffix('.proof.json'),receipt)
    return receipt


def verify_publications(root,spec):
    if set(spec.get('admissions',{}))!=set(T.NAMES):raise ValueError('all four complete native reader admissions required')
    with C.connect(memory='128MB',threads=1,temp_dir=root/'_tmp'/('tier1-admission-'+uuid.uuid4().hex),db_file='admission.duckdb') as con:
        A.guard(con)
        for name,binding in spec['admissions'].items():T.admit(name,binding,con)


def verify_catalog(root,path,spec):
    """Existing lake verify binds catalog bytes and every serving view/macro."""
    import duckdb
    A.check_pins(spec['code_pins'],code_pins())
    verify_publications(root,spec)
    frozen,stages,results=preflight(root,native_text=spec.get('native_text'),publications=spec.get('publications'))
    if A.digest(frozen)!=spec['snapshot_sha256']:raise ValueError('independent full catalog snapshot differs')
    proof=C.read_json(path.with_suffix('.proof.json'))
    if proof['version']!=VERSION or proof['snapshot_sha256']!=spec['snapshot_sha256'] or A.pin(path)!=proof['catalog_identity']:
        raise ValueError('catalog actual bytes/provenance differ')
    with duckdb.connect(str(path),read_only=True,config={'memory_limit':'128MB','threads':'1'}) as con:
        views={r[0] for r in con.execute('SELECT view_name FROM duckdb_views() WHERE NOT internal').fetchall()}
        macros={r[0] for r in con.execute("SELECT function_name FROM duckdb_functions() WHERE function_type='table_macro' AND NOT internal").fetchall()}
        if set(proof['views'])!=views or not set(proof['macros'])<=macros:
            raise ValueError('catalog required serving views/macros differ')
        if {r[0] for r in con.execute('SELECT name FROM lake_stages').fetchall()}!={s.name for s in stages}:
            raise ValueError('catalog required stage registry differs')
        for stage in stages:
            for out in stage.outputs:
                if out.view not in views or (out.clock is not None and out.view+'_as_of' not in macros):
                    raise ValueError('required native stage view/as-of macro missing: '+out.view)
    verify_publications(root,spec)
    if snapshot(root,stages,spec.get('native_text'))!=frozen:raise ValueError('actual serving inputs changed during catalog verification')
    A.check_pins(spec['code_pins'],code_pins())
    return {'status':'PASS_INTEGRITY_ONLY','source_quality_certified':False}


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command',choices=('freeze','verify','build'))
    parser.add_argument('--root',required=True,type=Path)
    parser.add_argument('--contract',type=Path)
    parser.add_argument('--out',type=Path)
    args=parser.parse_args(argv)
    if args.command in ('freeze','verify'):
        spec=C.read_json(args.contract) if args.contract else {}
        frozen,stages,results=preflight(args.root,native_text=spec.get('native_text'),publications=spec.get('publications'))
        if args.command=='freeze': print(__import__('json').dumps(frozen,sort_keys=True,indent=2))
        else: print(verify.render(results))
    else:
        spec=C.read_json(args.contract)
        print(__import__('json').dumps(build(args.root,args.out,spec['snapshot_sha256'],spec['code_pins'],spec['admissions'],native_text=spec.get('native_text'),publications=spec.get('publications')),indent=2))

if __name__=='__main__': main()
