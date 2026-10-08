"""Offline synthetic R19 boundaries; no production source or economics proof."""
import copy
import json
import shutil
import zipfile
from pathlib import Path

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


@pytest.fixture(scope='module')
def base(tmp_path_factory):
    root=tmp_path_factory.mktemp('extract-resume-base');extract=root/'extract';extract.mkdir()
    archive=root/'companyfacts.zip'
    with zipfile.ZipFile(archive,'w') as z:
        for cik in range(1,86):
            payload={'facts':{'us-gaap':{'Assets':{'units':{'USD':[{'end':'2020-12-31',
                'filed':'2021-02-20','val':100.,'form':'10-K','fy':2020,'fp':'FY','accn':f'{cik}-21-001'}]}}}}}
            z.writestr(f'CIK{cik:010d}.json',json.dumps(payload))
    extractor=R._clone(R.X,{'out_dir':lambda:extract,'archive_identity':lambda:{'bytes':archive.stat().st_size,'mtime_ns':archive.stat().st_mtime_ns}})
    with zipfile.ZipFile(archive) as z:
        for bid,member in enumerate(sorted(z.infolist(),key=lambda m:m.filename)):
            extractor.extract_batch(z,bid,[member])
    R.C.write_json_atomic(extract/'plan.json',{'batches':85,'code_version':R.X.CODE_VERSION})
    yield root
    shutil.rmtree(root)


@pytest.fixture
def fixture(tmp_path,base,monkeypatch):
    root=tmp_path/'root';shutil.copytree(base,root)
    archive=root/'companyfacts.zip';extract=root/'extract'
    # copytree preserves timestamps; native ZIP planning still uses its own algorithm.
    monkeypatch.setattr(R.X,'TARGET_UNCOMPRESSED',1)
    monkeypatch.setenv('ATX_GUARD_JOB','offline-fixture')
    code=root/'fixture-code.py';code.write_text('frozen fixture code')
    monkeypatch.setattr(R,'code_paths',lambda:[code,R.contract_path()])
    binding={'root':str(root),'extract_root':str(extract),'archive':{'path':str(archive),**R.identity(archive)}}
    path=root/'binding.json';R.C.write_json_atomic(path,binding)
    yield binding,path,tmp_path/'receipts'
    shutil.rmtree(tmp_path)


def inventory(fixture,con):
    """Synthetic controller inventory; no production controller admission is inferred."""
    binding,path,directory=fixture
    checked=R.check_extract(binding,con,verify_keys=False)
    with zipfile.ZipFile(binding['archive']['path']) as z:planned=R.X.plan(z)
    pins={}
    for bid in R.REQUIRED:
        receipt=directory/f'{bid:04d}.json'
        R._write_immutable(receipt,{'version':R.EXTRACT_VALIDATION,'batch':bid,
            'source':R.extract_source(binding,bid,planned,checked['files']),
            'validation':R.extraction_result(binding,bid)})
        pins[str(bid)]={'path':str(receipt),'identity':R.identity(receipt)}
    admission=directory.parent/'controller-admission.json'
    R._write_immutable(admission,{'version':R.EXTRACT_VALIDATION,'receipts':pins})
    return admission,R.identity(admission)['sha256']


def test_bounded_receipt_exact_and_immutable(fixture,con,monkeypatch):
    binding,path,directory=fixture;target=directory/'one.json';calls=[]
    replay=R.replay_extract
    monkeypatch.setattr(R,'replay_extract',lambda b,c,i,m:(calls.append(i),replay(b,c,i,m))[1])
    result=R.validate_extract_batch(path,0,target,con)
    assert calls==[0] and result['whole_source_admitted'] is False
    row=R.read(target)
    assert row['validation']['checks']==R.EXTRACT_CHECKS
    assert row['validation']['member_rows']==[{'cik':1,'rows':1}]
    assert len(row['source']['members'])==85
    pin=R.identity(target)
    with pytest.raises(ValueError,match='immutable'):R.validate_extract_batch(path,0,target,con)
    assert R.identity(target)==pin
    assert len(list((directory/'attempts').glob('*/start.json')))==1
    with pytest.raises(FileExistsError):R._write_immutable(target,{'tampered':True})
    assert R.identity(target)==pin


