"""Tiny offline admission fixtures; these do not establish source acceptance."""
import copy
import datetime as dt
import json
from pathlib import Path
import types
import zipfile

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import fund_batch_release as R


@pytest.fixture
def con():
    db=duckdb.connect()
    db.execute("SET memory_limit='128MB'")
    db.execute('SET threads=1')
    db.execute("SET TimeZone='UTC'")
    yield db
    db.close()


def fixture_binding(tmp):
    prep=tmp/'prep';extract=tmp/'extract'
    prep.mkdir();extract.mkdir()
    binding={'root':str(tmp),'prep_root':str(prep),'extract_root':str(extract),
        'fsds_root':str(tmp/'fsds'),'archive':{'path':str(tmp/'companyfacts.zip')},
        'products':{'fx':None,'eight_k':None,'calendar':{'files':{str(tmp/'calendar.parquet'): {}}},
                    'links_prepare':{'files':{str(tmp/'links.parquet'): {}}},'prices':{'files':{}},'prices_history':{'files':{}}}}
    native,_=R.native_adapter(binding,tmp/'unused')
    date=dt.date(2020,12,31);acc='0000042-21-000001'
    rows=[{'cik':42,'taxonomy':'us-gaap','concept':concept,'unit':'USD','period_start':start,
        'period_end':date,'filed_date':dt.date(2021,2,20),'fiscal_year':2020,'fiscal_period':'FY',
        'form':'10-K','accession_number':acc,'value':value}
        for concept,start,value in [('Assets',None,1000.),('StockholdersEquity',None,400.),
                                   ('Revenues',dt.date(2020,1,1),800.)]]
    pq.write_table(pa.Table.from_pylist(rows,schema=R.X.SCHEMA),extract/'batch-0000.parquet')
    pq.write_table(pa.Table.from_pylist([{'adsh':acc,'sub_cik':42,'sic':3674,'sub_form':'10-K',
        'sub_fy':2020,'sub_fp':'FY','sub_filed':dt.date(2021,2,20),
        'accepted_utc':dt.datetime(2021,2,20,21,5),'sub_quarter':'2021q1'}]),native.sub_clock_path())
    common_fields={'adsh':pa.array([],pa.string()),'tag':pa.array([],pa.string()),
        'ddate':pa.array([],pa.date32()),'qtrs':pa.array([],pa.int32()),
        'n_members':pa.array([],pa.int64()),'value':pa.array([],pa.float64())}
    pq.write_table(pa.table(common_fields),native.class_shares_path())
    pq.write_table(pa.table({**common_fields,'uom':pa.array([],pa.string())}),native.pos_sums_path())
    pq.write_table(pa.table({'adsh':pa.array([],pa.string()),'concept':pa.array([],pa.string()),
        'ddate':pa.array([],pa.date32()),'qtrs':pa.array([],pa.int32()),'uom':pa.array([],pa.string()),
        'n_lines':pa.array([],pa.int64()),'n_total':pa.array([],pa.int64()),'value':pa.array([],pa.float64())}),native.label_lines_path())
    pq.write_table(pa.Table.from_pylist([{'adsh':acc,**{f:False for f in R.F.fi.PRE_FLAGS},
        'has_is':True,'has_cf':True,'has_bs':True,'is_rev':True}]),native.pre_flags_path())
    return {'binding':binding,'fixture':True}


