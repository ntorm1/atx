"""Actual-byte, partition, immutable-phase and catalog macro fixtures."""
import datetime as dt
from pathlib import Path
import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from atx_db.alpha_panel import common as C, tier1_catalog as T, private_tier1_admission as A
from atx_db.stagelake.contract import Stage,Output


def world(root):
    path=root/'fixture/year=2021/data.parquet';path.parent.mkdir(parents=True,exist_ok=True)
    pq.write_table(pa.table({'security_id':[1,1],'value':[10.,20.],
        'available_at':pa.array([dt.datetime(2021,1,1),dt.datetime(2021,2,1)],type=pa.timestamp('us'))}),path)
    C.write_json_atomic(root/'fixture/manifest.json',{'schema':'fixture/v1','status':'complete',
        'code':{'fixture':'test'},'files':{'year=2021/data.parquet':A.pin(path)}})
    stage=Stage('fixture','OPS','fixture/v1',None,doc='Built by offline fixture',
        outputs=(Output('fixture/year=*/data.parquet','fixture',vintage='vintage',keys=('security_id',)),))
    return path,stage


def test_all_plan_family_stubs_present():
    stages=T.declarations();by_name={s.name:s for s in stages}
    assert set(T.STUBS)<=set(by_name)
    assert {'compustat_std_v11','ratios_v11','fundamentals_v11'}<=set(by_name)
    assert len(by_name['fundamentals_notes'].outputs)==7


def test_changed_partition_and_new_unbound_partition_refuse_verify(tmp_path):
    path,stage=world(tmp_path);before,_,_=T.preflight(tmp_path,[stage])
    pq.write_table(pa.table({'changed':[1]}),path)
    with pytest.raises(ValueError,match='lake verify incomplete'):T.preflight(tmp_path,[stage])
    path,stage=world(tmp_path)
    extra=tmp_path/'fixture/year=2022/data.parquet';extra.parent.mkdir();extra.write_bytes(path.read_bytes())
    with pytest.raises(ValueError,match='unbound_file'): T.preflight(tmp_path,[stage])


def test_real_catalog_preserves_strict_cutoff_latest_vintage(tmp_path,monkeypatch):
    path,stage=world(tmp_path);before,_,_=T.preflight(tmp_path,[stage]);out=tmp_path/'catalog.duckdb'
    monkeypatch.setattr(A,'guard',lambda con:None)
    receipt=T.build(tmp_path,out,A.digest(before),T.code_pins(),stages=[stage])
    assert receipt['source_quality_certified'] is False and not receipt['skipped']
    with duckdb.connect(str(out),read_only=True,config={'memory_limit':'128MB','threads':'1'}) as con:
        assert con.execute("SELECT value FROM fixture_as_of(TIMESTAMP '2021-02-01')").fetchall()==[(10.,)]
        assert con.execute("SELECT value FROM fixture_as_of(TIMESTAMP '2021-02-02')").fetchall()==[(20.,)]


def test_catalog_retains_prior_database_when_partition_changes_during_build(tmp_path,monkeypatch):
    path,stage=world(tmp_path);before,_,_=T.preflight(tmp_path,[stage]);out=tmp_path/'catalog.duckdb';out.write_bytes(b'prior')
    monkeypatch.setattr(A,'guard',lambda con:None)
    original=T.catalog.build
    def changed(*args,**kwargs):
        result=original(*args,**kwargs);path.write_bytes(b'replaced partition');return result
    monkeypatch.setattr(T.catalog,'build',changed)
    with pytest.raises(ValueError,match='lake verify incomplete'):
        T.build(tmp_path,out,A.digest(before),T.code_pins(),stages=[stage])
    assert out.read_bytes()==b'prior' and list(tmp_path.glob('catalog-attempt-*'))


def test_parsed_earnings_cannot_be_catalog_features(tmp_path,monkeypatch):
    monkeypatch.setenv('ATX_ALPHA_PANEL_ROOT',str(tmp_path))
    stage=Stage('ea_actuals','EVT','native/v1',None,doc='Built by frozen native Task9',
        outputs=(Output('ea_actuals/ann_features.parquet','ea_actuals'),))
    path=tmp_path/'ea_actuals';path.mkdir()
    C.write_json_atomic(path/'manifest.json',{'schema':'native/v1','status':'complete','phase':'parsed','code':{'fixture':'test'},'files':{'unused':{'sha256':'0'*64,'bytes':0}}})
    with pytest.raises(RuntimeError,match='phase'):T.resolve(tmp_path,[stage])


