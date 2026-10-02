"""S3.4 market/shares_daily (rule shrout-pit-v1) on a fixture lake: SEC vs vendor choice, PIT clocks, splits."""

from __future__ import annotations

import datetime as dt

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

D = dt.date
T = dt.datetime


def sessions(start: dt.date, end: dt.date) -> list[dt.date]:
    out, d = [], start
    while d <= end:
        if d.weekday() < 5:
            out.append(d)
        d += dt.timedelta(days=1)
    return out


def write(path, rows: list[dict], schema: pa.Schema | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.Table.from_pylist(rows, schema=schema), path)


PRICE_SCHEMA = pa.schema([
    ("security_id", pa.int64()), ("session_date", pa.date32()), ("ticker", pa.string()), ("open", pa.float64()),
    ("high", pa.float64()), ("low", pa.float64()), ("close", pa.float64()), ("volume", pa.float64()),
    ("dollar_volume", pa.float64()), ("ret", pa.float64()), ("ret_guarded", pa.bool_()),
    ("return_factor", pa.float64()), ("fb_action", pa.string()), ("shares_vendor", pa.float64()),
    ("prev_raw_close", pa.float64()), ("gap_days", pa.int64())])

SPLIT_DAY = D(2018, 2, 15)


def vendor_shares(sid: int, d: dt.date) -> float:
    if sid == 16:
        base = 1_000_000.0 if d < D(2018, 1, 19) else 1_050_000.0
        return base * (2 if d >= SPLIT_DAY else 1)
    return {32: 3_000_000.0, 48: 500_000.0}[sid]


