"""Prior pointer restoration at immutable report/manifest boundaries, bounded file."""
import json
import pytest
from atx_db.alpha_panel import char_replication as R
from tests.test_alpha_panel_char_replication_publication import publication
from tests.test_alpha_panel_char_replication_finalization import interrupt_after


@pytest.mark.parametrize('boundary',['char_replication.json','manifest.json'])
def test_prior_publication_preserved_at_immutable_finals(publication,monkeypatch,boundary):
    root,binding,snapshot,units,calls=publication;R.build(root,binding,'previous')
    paths=R.pointer_paths(root);prior={k:p.read_bytes() for k,p in paths.items()}
    original,atomic,fired=interrupt_after(monkeypatch,boundary)
    with pytest.raises(RuntimeError):R.build(root,binding,'candidate')
    assert fired==[boundary] and prior=={k:p.read_bytes() for k,p in paths.items()}
    monkeypatch.setattr(R,'immutable',original);monkeypatch.setattr(R,'atomic_pointer',atomic)
    R.build(root,binding,'candidate',1)
    assert all(json.loads(p.read_text())['generation']=='candidate' for p in paths.values())
    assert len(list((root/R.STAGE/'publications').glob('*/complete.json')))==2
