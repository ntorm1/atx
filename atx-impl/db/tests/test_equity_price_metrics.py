"""Tests for the derived ``equity_price_metrics`` dataset (S14).

The engine splits into a pure, DB-free transform (``compute_equity_price_metrics``) that
maps daily bars to typed price metric rows (returns, gap, realized vol, momentum,
distance-from-high, dollar volume), and a thin DuckDB materializer
(``refresh_equity_price_metrics`` / ``EquityPriceMetricsDataset``) that feeds it the
cached bars and writes the result.

No network: metrics derive purely from already-cached warehouse tables.
"""
from __future__ import annotations

import datetime as dt

import numpy as np
import pandas as pd
import pytest

from db.equity_price_metrics import (
    EquityPriceMetricsDataset,
    EquityPriceMetricsOptions,
    compute_equity_price_metrics,
    equity_price_metrics_asof,
    refresh_equity_price_metrics,
)


def _ts(s: str) -> pd.Timestamp:
    return pd.Timestamp(s)


def _bar(security_id, symbol, date, close, *, split_factor=1.0, open_=None, volume=1000, av="2013-01-02"):
    return {
        "security_id": security_id,
        "symbol": symbol,
        "trade_date": pd.Timestamp(date),
        "open": open_ if open_ is not None else close,
        "close": close,
        "split_factor": split_factor,
        "volume": volume,
        "available_at": _ts(av),
    }


def _series(security_id, symbol, closes, *, start=dt.date(2013, 1, 2), splits=None):
    rows = []
    for i, c in enumerate(closes):
        d = start + dt.timedelta(days=i)
        rows.append(_bar(security_id, symbol, d, c, split_factor=(splits[i] if splits else 1.0)))
    return rows


class TestComputeEquityPriceMetrics:
    def test_daily_and_log_return(self):
        out = compute_equity_price_metrics(pd.DataFrame(_series("S1", "AAA", [100.0, 110.0, 99.0])))
        by = {r.trade_date: r for r in out.itertuples(index=False)}
        d2 = by[dt.date(2013, 1, 3)]
        assert pd.isna(by[dt.date(2013, 1, 2)].daily_return)
        assert d2.daily_return == pytest.approx(0.10)
        assert d2.log_return == pytest.approx(np.log(110.0 / 100.0))

    def test_dollar_volume_and_gap(self):
        rows = [
            _bar("S1", "AAA", dt.date(2013, 1, 2), 100.0, volume=2000),
            _bar("S1", "AAA", dt.date(2013, 1, 3), 102.0, open_=101.0, volume=3000),
        ]
        out = compute_equity_price_metrics(pd.DataFrame(rows))
        by = {r.trade_date: r for r in out.itertuples(index=False)}
        assert by[dt.date(2013, 1, 2)].dollar_volume == pytest.approx(100.0 * 2000)
        # gap = open(101) / prior close(100) - 1
        assert by[dt.date(2013, 1, 3)].gap_return == pytest.approx(101.0 / 100.0 - 1.0)

    def test_pct_from_trailing_high_is_non_positive(self):
        closes = [100.0, 120.0, 90.0]  # adjusted == close (no splits)
        out = compute_equity_price_metrics(pd.DataFrame(_series("S1", "AAA", closes)))
        by = {r.trade_date: r for r in out.itertuples(index=False)}
        # day 3: adjusted close 90 vs trailing adjusted high max(100,120,90)=120 -> 90/120 - 1
        assert by[dt.date(2013, 1, 4)].pct_from_high_252d == pytest.approx(90.0 / 120.0 - 1.0)
        assert by[dt.date(2013, 1, 4)].pct_from_high_252d <= 0

    def test_momentum_21d(self):
        closes = [100.0 + i for i in range(25)]  # 25 bars
        out = compute_equity_price_metrics(pd.DataFrame(_series("S1", "AAA", closes))).sort_values("trade_date").reset_index(drop=True)
        # at index 21: adj/adj[0] - 1
        assert out.iloc[21]["momentum_21d"] == pytest.approx(closes[21] / closes[0] - 1.0)
        assert pd.isna(out.iloc[20]["momentum_21d"])  # not enough lookback

    def test_realized_vol_respects_min_periods(self):
        rng = [100.0]
        for i in range(1, 30):
            rng.append(rng[-1] * (1.0 + (0.01 if i % 2 else -0.008)))  # alternating moves
        out = compute_equity_price_metrics(pd.DataFrame(_series("S1", "AAA", rng))).sort_values("trade_date").reset_index(drop=True)
        assert pd.isna(out.iloc[19]["realized_vol_20d"])  # < 20 returns
        assert np.isfinite(out.iloc[29]["realized_vol_20d"])
        assert out.iloc[29]["realized_vol_20d"] > 0

    def test_returns_are_split_adjusted(self):
        # 2:1 split between day 2 and day 3: raw close halves (100 -> 51) but the real
        # move is small. split_factor=0.5 on the split day back-adjusts the pre-split
        # price so the return reflects the true move, not the split.
        rows = [
            _bar("S1", "AAA", dt.date(2013, 1, 2), 100.0),
            _bar("S1", "AAA", dt.date(2013, 1, 3), 51.0, split_factor=0.5),
        ]
        out = compute_equity_price_metrics(pd.DataFrame(rows))
        by = {r.trade_date: r for r in out.itertuples(index=False)}
        # adjusted: day1 = 100*0.5 = 50, day2 = 51 -> return = 51/50 - 1 = +2%
        assert by[dt.date(2013, 1, 2)].adjusted_close == pytest.approx(50.0)
        assert by[dt.date(2013, 1, 3)].daily_return == pytest.approx(51.0 / 50.0 - 1.0)
        assert abs(by[dt.date(2013, 1, 3)].daily_return) < 0.10  # not the -49% raw artifact

    def test_per_security_isolation(self):
        rows = _series("S1", "AAA", [100.0, 110.0]) + _series("S2", "BBB", [50.0, 40.0])
        out = compute_equity_price_metrics(pd.DataFrame(rows))
        by = {(r.security_id, r.trade_date): r for r in out.itertuples(index=False)}
        assert by[("S1", dt.date(2013, 1, 3))].daily_return == pytest.approx(0.10)
        assert by[("S2", dt.date(2013, 1, 3))].daily_return == pytest.approx(-0.20)

    def test_metric_id_deterministic_and_unique(self):
        rows = pd.DataFrame(_series("S1", "AAA", [100.0, 110.0]) + _series("S2", "BBB", [50.0, 40.0]))
        a = compute_equity_price_metrics(rows)
        b = compute_equity_price_metrics(rows)
        assert list(a["metric_id"]) == list(b["metric_id"])
        assert a["metric_id"].is_unique

    def test_empty_returns_typed_empty(self):
        out = compute_equity_price_metrics(pd.DataFrame())
        assert out.empty
        assert "realized_vol_20d" in out.columns