@pytest.mark.parametrize('bid',[-1,85,True,None])
def test_bounds_before_source_work(fixture,con,monkeypatch,bid):
    binding,path,directory=fixture
    monkeypatch.setattr(R,'check_extract',lambda *a,**k:pytest.fail('expensive source work'))
    with pytest.raises(ValueError):R.validate_extract_batch(path,bid,directory/'one.json',con)


def test_interrupted_attempt_is_unsealed_and_not_reused(fixture,con,monkeypatch):
    binding,path,directory=fixture;target=directory/'one.json';replay=R.replay_extract
    def interrupted(*a):raise RuntimeError('interrupted')
    monkeypatch.setattr(R,'replay_extract',interrupted)
    with pytest.raises(RuntimeError):R.validate_extract_batch(path,0,target,con)
    assert not target.exists() and len(list((directory/'attempts').glob('*/start.json')))==1
    monkeypatch.setattr(R,'replay_extract',replay)
    R.validate_extract_batch(path,0,target,con)
    assert len(list((directory/'attempts').glob('*/start.json')))==2


@pytest.mark.parametrize('damage',['payload','stats','counter','population','duplicate','plan','code_during','archive_during'])
def test_exact_replay_and_before_after_fail_closed(fixture,con,monkeypatch,damage):
    binding,path,directory=fixture;root=Path(binding['extract_root']);target=directory/'one.json'
    if damage in ('payload','population','duplicate'):
        payload=root/'batch-0000.parquet';table=pq.read_table(payload);rows=table.to_pylist()
        if damage=='payload':rows[0]['value']=999.
        elif damage=='population':rows[0]['cik']=2
        else:rows*=2
        pq.write_table(pa.Table.from_pylist(rows,schema=table.schema),payload)
        receipt=R.read(root/'batch-0000.json');pin=R.identity(payload)
        receipt.update(parquet_sha256=pin['sha256'],parquet_bytes=pin['bytes'],rows=len(rows))
        if damage=='duplicate':receipt['member_rows'][0]['rows']=2
        R.C.write_json_atomic(root/'batch-0000.json',receipt)
    elif damage=='stats':
        payload=root/'batch-0000.stats.parquet';table=pq.read_table(payload);rows=table.to_pylist();rows[0]['n_facts']=99
        pq.write_table(pa.Table.from_pylist(rows,schema=table.schema),payload)
    elif damage=='counter':
        receipt=R.read(root/'batch-0000.json');receipt['counts']['members_error']=99
        R.C.write_json_atomic(root/'batch-0000.json',receipt)
    elif damage=='plan':R.C.write_json_atomic(root/'plan.json',{'batches':84,'code_version':R.X.CODE_VERSION})
    else:
        replay=R.replay_extract
        def drift(*a):
            replay(*a)
            changed=R.code_paths()[0] if damage=='code_during' else Path(binding['archive']['path'])
            with changed.open('ab') as stream:stream.write(b'drift')
        monkeypatch.setattr(R,'replay_extract',drift)
    with pytest.raises(ValueError):R.validate_extract_batch(path,0,target,con)
    assert not target.exists()


def test_pinned_resume_no_reconstruction_force_path_and_no_self_admission(fixture,con,monkeypatch):
    binding,path,directory=fixture;admission,sha=inventory(fixture,con)
    admitted=R.load_extract_admission(binding,admission,sha)
    with pytest.raises(ValueError,match='caller expected'):
        R.check_extract(binding,con,admission=admitted)
    replay=R.replay_extract;calls=[]
    def counted(*a):calls.append(a[2]);return replay(*a)
    monkeypatch.setattr(R,'replay_extract',counted)
    R.check_extract(binding,con,admission=admitted,expected_admission_sha256=sha)
    assert calls==[]
    R.check_extract(binding,con,admission=admitted,force_full_replay=True,expected_admission_sha256=sha)
    assert calls==list(range(85))
    calls.clear();R.check_extract(binding,con)
    assert calls==list(range(85))  # existence, candidate claims and unpinned files grant nothing


