"""Stage ``options`` (S8.2): free ATM term structure from the prices stage, licensed merge, clocks, coverage."""

from __future__ import annotations

import datetime as dt
import json
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import options as OP
from atx_db.licensed import options as LO
from atx_db.licensed.mock import MockUniverse


def _fixture_root(root: Path, u: MockUniverse) -> None:
    days = u.weekdays(dt.date(2024, 6, 3), dt.date(2024, 6, 28))
    rows = {"session_date": [], "security_id": [], "ticker": [], "iv_atm_21d": [], "iv_atm_63d": [],
            "iv_atm_126d": [], "iv_atm_252d": [], "close": []}
    panel = {"session_date": [], "security_id": [], "member_equity": []}
    for d in days:
        for i, ln in enumerate(u.lines):
            if not ln.alive(d):
                continue
            has_iv = i % 5 != 4  # lines 4, 9, 14: no listed options
            rows["session_date"].append(d)
            rows["security_id"].append(ln.security_id)
            rows["ticker"].append(ln.ticker_on(d))
            rows["iv_atm_21d"].append(0.3 if has_iv else None)
            rows["iv_atm_63d"].append(0.32 if has_iv else None)
            rows["iv_atm_126d"].append(0.33 if has_iv else None)
            rows["iv_atm_252d"].append((0.35 if i != 5 else None) if has_iv else None)
            rows["close"].append(10.0)
            panel["session_date"].append(d)
            panel["security_id"].append(ln.security_id)
            panel["member_equity"].append(i != 7)
    (root / "prices" / "year=2024").mkdir(parents=True)
    pq.write_table(pa.table(rows), root / "prices" / "year=2024" / "prices.parquet")
    (root / "panel" / "year=2024").mkdir(parents=True)
    pq.write_table(pa.table(panel), root / "panel" / "year=2024" / "panel-06.parquet")
    (root / "prices" / "manifest.json").write_text('{"stage": "prices"}', encoding="utf-8")


@pytest.fixture
def root(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> tuple[Path, MockUniverse]:
    u = MockUniverse()
    r = tmp_path / "lake"
    _fixture_root(r, u)
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(r))
    return r, u


def test_free_stage_term_slopes_and_clock(root) -> None:
    r, u = root
    payload = OP.build()
    t = pq.read_table(r / "options" / "year=2024" / "options.parquet").to_pylist()
    assert t and all(x["atm_source"] == "tickerhistory3" and x["skew_source"] == "none" for x in t)
    x = next(x for x in t if x["security_id"] == u.lines[0].security_id and x["session_date"] == dt.date(2024, 6, 3))
    assert x["term_slope_63_21"] == pytest.approx(0.02) and x["term_slope_252_21"] == pytest.approx(0.05)
    assert x["atm_available_at"] == dt.datetime(2024, 6, 4, 3, 0) == x["available_at"]
    assert all(x[c] is None for c in OP.LICENSED_COLUMNS)
    assert not any(x["security_id"] == u.lines[4].security_id for x in t)  # no IV -> no row
    cov = payload["coverage"]["2024"]
    lines_member = [ln for i, ln in enumerate(u.lines) if i != 7 and ln.alive(dt.date(2024, 6, 3))]
    lines_opt = [ln for ln in lines_member if u.lines.index(ln) % 5 != 4]
    assert cov["member_lines"] == len(lines_member) and cov["optionable_lines"] == len(lines_opt)
    assert cov["term_slope_line_share"] == pytest.approx((len(lines_opt) - 1) / len(lines_opt), abs=1e-4)
    assert cov["skew_share"] == 0 and cov["volume_line_share"] == 0
    man = json.loads((r / "options" / "manifest.json").read_text(encoding="utf-8"))
    assert man["schema"] == OP.SCHEMA and man["status"] == "complete" and "prices/manifest.json" in man["input_manifests_sha256"]
    assert set(man["files"]) == {"year=2024/options.parquet"}


def test_licensed_merge(root, tmp_path: Path) -> None:
    r, u = root
    raw = tmp_path / "raw_opt"
    LO.ADAPTER.mock(raw, u)
    stage = tmp_path / "licensed_options"
    LO.ADAPTER.load(raw, identity=u.resolver(), build_time=dt.datetime(2026, 7, 2), out_dir=stage)
    payload = OP.build(licensed=stage)
    t = pq.read_table(r / "options" / "year=2024").to_pylist()
    x = next(x for x in t if x["security_id"] == u.lines[0].security_id and x["session_date"] == dt.date(2024, 6, 3))
    assert x["skew_source"] == "licensed:pit_archive" and x["skew_25d_30d"] > 0 and x["call_oi"] > 0
    assert x["available_at"] == max(x["atm_available_at"], x["lic_available_at"])
    # licensed rows for July and for lines without vendor IV are kept (FULL OUTER JOIN), ATM part NULL
    extra = [x for x in t if x["atm_source"] is None]
    assert extra and all(x["iv_atm_21d"] is None and x["skew_source"].startswith("licensed") for x in extra)
    cov = payload["coverage"]["2024"]
    assert cov["skew_line_share"] == 1.0 and cov["volume_line_share"] == 1.0 and cov["oi_line_share"] == 1.0
    assert cov["licensed_cells_without_vendor_iv"] > 0
    assert "licensed_options/manifest.json" in payload["input_manifests_sha256"]
