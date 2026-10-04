"""Task8 publication state machine; economic adapter isolated explicitly here."""
import copy
import json
from pathlib import Path
import pytest
from atx_db.alpha_panel import char_replication as R
from atx_db.alpha_panel.char_monthly_registry import JKP153

def evidence():
    entries={n:{'status':'mapped','native_name':n,'sign':1,'weighting':'vw_cap','metadata_id':'synthetic'} for n in JKP153}
    osap={n:{'status':'excluded','reason':'synthetic unmatched','metadata_id':'synthetic'} for n in JKP153}
    osap['be_me']=dict(entries['be_me'],weighting='equal_weight')
    units={m:{'holding_month':m,'formation':'synthetic-pre2023','population':{},'benchmark_counts':{},'outcomes':{n:{'own':i*.001,'jkp':i*.002,'osap':i*.003 if n=='be_me' else None,'reason':None} for n in JKP153}} for i,m in enumerate(R.MONTHS)}
    return units,entries,osap


@pytest.fixture
def publication(tmp_path,monkeypatch):
    monkeypatch.setenv('ATX_GUARD_JOB','synthetic-only')
    units,jkp,osap=evidence();binding=tmp_path/'bindings.json';binding.write_text('{}')
    snapshot={'version':R.VERSION,'config':R.CONFIG,'binding_path':str(binding),'synthetic_state':'fixed'}
    context={'jkp_doc':{'entries':jkp},'osap_doc':{'entries':osap}}
    calls=[]
    monkeypatch.setattr(R,'freeze',lambda *args:(copy.deepcopy(snapshot),context))
    monkeypatch.setattr(R,'recheck',lambda snap:None)
    def month(root,snap,ctx,con,holding):
        calls.append(holding)
        return dict(copy.deepcopy(units[holding]),version=R.VERSION,snapshot_sha256=R.R.digest(snap))
    monkeypatch.setattr(R,'monthly_receipt',month)
    return tmp_path,binding,snapshot,units,calls


def test_bounded_restart_replays_prior_units_and_full_public_validation(publication):
    root,binding,snapshot,units,calls=publication
    result=R.build(root,binding,'synthetic',1)
    assert result['complete_months']==1 and not result['published']
    generation=R.generation_path(root,'synthetic')
    assert not (root/R.STAGE/'current.json').exists()
    first=(generation/'months/2013-01/receipt.json').read_bytes()
    R.build(root,binding,'synthetic',1)
    assert calls==['2013-01','2013-01','2013-02']
    assert first==(generation/'months/2013-01/receipt.json').read_bytes()
    result=R.build(root,binding,'synthetic')
    assert result['published'] and result['readiness']=='NOT_PRODUCTION_READY'
    assert (root/'validation/char_replication.json').is_file()
    assert len(calls)==243  # 1 + 2 + 120 build + 120 mandatory public replay
    con=R.C.connect(memory='128MB',threads=1)
    try:
        assert R.validate_generation(root,generation,con)['status']=='complete'
        assert len(calls)==363
        report=generation/'char_replication.json'
        doc=json.loads(report.read_text());doc['gates']['jkp_mean_sign_share']['denominator']=152
        report.write_text(json.dumps(doc))
        with pytest.raises(ValueError,match='tampered'):R.validate_generation(root,generation,con)
    finally:con.close()


@pytest.mark.parametrize('damage',['stale','tampered','unsealed'])
def test_restart_fail_closed_without_publishing(publication,damage):
    root,binding,snapshot,units,calls=publication
    R.build(root,binding,'synthetic',1);generation=R.generation_path(root,'synthetic')
    if damage=='stale':snapshot['synthetic_state']='changed'
    elif damage=='tampered':
        receipt=generation/'months/2013-01/receipt.json'
        doc=json.loads(receipt.read_text());doc['outcomes']['age']['own']=999
        receipt.write_text(json.dumps(doc))
    else:(generation/'snapshot.json').unlink()
    with pytest.raises(ValueError):R.build(root,binding,'synthetic',1)
    assert not (root/R.STAGE/'current.json').exists()
    assert not (root/R.STAGE/'.writer.lock').exists()


def test_interrupted_month_preserves_attempt_and_immutable_no_overwrite(publication,monkeypatch):
    root,binding,snapshot,units,calls=publication
    def failed(*args):raise RuntimeError('synthetic interruption')
    monkeypatch.setattr(R,'monthly_receipt',failed)
    with pytest.raises(RuntimeError):R.build(root,binding,'synthetic',1)
    generation=R.generation_path(root,'synthetic')
    assert len(list((generation/'months/2013-01/attempts').glob('*/start.json')))==1
    assert not (generation/'months/2013-01/receipt.json').exists()
    target=root/'immutable.json';R.immutable(target,{'first':1})
    with pytest.raises(FileExistsError):R.immutable(target,{'second':2})
    assert json.loads(target.read_text())=={'first':1}
    assert list(root.glob('immutable.json.*.partial'))


def test_guard_and_budget_checked_before_admission(tmp_path,monkeypatch):
    monkeypatch.delenv('ATX_GUARD_JOB',raising=False)
    with pytest.raises(RuntimeError):R.build(tmp_path,tmp_path/'absent','safe',1)
    monkeypatch.setenv('ATX_GUARD_JOB','synthetic')
    with pytest.raises(ValueError):R.build(tmp_path,tmp_path/'absent','safe',0)
    assert not (tmp_path/R.STAGE).exists()