@pytest.mark.parametrize('damage',['missing','duplicate','abbreviated','tampered','stale_code','stale_plan','stale_archive','stale_payload','stale_stats','stale_receipt','relative','self_hash','internal','unsealed','receipt_abbreviated'])
def test_external_inventory_boundary(fixture,con,damage):
    binding,path,directory=fixture;admission,sha=inventory(fixture,con);doc=R.read(admission)
    if damage=='missing':doc['receipts'].pop('84')
    elif damage=='duplicate':doc['receipts']['84']=doc['receipts']['0']
    elif damage=='abbreviated':doc['receipts']['0']['identity'].pop('bytes')
    elif damage=='relative':doc['receipts']['0']['path']='receipts/0000.json'
    elif damage=='self_hash':doc['sha256']=sha
    elif damage=='tampered':Path(doc['receipts']['0']['path']).write_text('{}')
    elif damage in ('unsealed','receipt_abbreviated'):
        receipt=Path(doc['receipts']['0']['path']);row=R.read(receipt)
        if damage=='unsealed':row={'version':R.EXTRACT_VALIDATION,'batch':0,'source':row['source']}
        else:row['validation']['checks']=[]
        R.C.write_json_atomic(receipt,row);doc['receipts']['0']['identity']=R.identity(receipt)
    elif damage=='internal':
        admission=Path(binding['extract_root'])/'admission.json';R.C.write_json_atomic(admission,doc)
    elif damage.startswith('stale_'):
        suffix=damage[6:]
        if suffix=='code':R.code_paths()[0].write_text('changed')
        elif suffix=='archive':
            archive=Path(binding['archive']['path'])
            with archive.open('ab') as stream:stream.write(b'changed')
            binding['archive'].update(R.identity(archive))
        elif suffix=='plan':
            plan=Path(binding['extract_root'])/'plan.json';value=R.read(plan);value['new']='changed';R.C.write_json_atomic(plan,value)
        else:
            file=Path(binding['extract_root'])/('batch-0000'+{'payload':'.parquet','stats':'.stats.parquet','receipt':'.json'}[suffix])
            if suffix=='receipt':value=R.read(file);value['seconds']=999;R.C.write_json_atomic(file,value)
            else:
                table=pq.read_table(file);rows=table.to_pylist();rows[0]['value' if suffix=='payload' else 'n_facts']=999
                pq.write_table(pa.Table.from_pylist(rows,schema=table.schema),file)
                if suffix=='payload':
                    receipt=file.with_suffix('.json');value=R.read(receipt);pin=R.identity(file)
                    value.update(parquet_sha256=pin['sha256'],parquet_bytes=pin['bytes']);R.C.write_json_atomic(receipt,value)
    if damage in ('missing','duplicate','abbreviated','relative','self_hash','unsealed','receipt_abbreviated'):
        R.C.write_json_atomic(admission,doc);sha=R.identity(admission)['sha256']
    if damage=='internal':sha=R.identity(admission)['sha256']
    with pytest.raises(ValueError):
        selected=R.load_extract_admission(binding,admission,sha)
        R.check_extract(binding,con,admission=selected,expected_admission_sha256=sha)


def test_caller_pin_required_and_admission_rechecked(fixture,con,monkeypatch):
    binding,path,directory=fixture;admission,sha=inventory(fixture,con)
    for expected in (None,'0'*64):
        with pytest.raises(ValueError):R.load_extract_admission(binding,admission,expected)
    with pytest.raises(ValueError):R.load_extract_admission(binding,'controller-admission.json',sha)
    with pytest.raises(ValueError):R._pairs([('0',1),('0',2)])
    selected=R.load_extract_admission(binding,admission,sha)
    original=R.admit_extract_receipt
    def drift(*args):
        original(*args)
        if args[1]==84:admission.write_text('{}')
    monkeypatch.setattr(R,'admit_extract_receipt',drift)
    with pytest.raises(ValueError,match='admission changed'):R.check_extract(binding,con,admission=selected,expected_admission_sha256=sha)


