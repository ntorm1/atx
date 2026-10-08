from __future__ import annotations

import datetime as dt
import json
import math
from pathlib import Path

import duckdb
import pytest

from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import labels as L
from atx_db.alpha_panel import labels_holdout as H


def write(con, rows, columns: str, path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    con.execute(f"CREATE OR REPLACE TABLE fixture({columns})")
    con.executemany(f"INSERT INTO fixture VALUES({','.join('?' for _ in columns.split(','))})", rows)
    con.execute(f"COPY fixture TO '{path.as_posix()}' (FORMAT PARQUET)")


@pytest.fixture()
def holdout_lake(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    cal = [dt.date(2024, 1, 1) + dt.timedelta(days=i) for i in range(130)]
    cutoff = cal[100]
    con = duckdb.connect()
    write(con, [(d,) for d in cal], "session_date DATE", tmp_path / "calendar.parquet")
    rows, repairs = [], []
    for i, d in enumerate(cal):
        for sid in range(1, 12):
            if (sid == 2 and i == 11) or (sid == 3 and i == 15) or (sid in (4, 5, 6) and i > 15):
                continue
            r = .01 if i <= 100 else 999999.0
            rows.append((d, sid, r, sid == 11 and i == 14, 0.0 if sid == 7 and i == 11 else 10.0))
            repaired = (sid == 8 and i == 14) or (sid == 9 and i == 10) or (sid == 10 and i == 11)
            repairs.append((d, sid, repaired))
    write(con, rows, "session_date DATE,security_id BIGINT,ret DOUBLE,ret_guarded BOOLEAN,raw_close DOUBLE",
          tmp_path / "panel_goldready/year=2024/panel-01.parquet")
    write(con, repairs, "session_date DATE,security_id BIGINT,sid0_repaired BOOLEAN",
          tmp_path / "prices/year=2024/prices.parquet")
    write(con, [(4, cal[15], False, -.3, False), (5, cal[15], False, None, False), (6, cal[15], False, -.3, True)],
          "security_id BIGINT,last_session DATE,continued BOOLEAN,dlret DOUBLE,dlret_imputed BOOLEAN",
          tmp_path / "delisting/events.parquet")
    for stage in ("panel_goldready", "prices", "delisting"):
        (tmp_path / stage / "manifest.json").write_text(json.dumps({"status": "complete"}))
    freeze = tmp_path / "freeze.json"
    freeze.write_text("fixture freeze")
    verified = {"panel_stage": "panel_goldready", "horizons": [5, 21, 63],
                "oos_start": "2024-01-01", "oos_cutoff": str(cutoff)}
    calls = []
    def verify(path, authorized):
        calls.append((path, authorized))
        if not authorized:
            raise ValueError("not authorized")
        return verified
    monkeypatch.setattr(H, "verify_freeze", verify)
    yield tmp_path, con, cal, cutoff, freeze, calls
    con.close()


def load(con, root):
    rows = con.execute(f"SELECT * FROM read_parquet('{(root / 'labels_holdout/year=2024/labels.parquet').as_posix()}',hive_partitioning=false)").fetchall()
    names = [col[0] for col in con.description]
    return {(row[0], row[1]): dict(zip(names, row, strict=True)) for row in rows}


def test_strict_holdout_arithmetic_attrition_and_latest_endpoint(holdout_lake):
    root, con, cal, cutoff, freeze, calls = holdout_lake
    before_cutoff = L.LABEL_CUTOFF
    receipt = H.build(freeze, authorized_oos=True, memory="160MB")
    out = load(con, root)
    row = out[(cal[10], 1)]
    for h in H.HORIZONS:
        assert row[f"fwd_{h}"] == pytest.approx(h * math.log1p(.01))
        assert row[f"n_{h}"] == h
        assert row[f"quality_{h}"] == 1
    for sid in (2, 3, 7, 11):
        assert out[(cal[10], sid)]["quality_5"] == 5
        assert out[(cal[10], sid)]["fwd_5"] is None
    terminal = out[(cal[10], 4)]
    assert terminal["quality_5"] == 2
    assert terminal["fwd_5"] == pytest.approx(4 * math.log1p(.01) + math.log(.7))
    assert out[(cal[10], 5)]["quality_5"] == 3
    assert out[(cal[10], 6)]["quality_5"] == 4
    assert out[(cal[10], 8)]["quality_5"] == 8
    assert out[(cal[10], 9)]["quality_5"] == 7
    assert out[(cal[10], 10)]["quality_5"] == 8
    assert out[(cal[94], 1)]["fwd_5"] == pytest.approx(5 * math.log1p(.01))
    assert out[(cal[95], 1)]["fwd_5"] is None
    assert out[(cal[95], 1)]["quality_5"] == 6
    assert out[(cal[100], 1)]["quality_63"] == 6
    assert receipt["max_label_end"] == str(cutoff)
    assert L.LABEL_CUTOFF == before_cutoff == dt.date(2023, 12, 29)
    assert calls == [(freeze, True), (freeze, True)]
    manifest = C.read_json(root / "labels_holdout/manifest.json")
    assert manifest["status"] == "complete"
    assert manifest["freeze_sha256"] == C.sha256_file(freeze)
    assert manifest["quality_attrition"]["2024"]["h5"]["8"] > 0


def test_unauthorized_or_invalid_freeze_never_opens_data(monkeypatch, tmp_path):
    def no_read(*args, **kwargs):
        raise AssertionError("data read before authorization")
    monkeypatch.setattr(C, "connect", no_read)
    with pytest.raises(ValueError, match="explicit authorized"):
        H.build(tmp_path / "missing.json")
    def invalid(*args, **kwargs):
        raise ValueError("invalid freeze")
    monkeypatch.setattr(H, "verify_freeze", invalid)
    with pytest.raises(ValueError, match="invalid freeze"):
        H.build(tmp_path / "missing.json", authorized_oos=True)


def test_final_freeze_verification_failure_prevents_publication(holdout_lake, monkeypatch):
    root, _, _, _, freeze, _ = holdout_lake
    original = H.verify_freeze
    calls = 0
    def verify(*args):
        nonlocal calls
        calls += 1
        if calls == 2:
            raise ValueError("frozen source changed")
        return original(*args)
    monkeypatch.setattr(H, "verify_freeze", verify)
    with pytest.raises(ValueError, match="frozen source changed"):
        H.build(freeze, authorized_oos=True, memory="160MB")
    assert C.read_json(root / "labels_holdout/manifest.json")["status"] == "building"


def test_source_or_freeze_change_refuses_existing_stage(holdout_lake):
    _root, _, _, _, freeze, _ = holdout_lake
    H.build(freeze, authorized_oos=True, memory="160MB")
    freeze.write_text("changed fixture freeze")
    with pytest.raises(ValueError, match="freeze/source changed"):
        H.build(freeze, authorized_oos=True, memory="160MB")


def test_scan_refuses_future_dates_without_touching_input():
    con = duckdb.connect()
    try:
        with pytest.raises(ValueError, match="exceeds authorized window"):
            H.scan_holdout(con, [], dt.date(2024, 1, 1), dt.date(2025, 1, 1), dt.date(2024, 12, 31))
    finally:
        con.close()