def test_existing_lake_catalog_and_verify_entrypoints_full_proof(tmp_path,monkeypatch):
    from atx_db.stagelake import catalog,verify,registry
    path,stage=world(tmp_path);monkeypatch.setattr(T,'declarations',lambda:[stage])
    monkeypatch.setattr(A,'guard',lambda con:None)
    calls=[];monkeypatch.setattr(T.T,'admit',lambda name,spec,con:calls.append(name))
    before,_,_=T.preflight(tmp_path);out=tmp_path/'catalog.duckdb'
    spec={'snapshot_sha256':A.digest(before),'code_pins':T.code_pins(),'admissions':{name:{} for name in T.T.NAMES}}
    contract=tmp_path/'independent-contract.json';C.write_json_atomic(contract,spec)
    assert catalog.main(['--root',str(tmp_path),'--out',str(out),'--tier1-contract',str(contract)])==0
    spec.update(catalog_pin=A.pin(out),proof_pin=A.pin(out.with_suffix('.proof.json')))
    C.write_json_atomic(contract,spec)
    assert verify.main(['--root',str(tmp_path),'--catalog',str(out),'--tier1-contract',str(contract)])==0
    assert all(calls.count(name)==4 for name in T.T.NAMES)
    assert registry.load()==[stage]
    out.write_bytes(b'corrupt actual catalog')
    assert verify.main(['--root',str(tmp_path),'--catalog',str(out),'--tier1-contract',str(contract)])==1


def test_same_named_invented_view_refuses_adjacent_proof_and_independent_repin(tmp_path,monkeypatch):
    _,stage=world(tmp_path);monkeypatch.setattr(T,'declarations',lambda:[stage])
    monkeypatch.setattr(A,'guard',lambda con:None);monkeypatch.setattr(T.T,'admit',lambda *args:None)
    before,_,_=T.preflight(tmp_path);out=tmp_path/'catalog.duckdb'
    spec={'snapshot_sha256':A.digest(before),'code_pins':T.code_pins(),'admissions':{name:{} for name in T.T.NAMES}}
    T.build(tmp_path,out,spec['snapshot_sha256'],spec['code_pins'],spec['admissions'])
    spec.update(catalog_pin=A.pin(out),proof_pin=A.pin(out.with_suffix('.proof.json')))
    with duckdb.connect(str(out),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute('CREATE OR REPLACE VIEW fixture AS SELECT security_id,999::DOUBLE AS value,available_at FROM '+A.sql([tmp_path/'fixture/year=2021/data.parquet']))
        assert con.execute("SELECT value FROM fixture_as_of(TIMESTAMP '2021-02-02')").fetchone()==(999.,)
    proof=C.read_json(out.with_suffix('.proof.json'));proof['catalog_identity']=A.pin(out)
    C.write_json_atomic(out.with_suffix('.proof.json'),proof)
    with pytest.raises(ValueError,match='independently admitted catalog/proof'):T.verify_catalog(tmp_path,out,spec)
    spec.update(catalog_pin=A.pin(out),proof_pin=A.pin(out.with_suffix('.proof.json')))
    with pytest.raises(ValueError,match='source-derived'):T.verify_catalog(tmp_path,out,spec)


def test_whole_notes_item_key_collision_cannot_be_certified(tmp_path,monkeypatch):
    path=tmp_path/'fundamentals_notes/items/year=2021/items.parquet';path.parent.mkdir(parents=True)
    pq.write_table(pa.table({'adsh':['a','a'],'tag':['tax_dtl','tax_dtl'],'value':[1.,2.]}),path)
    stage=Stage('fundamentals_notes','FUND','notes/v1',None,doc='native fixture',outputs=(Output(
        'fundamentals_notes/items/year=*/items.parquet','fundamentals_notes_items',keys=('adsh','tag')),))
    monkeypatch.setattr(A,'guard',lambda con:None)
    with pytest.raises(ValueError,match='1 collision groups/2 rows'):T.verify_notes_keys(tmp_path,[stage])