def test_private_native_v11_and_new_resume(tmp_path,con,monkeypatch):
    snapshot=fixture_binding(tmp_path)
    generation=tmp_path/'candidate';generation.mkdir()
    old_root=R.C.build_root;old_stage=R.X.stage_name;old_dir=R.F.parts_dir
    monkeypatch.setattr(R,'recheck',lambda *a,**k:None)
    row=R.compute_new(snapshot,generation,con,0)
    assert R.C.build_root is old_root and R.X.stage_name is old_stage and R.F.parts_dir is old_dir
    assert R.admit_unit(snapshot,generation,con,0)==row
    native=generation/'units/batch-0000'/row['attempt']/'native'
    event=pq.read_table(native/'events-0000.parquet').to_pylist()[0]
    catalog=pq.read_table(native/'catalog-0000.parquet').to_pylist()[0]
    assert event['clock_utc']==dt.datetime(2021,2,20,21,5)
    assert event['available_at']==event['clock_utc'] and event['at']==1000.
    assert catalog['tstk'] is None  # v11 source-backed branch, never synthetic zero
    with pytest.raises(ValueError,match='new-only'):
        R.compute_new(snapshot,generation,con,0)
    native_receipt=native/'batch-0000.json';original=R.read(native_receipt)
    forged=copy.deepcopy(original);forged['ciks_seen']=999
    R.C.write_json_atomic(native_receipt,forged)
    altered=copy.deepcopy(row);altered['native_receipt']=R.identity(native_receipt)
    R.C.write_json_atomic(generation/'units/batch-0000/admission.json',altered)
    with pytest.raises(ValueError,match='counter population'):R.admit_unit(snapshot,generation,con,0)
    R.C.write_json_atomic(native_receipt,original)
    row['native_receipt']=R.identity(native_receipt)
    R.C.write_json_atomic(generation/'units/batch-0000/admission.json',row)
    # Authentic start alone is insufficient: same-schema financial payload tampering fails replay.
    table=pq.read_table(native/'events-0000.parquet');rows=table.to_pylist();rows[0]['at']=999.
    pq.write_table(pa.Table.from_pylist(rows,schema=table.schema),native/'events-0000.parquet')
    with pytest.raises(ValueError,match='replay mismatch'):
        R.admit_unit(snapshot,generation,con,0)


@pytest.mark.parametrize('damage',['clock','population','duplicate','schema'])
def test_typed_unit_rejects_bad_keys_clocks(tmp_path,con,damage):
    snapshot=fixture_binding(tmp_path);native=tmp_path/'native';native.mkdir()
    R._replay(snapshot,con,0,native)
    path=native/'events-0000.parquet';table=pq.read_table(path);rows=table.to_pylist()
    if damage=='clock':rows[0]['available_at']=dt.datetime(2019,1,1)
    elif damage=='population':rows[0]['cik']=999
    elif damage=='duplicate':rows*=2
    else:table=table.drop(['clock_utc']);rows=table.to_pylist()
    pq.write_table(pa.Table.from_pylist(rows,schema=table.schema),path)
    with pytest.raises(ValueError):R.validate_payloads(con,native,0,Path(snapshot['binding']['extract_root'])/'batch-0000.parquet')


def test_legacy_no_start_is_not_admitted(tmp_path,con):
    snapshot=fixture_binding(tmp_path);generation=tmp_path/'candidate'
    root=generation/'units/batch-0000';root.mkdir(parents=True)
    R.C.write_json_atomic(root/'admission.json',{'version':R.VERSION,'batch':0,
        'snapshot_sha256':R.digest(snapshot),'attempt':'old','start_identity':{},'files':{},'native_receipt':{}})
    with pytest.raises(FileNotFoundError):R.admit_unit(snapshot,generation,con,0)


def test_partial_full_inventory_does_not_publish(tmp_path,con,monkeypatch):
    path=tmp_path/'bindings.json';path.write_text(json.dumps({'root':str(tmp_path)}))
    monkeypatch.setenv('ATX_GUARD_JOB','fixture')
    monkeypatch.setattr(R.C,'connect',lambda **k:con)
    monkeypatch.setattr(R,'freeze',lambda *a,**k:{'fixture':True})
    monkeypatch.setattr(R,'recheck',lambda *a,**k:None)
    monkeypatch.setattr(R,'admit_unit',lambda *a,**k:None)
    calls=[]
    monkeypatch.setattr(R,'compute_new',lambda s,g,c,b:(calls.append(b) or {'batch':b}))
    result=R.build(path,'fresh',max_new_batches=1)
    assert calls==[0] and result['missing_batches']==list(range(1,85))
    assert result['published'] is False
    stage=tmp_path/R.STAGE
    assert not (stage/'current.json').exists() and not (stage/'generations/fresh/manifest.json').exists()
    assert not (stage/'.writer.lock').exists()


