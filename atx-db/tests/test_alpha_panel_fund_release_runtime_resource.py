"""Small real native issuer join and typed replay using the R23 resource runtime."""
import json
import importlib.util
from pathlib import Path
import pytest

from atx_db.alpha_panel import fund_batch_release as N
from atx_db.alpha_panel import fund_release_runtime as A
from atx_db.alpha_panel import privatefund_runtime_resources as R

spec = importlib.util.spec_from_file_location('native_fixture_reference', Path(__file__).with_name('test_fund_batch_release.py'))
reference = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reference)
fixture_binding = reference.fixture_binding


def test_filebacked256mb_real_native_issuer_join_and_typed_replay(tmp_path):
    runtime_path = tmp_path/'execution.json'; runtime_path.write_text(json.dumps(A.CONTRACT), encoding='utf-8')
    runtime = A.load_runtime(runtime_path, N.identity(runtime_path)['sha256'], N.digest(A.code_inventory()))
    fixture_root = tmp_path/'fixture'; fixture_root.mkdir()
    snapshot = fixture_binding(fixture_root)
    first = tmp_path/'first'; first.mkdir(); second = tmp_path/'second'; second.mkdir()
    globals_before = (N.C.connect, N.C.build_root, N.F.common, N.F.stream_issuers)
    with A.connection(fixture_root/N.STAGE, runtime) as (con, evidence):
        assert evidence['guard']['cap_bytes'] <= R.GIB
        assert evidence['duckdb_actual'] == ['244.1 MiB', 1, 'UTC']
        N._replay(snapshot, con, 0, first)
        N._replay(snapshot, con, 0, second)
        rows = N.validate_payloads(con, first, 0, fixture_root/'extract/batch-0000.parquet', second)
        assert rows['events']['rows'] == 1 and rows['catalog']['rows'] == 1
        assert N.receipt_semantics(first/'batch-0000.json') == N.receipt_semantics(second/'batch-0000.json')
        event = con.execute('SELECT "at", available_at=clock_utc FROM '+N.sql([first/'events-0000.parquet'])).fetchone()
        assert event == (1000.0, True)
    assert (N.C.connect, N.C.build_root, N.F.common, N.F.stream_issuers) == globals_before


def test_connection_refuses_actual128mb_even_with256mb_contract(tmp_path, monkeypatch):
    runtime_path = tmp_path/'execution.json'; runtime_path.write_text(json.dumps(A.CONTRACT), encoding='utf-8')
    runtime = A.load_runtime(runtime_path, N.identity(runtime_path)['sha256'], N.digest(A.code_inventory()))
    monkeypatch.setattr(R, 'connect', lambda stage: N.C.connect(memory='128MB', threads=1,
                      temp_dir=stage/'_spill', db_file='negative128.duckdb'))
    with pytest.raises(ValueError, match='actual runtime DuckDB settings'):
        with A.connection(tmp_path/N.STAGE, runtime):
            pytest.fail('declared contract must not conceal actual128MB connection')
