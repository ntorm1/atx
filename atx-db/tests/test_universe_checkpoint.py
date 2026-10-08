"""Offline authenticated year continuation and refusal regressions."""
import datetime as dt
from pathlib import Path
import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from atx_db.alpha_panel import common as C,universe as U,universe_checkpoint as Q

@pytest.fixture
def build_lake(tmp_path,monkeypatch):
    monkeypatch.setenv('ATX_ALPHA_PANEL_ROOT',str(tmp_path));monkeypatch.setattr(U,'YEARS',range(2021,2023))
    for name in (*U.INPUTS,*U.COVERAGE_INPUTS):
        path=tmp_path/name;path=path if path.suffix=='.json' else path/'manifest.json'
        path.parent.mkdir(parents=True,exist_ok=True);C.write_json_atomic(path,{'files':{}})
    pq.write_table(pa.table({'session_date':[dt.date(2021,1,4),dt.date(2022,1,4)]}),tmp_path/'calendar.parquet')
    for year in U.YEARS:
        p=tmp_path/'panel'/f'year={year}'/'panel.parquet';p.parent.mkdir(parents=True)
        pq.write_table(pa.table({'session_date':[dt.date(year,1,4)]}),p)
    class Con:
        def execute(self,sql,args=None):
            self.dates=[(dt.date(int(Path(args[0][0]).parent.name[5:]),1,4),)] if args else []
            return self
        def fetchall(self):return self.dates
        def close(self):pass
    con=Con();monkeypatch.setattr(C,'connect',lambda **kwargs:con)
    monkeypatch.setattr(U,'load_issuer_tables',lambda con:{})
    built=[];current=[None]
    def build(con,year):built.append(year);current[0]=year
    monkeypatch.setattr(U,'build_year',build)
    monkeypatch.setattr(U,'year_receipt',lambda con:[[dt.date(current[0],1,4)]+[1]*len(U.DAILY_COLS)])
    def copy(con,sql,path,**kwargs):
        path.parent.mkdir(parents=True,exist_ok=True);pq.write_table(pa.table({'year':[current[0]]}),path);return 1
    monkeypatch.setattr(C,'copy_to_parquet',copy)
    monkeypatch.setattr(U,'band_gate',lambda *a,**k:{'pass':True,'per_year':{}})
    monkeypatch.setattr(U,'measure_french',lambda con:{'pass':False})
    monkeypatch.setattr(U,'measure_r2000',lambda con:{'pass':True})
    monkeypatch.setattr(U,'measure_counts',lambda con:{})
    monkeypatch.setattr(U,'coverage',lambda con:{})
    monkeypatch.setattr(U,'hand_check_sample',lambda con:[])
    monkeypatch.setattr(U,'hand_check_gate',lambda *a,**k:{'pass':True})
    return tmp_path,built,current

def test_bounded_fresh_resume_and_uncapped_payload_equivalence(build_lake):
    root,built,_=build_lake
    assert U.build(max_new_years=0)['status']=='partial_cache_only' and not built
    first=U.build(max_new_years=1)
    assert first['missing_years']==[2022] and first['published'] is False
    assert not (root/'universe/manifest.json').exists() and not (root/'validation/universe.json').exists()
    assert not list((root/'universe').rglob('*.parquet'))
    final=U.build(max_new_years=1)
    assert final['published'] and final['resumed_years']==[2021] and built==[2021,2022]
    before=C.output_hashes(root/'universe')
    assert U.build(max_new_years=0)['resumed_years']==[2021,2022]
    assert C.output_hashes(root/'universe')==before
    assert C.read_json(root/'universe/manifest.json')['gate_status']['per_gate']['french_nyse'] is False

@pytest.mark.parametrize('damage',['payload','receipt','daily','rows','legacy','partial','input','code'])
def test_corrupt_stale_or_incomplete_cache_refuses_without_publication(build_lake,damage,monkeypatch):
    root,built,_=build_lake;U.build(max_new_years=1)
    cache=root/'_tmp/universe-authenticated-v1';rec=cache/'daily_2021.json';payload=cache/'year=2021/universe.parquet'
    if damage=='payload':payload.write_bytes(b'corrupt')
    elif damage=='receipt':rec.write_text('{')
    elif damage in ('daily','rows','legacy'):
        value=C.read_json(rec)
        if damage=='daily':value['daily']={}
        elif damage=='rows':value['rows']=0
        else:value={'rows':1,'daily':value['daily']}
        C.write_json_atomic(rec,value)
    elif damage=='partial':rec.unlink()
    elif damage=='input':
        p=root/'panel/year=2021/panel.parquet';pq.write_table(pa.table({'session_date':[dt.date(2021,1,5)]}),p)
    else:monkeypatch.setattr(C,'code_identity',lambda *a:{'changed.py':{'sha256_lf':'changed'}})
    with pytest.raises(RuntimeError):U.build(max_new_years=1)
    assert not (root/'universe/manifest.json').exists() and built==[2021]
    assert payload.exists() and rec.exists() is (damage!='partial')

def test_narrow_year_selection_cannot_publish_shorter_acceptance(build_lake):
    root,built,_=build_lake
    result=U.build(range(2021,2022))
    assert result['status']=='partial_cache_only' and result['missing_years']==[2022]
    assert not (root/'universe/manifest.json').exists()

def test_interrupted_year_leaves_no_receipt_or_publication(build_lake,monkeypatch):
    root,built,_=build_lake
    original=U.build_year
    def interrupt(con,y):
        if y==2022:raise KeyboardInterrupt('fixture guard stop')
        original(con,y)
    monkeypatch.setattr(U,'build_year',interrupt)
    with pytest.raises(KeyboardInterrupt):U.build()
    assert not (root/'universe/manifest.json').exists()
    monkeypatch.setattr(U,'build_year',original)
    assert U.build()['resumed_years']==[2021]
    assert built==[2021,2022]

@pytest.mark.parametrize('moment',['year','gates'])
def test_input_change_during_work_refuses_final_manifest(build_lake,monkeypatch,moment):
    root,built,_=build_lake
    def change():
        pq.write_table(pa.table({'session_date':[dt.date(2021,1,5)]}),root/'calendar.parquet')
    if moment=='year':
        original=U.build_year
        def build(con,year):original(con,year);change()
        monkeypatch.setattr(U,'build_year',build)
    else:monkeypatch.setattr(U,'measure_french',lambda con:(change() or {'pass':False}))
    with pytest.raises(RuntimeError,match='changed'):U.build()
    assert not (root/'universe/manifest.json').exists()

def test_unbudgeted_build_matches_bounded_payloads(build_lake):
    root,built,_=build_lake
    U.build(max_new_years=1);U.build(max_new_years=1)
    bounded=C.output_hashes(root/'universe')
    # Distinct empty scratch simulates a fresh uncapped invocation; old cache is retained.
    cache=root/'_tmp/universe-authenticated-v1';cache.rename(root/'_tmp/preserved-authenticated-cache')
    U.build()
    assert C.output_hashes(root/'universe')==bounded and built==[2021,2022,2021,2022]

def test_resume_only_refuses_input_change_before_partial_return(build_lake,monkeypatch):
    root,built,_=build_lake
    def mutate(con):
        pq.write_table(pa.table({'session_date':[dt.date(2021,1,5)]}),root/'calendar.parquet')
        return {}
    monkeypatch.setattr(U,'load_issuer_tables',mutate)
    with pytest.raises(RuntimeError,match='changed'):U.build(max_new_years=0)
    assert not built and not (root/'universe/manifest.json').exists()