# --------------------------------------------------------------------------- #
# Integration: cached bars -> price metrics
# --------------------------------------------------------------------------- #


def _insert_bar(store, *, security_id, symbol, date, close, adj, volume, av, open_=None, high=None):
    store.con.execute(
        """
        INSERT INTO equity_daily_bars (
            source, security_id, symbol, trade_date, open, high, low, close,
            adjusted_close, volume, is_adjusted, available_at, source_loaded_at
        ) VALUES ('test', ?, ?, ?, ?, ?, ?, ?, ?, ?, true, ?, ?)
        """,
        [security_id, symbol, date, open_ if open_ is not None else close,
         high if high is not None else close, close, close, adj, volume, av, av],
    )


class TestRefreshIntegration:
    def test_materializes_returns_from_bars(self, tmp_store):
        av = dt.datetime(2013, 1, 5, 22, 0)
        for i, (c, a) in enumerate([(100.0, 100.0), (110.0, 110.0), (99.0, 99.0)]):
            _insert_bar(tmp_store, security_id="S1", symbol="AAA",
                        date=dt.date(2013, 1, 2) + dt.timedelta(days=i), close=c, adj=a, volume=1000, av=av)
        n = refresh_equity_price_metrics(tmp_store, EquityPriceMetricsOptions())
        assert n == 3
        df = tmp_store.con.execute(
            "SELECT trade_date, daily_return, dollar_volume FROM equity_price_metrics ORDER BY trade_date"
        ).df()
        assert df.iloc[1]["daily_return"] == pytest.approx(0.10)
        assert df.iloc[0]["dollar_volume"] == pytest.approx(100000.0)

    def test_dataset_run_is_idempotent(self, tmp_store):
        av = dt.datetime(2013, 1, 5, 22, 0)
        _insert_bar(tmp_store, security_id="S1", symbol="AAA", date=dt.date(2013, 1, 2), close=100.0, adj=100.0, volume=1000, av=av)
        ds = EquityPriceMetricsDataset()
        r1 = ds.run(tmp_store, EquityPriceMetricsOptions())
        n1 = tmp_store.con.execute("SELECT count(*) FROM equity_price_metrics").fetchone()[0]
        r2 = ds.run(tmp_store, EquityPriceMetricsOptions())
        n2 = tmp_store.con.execute("SELECT count(*) FROM equity_price_metrics").fetchone()[0]
        assert r1.rows_loaded == r2.rows_loaded
        assert n1 == n2 == 1


class TestAsofReader:
    def test_filters_by_available_at(self, tmp_store):
        _insert_bar(tmp_store, security_id="S1", symbol="AAA", date=dt.date(2013, 1, 2),
                    close=100.0, adj=100.0, volume=1000, av=dt.datetime(2013, 1, 2, 22, 0))
        EquityPriceMetricsDataset().run(tmp_store, EquityPriceMetricsOptions())
        early = equity_price_metrics_asof(dt.date(2013, 1, 1), store=tmp_store, symbols=["AAA"])
        assert early.empty
        late = equity_price_metrics_asof(dt.date(2013, 1, 31), store=tmp_store, symbols=["AAA"])
        assert not late.empty
        assert set(late["symbol"]) == {"AAA"}
