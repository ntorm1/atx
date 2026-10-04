"""Finalization interruptions; synthetic economics isolated by the shared fixture."""
import json
import pytest
from atx_db.alpha_panel import char_replication as R
from tests.test_alpha_panel_char_replication_publication import publication


def interrupt_after(monkeypatch,boundary):
    immutable,atomic=R.immutable,R.atomic_pointer;fired=[]
    def fail(name):
        if name==boundary and not fired:
            fired.append(name);raise RuntimeError('injected finalization interruption')
    def final(path,value):
        immutable(path,value)
        if path.name in ('char_replication.json','manifest.json'):fail(path.name)
    def pointer(path,payload):
        atomic(path,payload)
        fail('current' if path.name=='current.json' else 'validation')
    monkeypatch.setattr(R,'immutable',final);monkeypatch.setattr(R,'atomic_pointer',pointer)
    return immutable,atomic,fired


@pytest.mark.parametrize('boundary',['char_replication.json','manifest.json','current','validation'])
def test_interrupt_finalization_preserves_and_recovers_exact_artifacts(publication,monkeypatch,boundary):
    root,binding,snapshot,units,calls=publication
    original,atomic,fired=interrupt_after(monkeypatch,boundary)
    with pytest.raises(RuntimeError,match='injected'):R.build(root,binding,'candidate')
    assert fired==[boundary]
    paths=R.pointer_paths(root);assert all(not p.exists() for p in paths.values())
    generation=R.generation_path(root,'candidate')
    saved={p.name:p.read_bytes() for p in (generation/'char_replication.json',generation/'manifest.json') if p.exists()}
    assert len(list((generation/'months').glob('*/receipt.json')))==120
    monkeypatch.setattr(R,'immutable',original);monkeypatch.setattr(R,'atomic_pointer',atomic)
    before=len(calls);result=R.build(root,binding,'candidate',max_new_months=1)
    assert result['published'] and len(calls)-before>=120
    assert all((generation/name).read_bytes()==data for name,data in saved.items())
    assert not (root/R.STAGE/'publication.pending.json').exists()
    assert len(list((root/R.STAGE/'publications').glob('*/complete.json')))==1
    current,validation=[json.loads(paths[key].read_text()) for key in ('current','validation')]
    assert current['generation']==validation['generation']=='candidate' and current['manifest']==validation['manifest']
    assert validation['report']['gate_status']['source_admission']=='FAIL'
    assert validation['report']['gates']['source_admission']['measured'] is None


@pytest.mark.parametrize('final',['char_replication.json','manifest.json'])
def test_different_existing_final_bytes_never_overwritten(publication,monkeypatch,final):
    root,binding,snapshot,units,calls=publication
    original,atomic,_=interrupt_after(monkeypatch,final)
    with pytest.raises(RuntimeError):R.build(root,binding,'candidate')
    monkeypatch.setattr(R,'immutable',original);monkeypatch.setattr(R,'atomic_pointer',atomic)
    path=R.generation_path(root,'candidate')/final
    # Even refreshed hashes cannot authenticate different immutable final bytes.
    altered=path.read_bytes()+b' ';path.write_bytes(altered)
    with pytest.raises(ValueError,match='immutable final'):R.build(root,binding,'candidate')
    assert path.read_bytes()==altered and all(not p.exists() for p in R.pointer_paths(root).values())


def test_completed_republication_is_idempotent_full_native_replay(publication):
    root,binding,snapshot,units,calls=publication;R.build(root,binding,'candidate')
    saved={str(p):p.read_bytes() for p in R.pointer_paths(root).values()}
    before=len(calls);R.build(root,binding,'candidate',1)
    assert len(calls)-before==120 and saved=={str(p):p.read_bytes() for p in R.pointer_paths(root).values()}
    assert len(list((root/R.STAGE/'publications').glob('*/intent.json')))==1