def good_evidence():
    return {'coverage':{'observations_by_fyear':{str(y):1 for y in R.V.FYEARS},'items_pass_covered':105},
        'balance':{'issuer_periods':1,'ok_share':1.,'pass_99':True},
        'benchmark':{'pass':True,'cells':10000},
        'cutoff':{'pass':True,'events_sampled':200,'compared_rows':1,'differences':0},
        'exit':{'years':R.CONTRACT['exit_years'],'targets':R.CONTRACT['exit_targets'],
            'by_year':{str(y):{'member_cells':10,**{f:{'member_all_cells':10,'linked_usd_cells':10} for f in R.V.EXIT_FIELDS}} for y in R.CONTRACT['exit_years']},
            'pooled':{f:{'target':t,'met':True} for f,t in R.V.EXIT_FIELDS.items()}}}


@pytest.mark.parametrize('damage',['none','year','zero','sample','benchmark','member','exit'])
def test_original_gates_do_not_shrink_denominators(damage):
    e=good_evidence()
    if damage=='year':e['coverage']['observations_by_fyear'].pop('2025')
    if damage=='zero':e['coverage']['observations_by_fyear']['2025']=0
    if damage=='sample':e['cutoff']['events_sampled']=0
    if damage=='benchmark':e['benchmark']['cells']=1
    if damage=='member':e['exit']['by_year']['2025']['member_cells']=0
    if damage=='exit':e['exit']['by_year'].pop('2025')
    status=R.gate_status(e)
    if damage=='none':assert all(status[k]=='PASS' for k in ('item_depth','balance','FSDS','cutoff','exit'))
    elif damage in ('year','zero'):assert status['item_depth']=='FAIL'
    elif damage=='sample':assert status['cutoff']=='FAIL'
    elif damage=='benchmark':assert status['FSDS']=='FAIL'
    else:assert status['exit']=='FAIL'
    assert status['whole_dataset_acceptance']=='FAIL_PENDING' and status['economic_source_quality']=='FAIL_UNQUALIFIED'


def test_complete_snapshot_contract_is_mandatory(tmp_path):
    snapshot={'version':R.VERSION,'contract':R.CONTRACT,'binding':{'root':str(tmp_path)},
        'code':{str(p.resolve()):{} for p in R.code_paths()},
        'products':{k:{} for k in R.reader_contract(tmp_path)},'preparation':{'units':{str(i):{} for i in range(144)}}}
    R.require_snapshot(snapshot)
    for key in ('code','products'):
        damaged=copy.deepcopy(snapshot);damaged[key].pop(next(iter(damaged[key])))
        with pytest.raises(ValueError,match='inventory'):R.require_snapshot(damaged)
    damaged=copy.deepcopy(snapshot);damaged['preparation']['units'].pop('0')
    with pytest.raises(ValueError,match='inventory'):R.require_snapshot(damaged)


def test_pointer_pin_lock_transaction_and_rollback(tmp_path,con,monkeypatch):
    stage=tmp_path/R.STAGE
    for name in ('a','b'):
        gen=R._generation(stage,name);gen.mkdir(parents=True)
        R.C.write_json_atomic(gen/'manifest.json',{'generation':name})
    admitted=[]
    monkeypatch.setattr(R,'validate_generation',lambda s,g,c:admitted.append(g.name))
    R._publish_pointer(stage,R._generation(stage,'a'),R.identity(R._generation(stage,'a')/'manifest.json'))
    R._publish_pointer(stage,R._generation(stage,'b'),R.identity(R._generation(stage,'b')/'manifest.json'))
    assert R.resolve_current(stage).name=='b'
    R.rollback(stage,con)
    assert admitted==['a'] and R.resolve_current(stage).name=='a'
    with R.writer_lock(stage):
        with pytest.raises(RuntimeError):R.rollback(stage,con)
    tx={'version':R.VERSION,'current':R.read(stage/'previous.json'),'previous':R.read(stage/'current.json')}
    R.C.write_json_atomic(stage/'pointer-transaction.json',tx)
    with pytest.raises(ValueError,match='transaction'):R.resolve_current(stage)
    R.recover(stage,con)
    assert R.resolve_current(stage).name=='b' and admitted==['a','b']
    R.C.write_json_atomic(R.resolve_current(stage)/'manifest.json',{'generation':'tampered'})
    with pytest.raises(ValueError,match='identity'):R.resolve_current(stage)


