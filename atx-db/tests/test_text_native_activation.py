"""R1 finding3: authenticated rename-before-pointer recovery and intervention."""
import json
import pytest
from atx_db.alpha_panel import common as C,filing_text_native as N,text_features_native as P
from tests.test_text_features_native import native


def test_interrupted_activation_requires_exact_transition_and_keeps_history(native,monkeypatch):
    root,output,plan=native
    output.mkdir()
    prior={'generation':'previous','manifest_sha256':'old'}
    C.write_json_atomic(output/'active.json',prior)
    writer=C.write_json_atomic
    def fail_pointer(path,value):
        if path.name=='active.json':raise InterruptedError('pointer interruption')
        return writer(path,value)
    monkeypatch.setattr(C,'write_json_atomic',fail_pointer)
    with pytest.raises(InterruptedError):P.publish_native(root,output,plan,N.digest(plan))
    assert json.loads((output/'active.json').read_text())==prior
    generation=next((output/'generations').iterdir())
    manifest=N.pin(generation/'manifest.json')['sha256']
    transition=output/'transitions'/f'{generation.name}.json'
    assert json.loads(transition.read_text())['previous_pointer']==prior
    monkeypatch.setattr(C,'write_json_atomic',writer)
    with pytest.raises(ValueError,match='transition required'):
        P.publish_native(root,output,plan,N.digest(plan),expected_manifest_sha256=manifest)
    writer(output/'active.json',{'generation':'intervening','manifest_sha256':'other'})
    kwargs=dict(expected_manifest_sha256=manifest,expected_activation_transition_sha256=N.pin(transition)['sha256'])
    with pytest.raises(ValueError,match='intervening activation'):
        P.publish_native(root,output,plan,N.digest(plan),**kwargs)
    writer(output/'active.json',prior)
    assert P.publish_native(root,output,plan,N.digest(plan),**kwargs)==generation
    assert json.loads((output/'active.json').read_text())['generation']==generation.name
