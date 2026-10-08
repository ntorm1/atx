"""Offline real native kernel equivalence and fail-closed consumer identities."""
import datetime as dt
import importlib.util
from pathlib import Path
import shutil
import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from atx_db.alpha_panel import compustat_std as S, private_tier1_accounting as K
from atx_db.alpha_panel import private_tier1_admission as A, tier1_native_readers as T


def test_complete85_preparation_and_native_bucket_equivalence(tmp_path,monkeypatch):
    fixture=importlib.util.spec_from_file_location('std_fixture',Path(__file__).with_name('test_alpha_panel_compustat_std.py'))
    module=importlib.util.module_from_spec(fixture);fixture.loader.exec_module(module)
    world=module.World(tmp_path)
    world.ev(7,'a',dt.datetime(2021,5,1),dt.date(2021,3,31),'Q1')
    for value in (11.,13.): world.fact(7,'NetIncomeLoss',dt.date(2021,1,1),dt.date(2021,3,31),value,'a')
    world.fact(7,'NetIncomeLoss',dt.date(1988,1,1),dt.date(1988,3,31),5.,'a')
    world.fact(7,'NetIncomeLoss',dt.date(2021,1,1),dt.date(2021,3,31),99.,'missing-event')
    world.write('fundamentals_v11');fund=tmp_path/'fundamentals_v11';extract=tmp_path/'extract';extract.mkdir()
    for i in range(85):
        source=fund/'_work/cf'/f'batch-{i:04d}.parquet'
        if i: pq.write_table(pa.Table.from_pylist([],schema=module.CF_SCHEMA),source)
        shutil.copyfile(source,extract/source.name)
    with duckdb.connect(str(tmp_path/'native.duckdb'),config={'memory_limit':'128MB','threads':'1'}) as con:
        con.execute("SET TimeZone='UTC'")
        native=S.prepare_facts(con,fund,tmp_path/'native.parquet',dt.date(2027,10,4))
        actual=K.prepare_facts(con,fund,extract,tmp_path/'actual.parquet',dt.date(2027,10,4))
        assert {k:v for k,v in native.items() if k!='source'} == {k:v for k,v in actual.items() if k!='source'}
        assert actual['batches']==85 and actual['conflicting_duplicates_max_kept']==1
        A.compare(con,tmp_path/'native.parquet',tmp_path/'actual.parquet')
        inp=S.Inputs(fund,tmp_path/'earn_x',tmp_path/'actual.parquet',[],dt.date(2027,10,4))
        peer=tmp_path/'bucket';peer.mkdir();K.write_standard_bucket(con,inp,7%S.N_BUCKETS,peer)
        S.derive_subset(con,inp,f'cik % {S.N_BUCKETS} = {7%S.N_BUCKETS}')
        for product,relation in (('fundq','q_out'),('funda','a_out')):
            from atx_db.alpha_panel import common as C
            C.copy_to_parquet(con,'SELECT * FROM '+relation,tmp_path/(product+'.parquet'),32768)
            A.compare(con,peer/(product+'.parquet'),tmp_path/(product+'.parquet'))
        monkeypatch.setenv('ATX_ALPHA_PANEL_ROOT',str(tmp_path))
        work=tmp_path/'_tmp/bucket';work.mkdir(parents=True);shutil.copyfile(tmp_path/'actual.parquet',work/'facts.parquet')
        C.write_json_atomic(peer/'manifest.json',{'fund_stage':'fundamentals_v11','earn_stage':'earn_x','datadate_window':['1990-01-01','2027-10-04']})
        monkeypatch.setattr(S,'member_ciks',lambda connection:connection.execute('CREATE OR REPLACE TEMP TABLE mem AS SELECT 7::BIGINT cik'))
        original_connect=C.connect
        monkeypatch.setattr(C,'connect',lambda *a,**kw:original_connect(memory='128MB',threads=1,temp_dir=tmp_path/'cutoff-scratch',db_file='cutoff.duckdb'))
        native_cutoff=S.cutoff_check('bucket')
        actual_cutoff=K.cutoff(con,inp,peer)
        assert {k:v for k,v in native_cutoff.items() if k!='seconds'}==actual_cutoff
        assert actual_cutoff['pass']


def test_exact_multiset_retains_duplicate_count_and_schema(tmp_path):
    paths=[tmp_path/'a.parquet',tmp_path/'b.parquet']
    pq.write_table(pa.table({'x':pa.array([1,1],type=pa.int64())}),paths[0])
    pq.write_table(pa.table({'x':pa.array([1],type=pa.int64())}),paths[1])
    with duckdb.connect(config={'memory_limit':'128MB','threads':'1'}) as con:
        with pytest.raises(ValueError,match='multiset'): A.compare(con,*paths)
        pq.write_table(pa.table({'x':pa.array([1,1],type=pa.int32())}),paths[1])
        with pytest.raises(ValueError,match='schema'): A.compare(con,*paths)


def test_independent_pins_missing_changed_and_unknown_interface(tmp_path):
    path=tmp_path/'source';path.write_text('known'); pins={str(path):A.pin(path)}
    with pytest.raises(ValueError,match='missing/substituted'): A.check_pins(pins,{str(tmp_path/'absent'):pins[str(path)]})
    path.write_text('other')
    with pytest.raises(ValueError,match='bytes changed'): A.check_pins(pins)
    with pytest.raises(ValueError,match='fixed reviewed'): T.admit('caller_plugin',{'interface_version':T.VERSION},None)


def test_independent_execution_contract_and_actual_configured_connection(tmp_path,monkeypatch):
    from atx_db.alpha_panel import common as C,privatefund_runtime_resources as R
    path=tmp_path/'execution.json';C.write_json_atomic(path,A.EXECUTION)
    spec={'execution_contract':str(path),'expected_execution_contract_sha256':A.pin(path)['sha256'],
        'expected_adapter_code_sha256':A.digest(A.code(A.ADAPTER_CODE))}
    assert A.execution(spec)[str(path)]==A.pin(path)
    with pytest.raises(ValueError,match='provenance'):
        A.execution({**spec,'expected_adapter_code_sha256':'0'*64})
    path.write_text('{}')
    with pytest.raises(ValueError,match='contract'):A.execution(spec)
    monkeypatch.setattr(R,'guard_identity',lambda:None)
    monkeypatch.setenv('ATX_ALPHA_PANEL_ROOT',str(tmp_path))
    with C.connect(memory='128MB',threads=1,temp_dir=tmp_path/'actual',db_file='configured.duckdb') as con:A.guard(con)
    with C.connect(memory='256MB',threads=1,temp_dir=tmp_path/'too-large',db_file='configured.duckdb') as con:
        with pytest.raises(ValueError,match='128MB'):A.guard(con)