@pytest.mark.parametrize('damage',['none','old_manifest','new_manifest','old_path','new_admission','old_during_admission'])
def test_recovery_source_refresh_preserves_stale_previous_identity(tmp_path,con,monkeypatch,damage):
    stage=tmp_path/R.STAGE
    pointers={}
    for name in ('old','new'):
        generation=R._generation(stage,name);generation.mkdir(parents=True)
        R.C.write_json_atomic(generation/'manifest.json',{'generation':name})
        pointers[name]={'generation':name,'manifest':R.identity(generation/'manifest.json')}
    R.C.write_json_atomic(stage/'current.json',pointers['old'])
    tx={'version':R.VERSION,'current':pointers['new'],'previous':pointers['old']}
    if damage=='old_path':tx['previous']={**pointers['old'],'generation':'../old'}
    R.C.write_json_atomic(stage/'pointer-transaction.json',tx)
    if damage in ('old_manifest','new_manifest'):
        name=damage.split('_')[0]
        R.C.write_json_atomic(R._generation(stage,name)/'manifest.json',{'tampered':True})
    admitted=[]
    def validate(s,g,c):
        admitted.append(g.name)
        if g.name=='old' or damage=='new_admission':raise ValueError('stale source/code snapshot')
        if damage=='old_during_admission':
            R.C.write_json_atomic(R._generation(stage,'old')/'manifest.json',{'tampered':True})
    monkeypatch.setattr(R,'validate_generation',validate)
    if damage=='none':
        R.recover(stage,con)
        assert admitted==['new'] and R.resolve_current(stage).name=='new'
        assert R.read(stage/'previous.json')==pointers['old']
        assert not (stage/'pointer-transaction.json').exists()
        # Identity-preserved stale previous still cannot become current.
        with pytest.raises(ValueError,match='stale source/code'):R.rollback(stage,con)
        assert admitted==['new','old'] and R.resolve_current(stage).name=='new'
    else:
        with pytest.raises(ValueError):R.recover(stage,con)
        assert (stage/'pointer-transaction.json').exists()
        assert R.read(stage/'current.json')==pointers['old']
    assert not (stage/'.writer.lock').exists()


def test_private_native_validator_root_and_std_manifest(tmp_path):
    binding=fixture_binding(tmp_path)['binding'];leaf=tmp_path/R.STAGE/'generations/g'/R.SEMANTIC_STAGE
    native,validator=R.native_adapter(binding,leaf)
    assert native.fund_dir()==leaf.resolve() and native.fx.stage_name()=='fundamentals_v11'
    assert validator.common.build_root()==tmp_path.resolve() and validator._stage()==leaf.resolve()
    from atx_db.alpha_panel import compustat_std as S
    inp=S.Inputs(leaf,tmp_path/'earn',None,None,dt.date(2025,12,31))
    assert inp.fund.name=='fundamentals_v11'
    leaf.mkdir(parents=True);R.C.write_json_atomic(leaf/'manifest.json',{'stage':R.SEMANTIC_STAGE})
    assert S._manifest_sha(leaf/'manifest.json')==R.identity(leaf/'manifest.json')['sha256']


def test_reader_selection_bytes_and_optional_absence(tmp_path,monkeypatch):
    selected=tmp_path/'x.parquet';pq.write_table(pa.table({'x':[1]}),selected)
    monkeypatch.setattr(R,'reader_contract',lambda r:{'one':{'paths':[selected]},'eight_k':{'paths':[tmp_path/'missing'],'optional':True}})
    product={'source':'fixture producer','clock':'fixture available_at','admission':'fixture independent check','files':{str(selected):R.identity(selected)}}
    R.check_products(tmp_path,{'one':product,'eight_k':None})
    with pytest.raises(ValueError,match='inventory'):R.check_products(tmp_path,{'one':product})
    pq.write_table(pa.table({'x':[2]}),selected)
    with pytest.raises(ValueError,match='bytes'):R.check_products(tmp_path,{'one':product,'eight_k':None})
    (tmp_path/'missing').write_text('present')
    with pytest.raises(ValueError,match='present optional'):R.check_products(tmp_path,{'one':{**product,'files':{str(selected):R.identity(selected)}},'eight_k':None})


