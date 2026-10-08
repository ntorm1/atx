"""Sealed, memory-mapped feature export on the consumer's exact date/ID axes."""
import datetime as dt
import json

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import export_impl as E


def role(path, days):
    path.mkdir()
    np.array([(x - E.EPOCH).days * E.NS_DAY for x in days], dtype="<i8").tofile(path / "sessions.i64")
    np.array([2, 4, 6], dtype="<u8").tofile(path / "ids.u64")
    present = np.ones((len(days), 3), dtype="u1")
    present[0, 1] = 0
    present.tofile(path / "present.u8")
    np.ones((len(days), 3), dtype="u1").tofile(path / "member.u8")
    (path / "manifest.json").write_text(json.dumps({"schema": E.ROLE_SCHEMA, "status": "complete"}))


def test_align_stage_missing_and_absent_stay_nan_and_seal(monkeypatch, tmp_path):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "lake"))
    source = tmp_path / "lake" / "gold" / "year=2023"
    source.mkdir(parents=True)
    day = dt.date(2023, 12, 28)
    pq.write_table(pa.table({"session_date": [day] * 4, "security_id": [2, 4, 6, 9],
                             "g_x": [1., 2., None, 999.]}), source / "gold.parquet")
    (source.parent / "manifest.json").write_text(json.dumps({"status": "complete"}))
    future = source.parent / "year=2024"
    future.mkdir()
    pq.write_table(pa.table({"session_date": [dt.date(2024, 1, 2)], "security_id": [2], "g_x": [999.]}), future / "gold.parquet")
    role(tmp_path / "role", [day, dt.date(2023, 12, 29)])
    E.align_stage(tmp_path / "role", tmp_path / "out")
    vals = np.fromfile(tmp_path / "out" / "g_x.f64", dtype="<f8").reshape(2, 3)
    assert vals[0, 0] == 1 and np.isnan(vals[0, 1:]).all() and np.isnan(vals[1]).all()
    manifest = json.loads((tmp_path / "out" / "manifest.json").read_text())
    assert manifest["role"]["dates"] == 2 and manifest["role"]["instruments"] == 3
    assert manifest["role"]["last"] == "2023-12-29"
    assert manifest["sources"]["feature_stage"] == "gold"
    assert manifest["fields"][0]["coverage"]["finite_member_frac"] == pytest.approx(1/6)


def test_align_stage_refuses_future_role(tmp_path):
    role(tmp_path / "role", [dt.date(2024, 1, 2)])
    with pytest.raises(SystemExit, match="seal"):
        E.align_stage(tmp_path / "role", tmp_path / "out")


def test_align_stage_refuses_unrecognized_stage():
    with pytest.raises(ValueError, match="stage"):
        E.align_stage(None, None, "labels")


def test_market_export_uses_refreshed_silver_and_native_layout(monkeypatch, tmp_path):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "lake"))
    monkeypatch.setenv("ATX_ALPHA_PANEL_STAGE", "panel_goldready")
    root = tmp_path / "lake"
    root.mkdir()
    days = [dt.date(2018, 1, 2) + dt.timedelta(days=k) for k in range(406)]
    pq.write_table(pa.table({"session_date": days}), root / "calendar.parquet")
    vendor = tmp_path / "vendor.parquet"
    vendor.write_bytes(b"source")
    monkeypatch.setattr(E.C, "TICKERHISTORY", vendor)
    for stage, close in (("panel", 999.), ("panel_goldready", 11.)):
        for year in (2018, 2019):
            folder = root / stage / f"year={year}"
            folder.mkdir(parents=True)
            sessions = [d for d in days if d.year == year]
            pq.write_table(pa.table({"session_date": sessions, "security_id": [2] * len(sessions),
                                     "member": [True] * len(sessions), "close": [close] * len(sessions),
                                     "raw_close": [10.] * len(sessions), "volume": [100.] * len(sessions)}), folder / "p.parquet")
        (root / stage / "manifest.json").write_text(json.dumps({"status": "complete", "stage": stage}))
    E.export(tmp_path / "market", days[400], days[-1], [])
    binary = np.fromfile(tmp_path / "market" / "role" / "close.f64", dtype="<f8")
    assert binary.shape == (406,) and np.all(binary == 11.)
    manifest = json.loads((tmp_path / "market" / "role" / "manifest.json").read_text())
    assert manifest["sources"]["panel_stage"] == "panel_goldready"
    assert manifest["score_begin"] == 400 and manifest["dates"] == 406 and manifest["instruments"] == 1
