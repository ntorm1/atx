"""Exact authenticated catalog/proof transitions retain unrelated generations."""
from pathlib import Path
import pytest
from atx_db.alpha_panel import common as C, private_tier1_admission as A
from atx_db.alpha_panel import private_tier1_catalog_transaction as T


def pair(path,value):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_bytes(value)
    path.with_suffix('.proof.json').write_bytes(value+b'-proof')
    return {'catalog':A.pin(path),'proof':A.pin(path.with_suffix('.proof.json'))}


def test_interrupted_two_file_transition_preserves_prior_and_finishes_exact_target(tmp_path,monkeypatch):
    target=tmp_path/'catalog.duckdb';previous=pair(target,b'a')
    candidate=tmp_path/'catalog-attempt-fixture'/target.name;expected=pair(candidate,b'b')
    original=T.os.replace
    def interrupt(source,destination):
        if Path(destination)==target.with_suffix('.proof.json'):raise RuntimeError('after catalog replacement')
        return original(source,destination)
    monkeypatch.setattr(T.os,'replace',interrupt)
    with pytest.raises(RuntimeError,match='after catalog'):
        T.publish(target,candidate,'source',{},previous)
    intent=C.read_json(T.journal(target));assert A.pin(target)==expected['catalog']
    assert A.pin(target.with_suffix('.proof.json'))==previous['proof']
    assert (candidate.parent/'previous'/target.name).read_bytes()==b'a'
    monkeypatch.setattr(T.os,'replace',original)
    T.finish(target,intent,A.digest(intent))
    assert A.pin(target)==expected['catalog'] and A.pin(target.with_suffix('.proof.json'))==expected['proof']
    assert not T.journal(target).exists() and candidate.read_bytes()==b'b'


def test_recovery_refuses_unrelated_pair_and_mutated_intent(tmp_path):
    target=tmp_path/'catalog.duckdb';previous=pair(target,b'a')
    candidate=tmp_path/'catalog-attempt-fixture'/target.name;new=pair(candidate,b'b')
    intent={'version':T.VERSION,'destination':str(target.resolve()),'candidate':str(candidate.resolve()),
        'previous':previous,'target':new,'snapshot_sha256':'source','code_pins':{}}
    C.write_json_atomic(T.journal(target),intent)
    unrelated=pair(target,b'c')
    with pytest.raises(ValueError,match='unrelated'):T.finish(target,intent,A.digest(intent))
    assert A.pin(target)==unrelated['catalog'] and A.pin(target.with_suffix('.proof.json'))==unrelated['proof']
    C.write_json_atomic(T.journal(target),{**intent,'snapshot_sha256':'changed'})
    with pytest.raises(ValueError,match='intent changed'):T.finish(target,intent,A.digest(intent))