def test_full_extract_archive_replay_not_receipt_blessing(tmp_path,con,monkeypatch):
    archive=tmp_path/'companyfacts.zip';extract=tmp_path/'extract';extract.mkdir()
    with zipfile.ZipFile(archive,'w') as z:
        for cik in range(1,86):
            payload={'facts':{'us-gaap':{'Assets':{'units':{'USD':[{'end':'2020-12-31',
                'filed':'2021-02-20','val':100.,'form':'10-K','fy':2020,'fp':'FY','accn':f'{cik}-21-001'}]}}}}}
            z.writestr(f'CIK{cik:010d}.json',json.dumps(payload))
    monkeypatch.setattr(R.X,'plan',lambda z:[[i] for i in sorted(z.infolist(),key=lambda x:x.filename)])
    extractor=R._clone(R.X,{'out_dir':lambda:extract,'archive_identity':lambda:{'bytes':archive.stat().st_size,'mtime_ns':archive.stat().st_mtime_ns}})
    with zipfile.ZipFile(archive) as z:
        for bid,members in enumerate(R.X.plan(z)):extractor.extract_batch(z,bid,members)
    R.C.write_json_atomic(extract/'plan.json',{'batches':85,'code_version':R.X.CODE_VERSION})
    binding={'root':str(tmp_path),'extract_root':str(extract),'archive':{'path':str(archive),**R.identity(archive)}}
    assert len(R.check_extract(binding,con)['files'])==256
    # A forged fresh receipt and correct typed CIK count cannot authenticate false financial values.
    path=extract/'batch-0000.parquet';table=pq.read_table(path);rows=table.to_pylist();rows[0]['value']=999.
    pq.write_table(pa.Table.from_pylist(rows,schema=table.schema),path)
    receipt=R.read(path.with_suffix('.json'));receipt.update(parquet_sha256=R.identity(path)['sha256'],parquet_bytes=path.stat().st_size)
    R.C.write_json_atomic(path.with_suffix('.json'),receipt)
    with pytest.raises(ValueError,match='archive replay mismatch'):R.check_extract(binding,con)
    pq.write_table(table,path)
    receipt.update(parquet_sha256=R.identity(path)['sha256'],parquet_bytes=path.stat().st_size)
    receipt['counts']['members_error']=999
    R.C.write_json_atomic(path.with_suffix('.json'),receipt)
    with pytest.raises(ValueError,match='error population'):R.check_extract(binding,con)
    (extract/'batch-0084.stats.parquet').unlink()
    with pytest.raises(ValueError,match='inventory'):R.check_extract(binding,con)


def test_cutoff_timing_is_excluded_from_deterministic_gate_evidence(tmp_path,con,monkeypatch):
    native=types.SimpleNamespace(_stats=lambda *a:{'typed_counter':(1,2)},coverage_by_year=lambda *a:{})
    validator=types.SimpleNamespace(**{name:(lambda *a: {}) for name in ('coverage','benchmark','balance','fx_check','ttm_gaps','exit_measure')},
        cutoff=lambda *a:{'seconds':123.4,'events_sampled':200,'pass':True})
    monkeypatch.setattr(R,'native_adapter',lambda *a:(native,validator))
    evidence=R.native_evidence({'binding':{}},con,tmp_path/'fundamentals_v11')
    assert 'seconds' not in evidence['cutoff'] and evidence['cutoff']['events_sampled']==200
    assert evidence['native_stats']['typed_counter']==[1,2]


def test_frozen_config_and_generation_location(tmp_path,con):
    assert R.read(R.contract_path())==R.CONTRACT
    stage=tmp_path/R.STAGE
    with pytest.raises(ValueError,match='location'):
        R.validate_generation(stage,tmp_path/'legacy/fundamentals_v11',con)


def test_contract_cli_is_offline(capsys):
    R.main(['contract'])
    assert json.loads(capsys.readouterr().out)==R.CONTRACT


@pytest.mark.parametrize('damage',['none','version','empty','extra','path','hash','rows','sources','source_hash','link_table_missing',
    'output_relative','output_nonstring','security_company_links_relative','security_permanent_ids_relative','r4_manifest_relative','source_nonstring'])