@pytest.fixture()
def lake(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    cal = sessions(D(2017, 12, 1), D(2018, 4, 30))
    write(tmp_path / "calendar.parquet", [{"session_date": d, "vendor_rows": 5000, "vendor_ids": 5000} for d in cal])
    rows = []
    for sid in (16, 32, 48):
        for d in cal:
            split = sid == 16 and d == SPLIT_DAY
            close = 50.0 if not (sid == 16 and d >= SPLIT_DAY) else 25.0
            rows.append({"security_id": sid, "session_date": d, "ticker": f"T{sid}", "open": close,
                         "high": close * 1.01, "low": close * 0.99, "close": close, "volume": 1000.0,
                         "dollar_volume": close * 1000, "ret": 0.0, "ret_guarded": False,
                         "return_factor": 0.5 if split else 1.0, "fb_action": "none",
                         "shares_vendor": vendor_shares(sid, d), "prev_raw_close": close, "gap_days": 1})
    write(tmp_path / "prices" / "year=2018" / "prices.parquet", rows, PRICE_SCHEMA)
    write(tmp_path / "corporate_actions" / "vendor_events.parquet",
          [{"security_id": 16, "ex_date": SPLIT_DAY, "kind": "split", "split_ratio": 2.0}])
    link = pa.schema([("security_id", pa.int64()), ("cik", pa.int64()), ("valid_from", pa.date32()),
                      ("valid_to", pa.date32()), ("link_tier", pa.string()), ("available_at", pa.timestamp("us")),
                      ("evidence_at", pa.timestamp("us"))])
    write(tmp_path / "identity" / "link_table.parquet", [
        {"security_id": 16, "cik": 100, "valid_from": cal[0], "valid_to": cal[-1], "link_tier": "strict",
         "available_at": T(2017, 1, 1), "evidence_at": T(2017, 1, 1)},
        {"security_id": 32, "cik": 200, "valid_from": cal[0], "valid_to": cal[-1], "link_tier": "strict",
         "available_at": T(2017, 1, 1), "evidence_at": T(2017, 1, 1)},
        {"security_id": 48, "cik": 200, "valid_from": cal[0], "valid_to": cal[-1], "link_tier": "name",
         "available_at": T(2017, 1, 1), "evidence_at": T(2017, 1, 1)}], link)
    ev = pa.schema([("cik", pa.int64()), ("clock_utc", pa.timestamp("us")), ("filed", pa.date32()),
                    ("shrs_q", pa.float64()), ("shrs_src", pa.string())])
    write(tmp_path / "fundamentals" / "events.parquet", [
        {"cik": 100, "clock_utc": T(2017, 11, 1, 21), "filed": D(2017, 11, 1), "shrs_q": 1_000_000.0, "shrs_src": "dei"},
        {"cik": 100, "clock_utc": T(2018, 1, 25, 21), "filed": D(2018, 1, 25), "shrs_q": 1_050_000.0, "shrs_src": "dei"},
        {"cik": 200, "clock_utc": T(2017, 11, 2, 21), "filed": D(2017, 11, 2), "shrs_q": 3_500_000.0,
         "shrs_src": "cls_cso"}], ev)
    write(tmp_path / "security_master" / "finra_names.parquet",
          [{"security_id": s, "finra_type": "common_unverified"} for s in (16, 32, 48)])
    return tmp_path


def run_bucket(lake):
    import importlib

    from atx_db.alpha_panel import common, market_common, shares_daily
    importlib.reload(common)
    importlib.reload(market_common)
    importlib.reload(shares_daily)
    shares_daily.build(buckets=[0])
    t = pq.read_table(lake / "_tmp" / "mkt" / "shares" / "bucket=00.parquet").to_pylist()
    return {(r["security_id"], r["session_date"]): r for r in t}


def test_single_line_issuer_uses_sec_count_at_its_clock(lake):
    rows = run_bucket(lake)
    r = rows[(16, D(2018, 1, 25))]     # filing accepted 21:00 UTC that day: not yet visible
    assert r["shrout"] == pytest.approx(1_000_000.0) and r["shrout_source"] == "sec_dei"
    assert r["shrout_asof"] == D(2017, 11, 1) and r["issuer_equity_lines"] == 1
    r = rows[(16, D(2018, 1, 26))]
    assert r["shrout"] == pytest.approx(1_050_000.0) and r["obs_available_at"] == T(2018, 1, 25, 21)
    # the vendor changed on the cover date 2018-01-19; it matches that filing, so its clock is the filing's
    assert r["shares_vendor_matched"] is True and r["shares_vendor_asof"] == D(2018, 1, 19)
    # the vendor's first value (2017-12-01) has no filing within 3 days before it: unmatched, visible 120 days on
    assert rows[(16, D(2018, 1, 22))]["shares_vendor_pit"] is None
    assert rows[(16, D(2018, 1, 22))]["shares_vendor_current"] == pytest.approx(1_050_000.0)


def test_split_adjusts_between_observations_and_clocks_the_ex_date(lake):
    rows = run_bucket(lake)
    before, on = rows[(16, D(2018, 2, 14))], rows[(16, SPLIT_DAY)]
    assert before["shrout"] == pytest.approx(1_050_000.0)
    assert on["shrout"] == pytest.approx(2_100_000.0) and on["split_adj"] == pytest.approx(2.0)
    assert on["available_at"] == T(2018, 2, 15, 22) and before["available_at"] == T(2018, 1, 25, 21)


def test_multi_class_issuer_falls_back_to_vendor_per_line(lake):
    rows = run_bucket(lake)
    r32, r48 = rows[(32, D(2018, 3, 1))], rows[(48, D(2018, 3, 1))]
    assert r32["issuer_equity_lines"] == 2 and r32["shares_sec"] == pytest.approx(3_500_000.0)
    # the vendor's first value (2017-12-01) is unmatched: visible after 2018-03-31 22:00 UTC
    assert r32["shrout"] is None and rows[(32, D(2018, 4, 2))]["shrout"] is None
    r32 = rows[(32, D(2018, 4, 3))]
    assert r32["shrout"] == pytest.approx(3_000_000.0) and r32["shrout_source"] == "vendor_lag"
    assert r32["obs_available_at"] == T(2018, 3, 31, 22)
    assert rows[(48, D(2018, 4, 3))]["shrout"] == pytest.approx(500_000.0)


@pytest.mark.parametrize("change", ["waso", "ADR"])
def test_issuer_totals_and_adrs_never_become_line_shrout(lake, change):
    ev = pq.read_table(lake / "fundamentals" / "events.parquet").to_pylist()
    if change == "waso":
        for r in ev:
            r["shrs_src"] = "waso"
        pq.write_table(pa.Table.from_pylist(ev, schema=pq.read_schema(lake / "fundamentals" / "events.parquet")),
                       lake / "fundamentals" / "events.parquet")
    else:
        write(lake / "security_master" / "finra_names.parquet",
              [{"security_id": 16, "finra_type": "ADR"}, {"security_id": 32, "finra_type": "common_unverified"},
               {"security_id": 48, "finra_type": "common_unverified"}])
    rows = run_bucket(lake)
    r = rows[(16, D(2018, 1, 26))]
    assert r["shares_sec"] == pytest.approx(1_050_000.0) and r["shares_sec_line_level"] is False
    assert r["shrout_source"] == "vendor_sec_matched" and r["shrout"] == pytest.approx(1_050_000.0)