def test_frozen_admission_cannot_be_downgraded_or_authorize_its_own_reuse(fixture,con,monkeypatch):
    import types
    binding,path,directory=fixture
    binding.update(version=R.VERSION,semantic_producer=R.SEMANTIC_STAGE,products={},prep_root=str(Path(binding['root'])/'prep'))
    Path(binding['prep_root']).mkdir();R.C.write_json_atomic(Path(binding['prep_root'])/'prepare.json',{})
    R.C.write_json_atomic(path,binding)
    admission,sha=inventory(fixture,con)
    # Isolate reader/preparation orchestration, whose unchanged production checks stay mandatory.
    monkeypatch.setattr(R,'check_products',lambda *a:{})
    monkeypatch.setattr(R,'reader_contract',lambda *a:{})
    monkeypatch.setattr(R,'native_adapter',lambda *a:(types.SimpleNamespace(_verify_preparation=lambda:{'units':{str(i):{} for i in range(144)}}),None))
    snapshot=R.freeze(path,con,extract_admission=admission,expected_admission_sha256=sha)
    assert snapshot['extract_admission']['identity']['sha256']==sha
    assert snapshot['binding_identity']==R.identity(path)
    assert 'extract_admission' not in snapshot['binding']
    calls=[];replay=R.replay_extract
    def counted(*a):calls.append(a[2]);return replay(*a)
    monkeypatch.setattr(R,'replay_extract',counted)
    R.recheck(snapshot,con,full=True,extract_admission=admission,expected_admission_sha256=sha,force_full_replay=False)
    assert calls==[]
    R.recheck(snapshot,con,full=True)
    assert calls==list(range(85))
    damaged=copy.deepcopy(snapshot);damaged['extract_admission']['document']['receipts'].pop('84')
    with pytest.raises(ValueError):R.recheck(damaged,con)
    alternate=admission.parent/'alternate.json';R.C.write_json_atomic(alternate,R.read(admission))
    with pytest.raises(ValueError,match='differs from frozen'):
        R.recheck(snapshot,con,full=True,extract_admission=alternate,expected_admission_sha256=R.identity(alternate)['sha256'],force_full_replay=False)


def test_all85_mandatory_even_for_one_bounded_batch(fixture,con):
    binding,path,directory=fixture
    (Path(binding['extract_root'])/'batch-0084.stats.parquet').unlink()
    with pytest.raises(ValueError,match='inventory'):R.validate_extract_batch(path,0,directory/'one.json',con)
    assert not (directory/'one.json').exists()


def test_duplicate_native_zip_population_rejected(fixture,con,monkeypatch):
    binding,path,directory=fixture;plan=R.X.plan
    def duplicate(z):
        batches=plan(z);batches[-1]=batches[0];return batches
    monkeypatch.setattr(R.X,'plan',duplicate)
    with pytest.raises(ValueError,match='duplicate archive member'):
        R.validate_extract_batch(path,0,directory/'one.json',con)


@pytest.mark.parametrize('field',['batch_id','rows','members','counts','member_rows'])
def test_boolean_diagnostics_are_not_integer_proof(fixture,con,field):
    binding,path,directory=fixture;native=Path(binding['extract_root'])/'batch-0000.json';row=R.read(native)
    if field=='counts':row['counts']['rows']=True
    elif field=='member_rows':row['member_rows'][0]['cik']=True
    else:row[field]=False if field=='batch_id' else True
    R.C.write_json_atomic(native,row)
    with pytest.raises(ValueError):R.validate_extract_batch(path,0,directory/'one.json',con)


def test_cli_bounded_before_connection_and_complete_receipt(fixture,con,monkeypatch,capsys):
    import ast
    binding,path,directory=fixture;connections=[]
    def connect(**kwargs):connections.append(kwargs);return con
    monkeypatch.setattr(R.C,'connect',connect)
    with pytest.raises(SystemExit):
        R.main(['validate-extract','--bindings',str(path),'--batch','85','--receipt',str(directory/'cli.json')])
    assert connections==[]
    R.main(['validate-extract','--bindings',str(path),'--batch','0','--receipt',str(directory/'cli.json')])
    result=ast.literal_eval(capsys.readouterr().out)
    assert result['batch']==0 and result['whole_source_admitted'] is False
    assert connections[0]['memory']=='128MB' and connections[0]['threads']==1
    assert result['identity']==R.identity(directory/'cli.json')