def test_exact_identity_outputs_manifest_adapter(tmp_path,monkeypatch,damage):
    from atx_db.alpha_panel import identity_links as I
    directory=tmp_path/'identity';directory.mkdir()
    links=directory/'links.parquet';link_table=directory/'link_table.parquet'
    pq.write_table(pa.table({'cik':[42]}),links);pq.write_table(pa.table({'cik':[43]}),link_table)
    sources={}
    for name,attribute in [('security_company_links','LINKS_SRC'),('security_permanent_ids','PERM_IDS_SRC'),('r4_manifest','R4_MANIFEST')]:
        source=tmp_path/(name+'.fixture');source.write_text(name)
        monkeypatch.setattr(I,attribute,source)
        sources[name]={**R.identity(source),'path':str(source),'mtime_ns':source.stat().st_mtime_ns}
    manifest=directory/'manifest.json'
    output={**R.identity(links),'path':str(links),'mtime_ns':links.stat().st_mtime_ns,'rows':1}
    doc={'stage':'I','code_version':I.CODE_VERSION,'rule':I.RULE,'outputs':{'links.parquet':output},'sources':sources}
    if damage=='version':doc['code_version']='unknown'
    if damage=='empty':doc['outputs']={}
    if damage=='extra':doc['outputs']['extra.parquet']=output
    if damage=='path':output['path']=str(link_table)
    if damage=='hash':output['sha256']='0'*64
    if damage=='rows':output['rows']=2
    if damage=='sources':doc['sources'].pop('r4_manifest')
    if damage=='source_hash':doc['sources']['r4_manifest']['sha256']='0'*64
    if damage=='output_relative':
        monkeypatch.chdir(tmp_path)
        output['path']='identity/links.parquet'
    if damage=='output_nonstring':output['path']=None
    if damage.endswith('_relative') and damage!='output_relative':
        monkeypatch.chdir(tmp_path)
        name=damage.removesuffix('_relative')
        sources[name]['path']=name+'.fixture'
    if damage=='source_nonstring':sources['r4_manifest']['path']=None
    R.C.write_json_atomic(manifest,doc)
    monkeypatch.setattr(R,'reader_contract',lambda root:{'identity_manifest':{'paths':[manifest]},'links_prepare':{'paths':[links]},'links_validation':{'paths':[link_table]}})
    def product(path):return {'source':'fixture','clock':'unqualified source clock','admission':'byte fixture only','files':{str(path):R.identity(path)}}
    products={'identity_manifest':product(manifest),'links_prepare':product(links),'links_validation':product(link_table)}
    if damage=='link_table_missing':products.pop('links_validation')
    if damage=='none':assert R.check_products(tmp_path,products)==products
    else:
        with pytest.raises(ValueError):R.check_products(tmp_path,products)


@pytest.mark.parametrize('damage',['status','quality','source_backed','code','units'])
def test_public_validation_rejects_abbreviated_qualification_before_sources(tmp_path,con,damage):
    stage=tmp_path/R.STAGE;generation=R._generation(stage,'g')
    (generation/R.SEMANTIC_STAGE).mkdir(parents=True)
    for i in R.REQUIRED:(generation/'units'/f'batch-{i:04d}').mkdir(parents=True)
    snapshot={'version':R.VERSION,'contract':R.CONTRACT,'binding':{'root':str(tmp_path)},
        'code':{str(p.resolve()):{} for p in R.code_paths()},
        'products':{k:{} for k in R.reader_contract(tmp_path)},'preparation':{'units':{str(i):{} for i in range(144)}}}
    manifest={'stage':R.STAGE,'schema':'atx.fundamentals-release/v1','status':'complete','contract':R.CONTRACT,
        'semantic_producer':R.SEMANTIC_STAGE,'source_backed_only':True,'readiness':'NOT_PRODUCTION_READY','source_quality_certified':False}
    if damage=='status':manifest['status']='partial'
    if damage=='quality':manifest['source_quality_certified']=0
    if damage=='source_backed':manifest['source_backed_only']=1
    if damage=='code':snapshot['code']={}
    if damage=='units':(generation/'units/batch-0084').rmdir()
    R.C.write_json_atomic(generation/'snapshot.json',snapshot)
    R.C.write_json_atomic(generation/'manifest.json',manifest)
    R.C.write_json_atomic(generation/'validation.json',{})
    with pytest.raises(ValueError):R.validate_generation(stage,generation,con)
