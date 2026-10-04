"""Real native pointer ordering with interrupted alias activation and rollback."""
from pathlib import Path
import pytest
from atx_db.alpha_panel import common as C, fund_batch_release as F
from atx_db.alpha_panel import tier1_native_readers as T, private_tier1_admission as A


def generation(stage,name):
    path=stage/'generations'/name;path.mkdir(parents=True)
    C.write_json_atomic(path/'manifest.json',{'status':'complete','generation':name})
    return path


def test_interruption_refuses_consumption_and_full_replay_precedes_recovery(tmp_path,monkeypatch):
    stage=tmp_path/'compustat_std_v11';old=generation(stage,'old');new=generation(stage,'new')
    F._publish_pointer(stage,old,A.pin(old/'manifest.json'));C.write_json_atomic(stage/'manifest.json',C.read_json(old/'manifest.json'))
    original=T._alias
    def interrupt(*args):raise RuntimeError('interrupt after native pointer')
    monkeypatch.setattr(T,'_alias',interrupt)
    with pytest.raises(RuntimeError,match='interrupt'): T._activate_locked(stage,new,A.pin(new/'manifest.json'))
    assert C.read_json(stage/'current.json')['generation']=='new'
    assert C.read_json(stage/'previous.json')['generation']=='old'
    with pytest.raises(ValueError,match='unfinished'): T.resolve_current(stage)
    monkeypatch.setattr(T,'_alias',original)
    monkeypatch.setattr(A,'guard',lambda con:None);monkeypatch.setattr(A,'execution',lambda spec:{})
    monkeypatch.setattr(T,'_generation',lambda spec,name:(tmp_path,stage,new))
    order=[]
    def validated(name,spec,con):
        order.append('full-replay'); assert (stage/'activation.json').exists(); return {'admitted':True}
    monkeypatch.setattr(T,'validate',validated)
    spec={'manifest_pin':A.pin(new/'manifest.json')}
    assert T.recover('standardized_v11',spec,None)=={'admitted':True}
    assert order==['full-replay'] and T.resolve_current(stage)==new
    assert not (stage/'activation.json').exists()
    monkeypatch.setattr(T,'_generation',lambda spec,name:(tmp_path,stage,old))
    monkeypatch.setattr(T,'validate',lambda *args:{'rollback':True})
    assert T.activate('standardized_v11',{'manifest_pin':A.pin(old/'manifest.json')},None,rollback=True)=={'rollback':True}
    assert T.resolve_current(stage)==old


def test_substituted_recovery_manifest_never_finishes_pointer(tmp_path,monkeypatch):
    stage=tmp_path/'ratios_v11';new=generation(stage,'new');pin=A.pin(new/'manifest.json')
    C.write_json_atomic(stage/'activation.json',{'version':T.VERSION,'current':{'generation':'new','manifest':pin},'previous':None})
    C.write_json_atomic(new/'manifest.json',{'status':'changed'})
    monkeypatch.setattr(A,'guard',lambda con:None);monkeypatch.setattr(A,'execution',lambda spec:{})
    monkeypatch.setattr(T,'_generation',lambda spec,name:(tmp_path,stage,new))
    with pytest.raises(ValueError,match='identity mismatch'): T.recover('ratios_v11',{'manifest_pin':pin},None)
    assert not (stage/'current.json').exists() and (stage/'activation.json').exists()
