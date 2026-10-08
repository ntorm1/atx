import pytest
import pyarrow.parquet as pq
from atx_db.alpha_panel import mna_native_v2 as N
from atx_db.alpha_panel import mna_runtime_v2 as R


def test_i5_actual_loaded_reader_and_timezone_identity_bound(monkeypatch):
    expected=N.code_pins()
    assert 'pyarrow._parquet' in expected['runtime']['files'] and '_duckdb' in expected['runtime']['files']
    assert expected['runtime']['timezone']['sha256']
    original=pq.read_table
    def changed(*args,**kw): return original(*args,**kw)
    monkeypatch.setattr(pq,'read_table',changed)
    with pytest.raises(ValueError,match='reader/code identity'):
        N.replay_native({},(),admission_id='fixture',expected_code=expected)
    monkeypatch.setattr(pq,'read_table',original)
    assert R.runtime_identity()==expected['runtime']
    reduced=dict(expected);reduced.pop('runtime')
    with pytest.raises(ValueError,match='reader/code identity'):
        N.replay_native({},(),admission_id='fixture',expected_code=reduced)
    original_files=R.importlib.resources.files
    class ChangedZone:
        def joinpath(self,*args): return self
        def read_bytes(self): return b'changed-timezone-data'
    monkeypatch.setattr(R.importlib.resources,'files',lambda name: ChangedZone() if name=='tzdata.zoneinfo' else original_files(name))
    with pytest.raises(ValueError,match='reader/code identity'):
        N.replay_native({},(),admission_id='fixture',expected_code=expected)
