"""Options adapter (S8.2 license need): SpiderRock-layout mocks -> 25-delta skew, term slope, volume, OI."""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db.licensed import options as O
from atx_db.licensed.mock import UNKNOWN_TICKER, MockUniverse

BUILD = dt.datetime(2026, 7, 2)


@pytest.fixture(scope="module")
def run(tmp_path_factory: pytest.TempPathFactory):
    u = MockUniverse()
    raw = tmp_path_factory.mktemp("opt")
    O.ADAPTER.mock(raw, u)
    st = O.ADAPTER.load(raw, identity=u.resolver(), build_time=BUILD)
    return u, raw, st, st.tables["options_daily"].to_pylist()


def test_schema_and_validation(run) -> None:
    _, _, st, rows = run
    assert st.tables["options_daily"].schema == O.OPTIONS.schema and rows and not st.report.failures


def test_delta_grid_to_25_delta_and_slope(run) -> None:
    _, _, _, rows = run
    for r in rows:
        if r["iv_call_25d_30d"] is None:
            continue
        assert r["iv_put_25d_30d"] > r["iv_atm_30d"] * 0.99 and r["skew_25d_30d"] > 0
        assert r["skew_25d_30d"] == pytest.approx(r["iv_put_25d_30d"] - r["iv_call_25d_30d"])
        assert r["term_slope_1y_30d"] == pytest.approx(r["iv_atm_1y"] - r["iv_atm_30d"])
    assert sum(r["iv_call_25d_30d"] is not None for r in rows) == len(rows) - sum(r["ticker"] == UNKNOWN_TICKER for r in rows)


def test_clock_is_vendor_t0_delivery(run) -> None:
    _, _, _, rows = run
    r = next(r for r in rows if r["session_date"] == dt.date(2024, 6, 3))
    assert r["vendor_snapshot_at"] == dt.datetime(2024, 6, 3, 19, 55)
    assert r["available_at"] == dt.datetime(2024, 6, 4, 3, 0) and r["clock_basis"] == "publication_lag"
    # usable from the second following session: cutoff of 2024-06-04 is 2024-06-03 22:00 UTC
    assert not r["available_at"] < dt.datetime(2024, 6, 3, 22) and r["available_at"] < dt.datetime(2024, 6, 4, 22)


def test_native_ids_ticker_fallback_and_unmapped(run) -> None:
    u, _, st, rows = run
    tiers = st.report.stats["options_daily"]["link_tiers"]
    assert set(tiers) == {"vendor_native", "ticker_dated", "unmapped"}
    t = [r for r in rows if r["link_tier"] == "ticker_dated"]
    assert {r["security_id"] for r in t} == {u.lines[9].security_id}
    assert {r["ticker"] for r in rows if r["link_tier"] == "unmapped"} == {UNKNOWN_TICKER}


def test_volume_and_oi_by_call_put(run) -> None:
    _, _, _, rows = run
    assert all(r["call_volume"] >= 0 and r["put_volume"] >= 0 and r["call_oi"] >= 0 and r["put_oi"] >= 0 for r in rows)


def test_features_without_grid_keep_atm_and_volume(run, tmp_path) -> None:
    u, raw, _, _ = run
    src = raw / "OptionEODFeaturesHist_2024.parquet"
    (tmp_path / src.name).write_bytes(src.read_bytes())
    from atx_db.licensed import contract as K
    K.write_receipt(tmp_path, tmp_path / src.name, fetched_at=dt.datetime(2026, 7, 1, 6), history_mode="pit_archive")
    rows = O.ADAPTER.load(tmp_path, identity=u.resolver(), build_time=BUILD).tables["options_daily"].to_pylist()
    assert all(r["iv_call_25d_30d"] is None and r["iv_atm_30d"] is not None and r["call_volume"] is not None for r in rows)


def test_substitute_is_free_options_stage() -> None:
    sub = O.ADAPTER.substitute()
    assert sub.stage == "options" and sub.columns["term_slope_1y_30d"] == "term_slope_252_21"
