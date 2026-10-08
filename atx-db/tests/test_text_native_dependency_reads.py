"""R1 finding4: full dependency checks at bounded consumption and early close."""
import pytest
from atx_db.alpha_panel import filing_text_native as N,text_features_native as P
from tests.test_text_features_native import native


def test_full_source_code_runtime_dependency_checks_on_read_and_close(native,monkeypatch):
    root,output,plan=native
    helper=root/'synthetic-code.py'
    helper.write_text('synthetic helper version one')
    plan['code'][str(helper.resolve())]=N.pin(helper)
    generation=P.publish_native(root,output,plan,N.digest(plan))
    admission=P.read_native_generation(root,generation,N.pin(generation/'manifest.json')['sha256'],N.digest(plan))
    original=helper.read_text()
    helper.write_text('changed code')
    with pytest.raises(ValueError,match='pinned code'):next(P.iter_native_rows(admission,'text/features.parquet'))
    helper.write_text(original)
    source=root/plan['originals'][0]['path']
    reader=P.iter_native_rows(admission,'text/features.parquet',batch_size=1)
    next(reader)
    raw=source.read_bytes()
    source.write_bytes(raw+b'changed source')
    with pytest.raises(ValueError,match='pinned source'):reader.close()
    source.write_bytes(raw)
    runtime=N.runtime_identity()
    monkeypatch.setattr(N,'runtime_identity',lambda:{**runtime,'python':'changed'})
    with pytest.raises(ValueError,match='runtime mismatch'):next(P.iter_native_rows(admission,'text/features.parquet'))
