"""Mixed hard-stop pointer/source drift and economic tampering, bounded file."""
import pytest
from atx_db.alpha_panel import char_replication as R
from tests.test_alpha_panel_char_replication_publication import publication
from tests.test_alpha_panel_char_replication_finalization import interrupt_after


def test_hard_stop_mixed_pair_is_restored_before_stale_source_reject(publication,monkeypatch):
    root,binding,snapshot,units,calls=publication;R.build(root,binding,'previous')
    paths=R.pointer_paths(root);prior={k:p.read_bytes() for k,p in paths.items()}
    original,atomic,_=interrupt_after(monkeypatch,'current')
    with pytest.raises(RuntimeError):R.build(root,binding,'candidate')
    monkeypatch.setattr(R,'immutable',original);monkeypatch.setattr(R,'atomic_pointer',atomic)
    pending=R.load_pending(root,'candidate',None)
    # Emulate an uncatchable process termination after its first atomic write.
    atomic(paths['current'],R.json_bytes(pending[1]['desired']['current']))
    snapshot['synthetic_state']='source_changed'
    with pytest.raises(ValueError):R.build(root,binding,'candidate',1)
    assert prior=={k:p.read_bytes() for k,p in paths.items()}
    assert (root/R.STAGE/'publication.pending.json').exists()
    assert not (root/R.STAGE/'.writer.lock').exists()


def test_pending_target_cannot_publish_different_replayed_contents(publication,monkeypatch):
    root,binding,snapshot,units,calls=publication
    original,atomic,_=interrupt_after(monkeypatch,'current')
    with pytest.raises(RuntimeError):R.build(root,binding,'candidate')
    monkeypatch.setattr(R,'immutable',original);monkeypatch.setattr(R,'atomic_pointer',atomic)
    units['2013-01']['outcomes']['be_me']['own']=99.
    with pytest.raises(ValueError,match='economic evidence'):R.build(root,binding,'candidate',1)
    assert all(not p.exists() for p in R.pointer_paths(root).values())
    assert (root/R.STAGE/'publication.pending.json').exists()
