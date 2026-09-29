"""S4.3 FX conversion of non-USD filers (ruling D4): H.10 rates, balances at the period-end rate, flows at the
period-average rate, event available_at = max(filing clock, clock of the last rate used)."""

from __future__ import annotations

import datetime as dt
import math

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import fund_fx as ffx
from atx_db.alpha_panel import fund_items as fi

D = dt.date
T = dt.datetime


def _fx_rows(ccy, start, end, rate_fn, series="H10/TEST", lag_days=5):
    rows, d = [], start
    while d <= end:
        if d.weekday() < 5:
            rows.append({"obs_date": d, "currency": ccy, "usd_per_ccy": rate_fn(d), "series_id": series,
                         "available_at": T(d.year, d.month, d.day, 21) + dt.timedelta(days=lag_days)})
        d += dt.timedelta(days=1)
    return rows


def fx_table(tmp_path, rows):
    schema = pa.schema([("obs_date", pa.date32()), ("currency", pa.string()), ("usd_per_ccy", pa.float64()),
                        ("series_id", pa.string()), ("available_at", pa.timestamp("us", tz="UTC"))])
    path = tmp_path / "fx_daily.parquet"
    pq.write_table(pa.Table.from_pylist(rows, schema=schema), path)
    return ffx.FxTable.load(path)


def test_spot_is_last_obs_on_or_before_and_average_over_window(tmp_path):
    rows = _fx_rows("EUR", D(2023, 1, 2), D(2023, 12, 29), lambda d: 1.0 + d.month / 100)
    fx = fx_table(tmp_path, rows)
    assert fx.covers("EUR") and not fx.covers("TRY")
    r, avail = fx.spot("EUR", D(2023, 12, 31))            # Sunday: the Friday 2023-12-29 rate
    assert r == pytest.approx(1.12) and avail == T(2024, 1, 3, 21)
    assert fx.spot("EUR", D(2022, 12, 31)) is None         # before the series
    avg, avail = fx.average("EUR", D(2023, 10, 1), D(2023, 12, 31))
    assert avg == pytest.approx((1.10 * 22 + 1.11 * 22 + 1.12 * 21) / 65)
    assert avail == T(2024, 1, 3, 21)
    assert fx.average("EUR", D(2021, 1, 1), D(2021, 3, 31)) is None
    assert fx.spot("EUR", D(2024, 3, 31)) is None          # stale: last obs more than 10 days earlier


def _twd_issuer():
    rows = []
    for y, at, ni, sale in ((2021, 1000.0, 50.0, 600.0), (2022, 1100.0, 60.0, 700.0), (2023, 1200.0, 80.0, 800.0)):
        c, a, s, e = T(y + 1, 4, 10, 10, 0), f"fy{y}", D(y, 1, 1), D(y, 12, 31)
        rows += [(c, a, fi.CID["ifrs-full:Assets"], None, e, at, "TWD", "20-F", c.date(), y, "FY", "fsds_accepted_utc"),
                 (c, a, fi.CID["ifrs-full:ProfitLoss"], s, e, ni, "TWD", "20-F", c.date(), y, "FY", "fsds_accepted_utc"),
                 (c, a, fi.CID["ifrs-full:Revenue"], s, e, sale, "TWD", "20-F", c.date(), y, "FY", "fsds_accepted_utc"),
                 (c, a, fi.CID["EntityCommonStockSharesOutstanding"], None, D(y + 1, 3, 31), 1e9, "shares", "20-F",
                  c.date(), y, "FY", "fsds_accepted_utc")]
    return rows


def test_non_usd_events_converted(tmp_path):
    # TWD: 0.030 USD in 2021-2022, 0.033 in 2023 (a step on 2023-07-01)
    fx = fx_table(tmp_path, _fx_rows("TWD", D(2021, 1, 1), D(2024, 6, 30),
                                     lambda d: 0.033 if d >= D(2023, 7, 1) else 0.030))
    counters: dict[str, int] = {}
    ev = fi.issuer_events(7, _twd_issuer(), T(2022, 1, 1), counters, fx=fx)
    last = ev[-1]
    assert last["currency"] == "TWD" and last["fx_converted"] is True
    assert last["at"] == pytest.approx(1200.0 * 0.033)                    # balance: period-end rate
    assert last["fx_rate"] == pytest.approx(0.033)
    avg = last["fx_rate_avg_ttm"]
    assert 0.030 < avg < 0.033                                            # flow: 2023 average (half and half)
    assert last["ni_ttm"] == pytest.approx(80.0 * avg) and last["sale_ttm"] == pytest.approx(800.0 * avg)
    assert last["at_lag4"] == pytest.approx(1100.0 * 0.030)               # a lag balance at its own period-end rate
    assert last["shrs_q"] == 1e9                                          # shares are not converted
    assert last["available_at"] == last["clock_utc"]                      # the filing is later than every rate
    assert counters["events_fx_converted"] == len(ev)


def test_rate_clock_later_than_filing_moves_available_at(tmp_path):
    fx = fx_table(tmp_path, _fx_rows("TWD", D(2021, 1, 1), D(2024, 6, 30), lambda d: 0.03, lag_days=120))
    ev = fi.issuer_events(7, _twd_issuer(), T(2022, 1, 1), {}, fx=fx)
    last = ev[-1]
    assert last["available_at"] == T(2023, 12, 29, 21) + dt.timedelta(days=120)
    assert last["available_at"] > last["clock_utc"]


def test_uncovered_currency_keeps_v9_behaviour(tmp_path):
    fx = fx_table(tmp_path, _fx_rows("EUR", D(2021, 1, 1), D(2024, 6, 30), lambda d: 1.1))
    ev = fi.issuer_events(7, _twd_issuer(), T(2022, 1, 1), {}, fx=fx)
    last = ev[-1]
    assert last["fx_converted"] is False and last["at"] is None and last["fx_rate"] is None
    assert last["available_at"] == last["clock_utc"]
    assert last["fscore_n"] is not None or last["f_roa"] is not None          # unitless items still native


def test_usd_filer_untouched(tmp_path):
    fx = fx_table(tmp_path, _fx_rows("TWD", D(2021, 1, 1), D(2024, 6, 30), lambda d: 0.03))
    c, s, e = T(2024, 2, 20, 21), D(2023, 1, 1), D(2023, 12, 31)
    rows = [(c, "a", fi.CID["Assets"], None, e, 10.0, "USD", "10-K", c.date(), 2023, "FY", "fsds_accepted_utc"),
            (c, "a", fi.CID["NetIncomeLoss"], s, e, 1.0, "USD", "10-K", c.date(), 2023, "FY", "fsds_accepted_utc")]
    ev = fi.issuer_events(1, rows, T(2024, 1, 1), {}, fx=fx)
    assert ev[0]["at"] == 10.0 and ev[0]["fx_converted"] is False and ev[0]["fx_rate"] is None
    assert not math.isnan(ev[0]["ni_ttm"])
