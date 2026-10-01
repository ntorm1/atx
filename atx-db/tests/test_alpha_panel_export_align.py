"""export_impl.align on a fixture role: consumer role binding keys (dates/instruments), 2024-01-01 seal, export default end."""

from __future__ import annotations

import datetime as dt
import json

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import common
from atx_db.alpha_panel import export_impl as ex

NS = ex.NS_DAY


def _role(path, days, ids):
    path.mkdir(parents=True)
    sess = np.array([(d - ex.EPOCH).days * NS for d in days], dtype="<i8")
    sess.tofile(path / "sessions.i64")
    np.array(ids, dtype="<u8").tofile(path / "ids.u64")
    np.ones((len(days), len(ids)), dtype="u1").tofile(path / "present.u8")
    np.ones((len(days), len(ids)), dtype="u1").tofile(path / "member.u8")
    (path / "manifest.json").write_text(json.dumps({"schema": ex.ROLE_SCHEMA, "status": "complete"}), encoding="utf-8")


def _panel(root, days, ids):
    rows = [{"session_date": d, "security_id": i, "link_tier": "strict", "is_issuer_primary": True, "x": float(i), "n": None if i == 5 else i,
             "t": dt.datetime(1970, 1, 2) if i == 9 else None}
            for d in days for i in ids]
    (root / "panel" / f"year={days[0].year}").mkdir(parents=True)
    pq.write_table(pa.Table.from_pylist(rows, schema=pa.schema([("session_date", pa.date32()), ("security_id", pa.int64()), ("link_tier", pa.string()), ("is_issuer_primary", pa.bool_()), ("x", pa.float64()), ("n", pa.int32()), ("t", pa.timestamp("us"))])), root / "panel" / f"year={days[0].year}" / "p.parquet")
    (root / "fundamentals").mkdir()
    pq.write_table(pa.table({"cik": [1], "at": [1.0]}), root / "fundamentals" / "events.parquet")


def test_align_role_block_has_consumer_keys(monkeypatch, tmp_path):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "lake"))
    root = common.build_root()
    days = [dt.date(2023, 12, 27), dt.date(2023, 12, 28), dt.date(2023, 12, 29)]
    ids = [5, 9]
    _role(tmp_path / "role", days, ids)
    _panel(root, days, ids)
    ex.align(tmp_path / "role", tmp_path / "out", ["x", "n", "t"])
    col = np.fromfile(tmp_path / "out" / "n.f64", dtype="<f8").reshape(3, 2)      # int32 column with a NULL
    assert np.isnan(col[:, 0]).all() and (col[:, 1] == 9).all()
    ts = np.fromfile(tmp_path / "out" / "t.f64", dtype="<f8").reshape(3, 2)       # TIMESTAMP -> epoch seconds
    assert np.isnan(ts[:, 0]).all() and (ts[:, 1] == 86400.0).all()
    role = json.loads((tmp_path / "out" / "manifest.json").read_text(encoding="utf-8"))["role"]
    assert role["dates"] == 3 and role["instruments"] == 2
    assert role["dates"] == role["d"] and role["instruments"] == role["n"]
    assert role["last"] == "2023-12-29"


def test_align_refuses_role_reaching_2024(tmp_path):
    _role(tmp_path / "role", [dt.date(2023, 12, 29), dt.date(2024, 1, 2)], [5])
    with pytest.raises(SystemExit, match="seal"):
        ex.align(tmp_path / "role", tmp_path / "out", ["x"])


def test_export_refuses_end_at_seal_and_defaults_before_it():
    assert ex.SEAL == dt.date(2024, 1, 1)
    with pytest.raises(SystemExit, match="seal"):
        ex.export(None, dt.date(2020, 1, 2), dt.date(2024, 1, 1), [])
    src = open(ex.__file__, encoding="utf-8").read()
    assert 'default="2023-12-31"' in src
