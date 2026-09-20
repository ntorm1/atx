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

from atx_db.equity_price_metrics import (
    EquityPriceMetricsDataset,
    EquityPriceMetricsOptions,
    compute_equity_price_metrics,
    equity_price_metrics_asof,
    refresh_equity_price_metrics,
)


def _ts(s: str) -> pd.Timestamp:
    return pd.Timestamp(s)


def _bar(security_id, symbol, date, close, *, split_factor=1.0, adjusted_close=None, open_=None, volume=1000, av="2013-01-02"):
    return {
        "security_id": security_id,
        "symbol": symbol,
        "trade_date": pd.Timestamp(date),
        "open": open_ if open_ is not None else close,
        "close": close,
        "split_factor": split_factor,
        "adjusted_close": close if adjusted_close is None else adjusted_close,
        "volume": volume,
        "available_at": _ts(av),
    }


def _series(security_id, symbol, closes, *, start=dt.date(2013, 1, 2), splits=None):
    rows = []
    for i, c in enumerate(closes):
        d = start + dt.timedelta(days=i)
        rows.append(_bar(security_id, symbol, d, c, split_factor=(splits[i] if splits else 1.0)))
    return rows


def _prices_from_returns(returns, *, start=100.0):
    prices = [float(start)]
    for ret in returns:
        prices.append(prices[-1] * (1.0 + float(ret)))
    return prices


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
        # move is small. Canonical adjusted close carries the correction; the
        # split_factor must not be applied again.
        rows = [
            _bar("S1", "AAA", dt.date(2013, 1, 2), 100.0, adjusted_close=50.0),
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

    def test_bounded_writer_matches_pure_transform_and_asof_cutoff(self, tmp_store):
        """The SQL writer keeps full peers/clocks while avoiding a pandas universe load."""
        start = dt.date(2020, 1, 2)
        bars = []
        for security_id, symbol, step in (("S1", "AAA", 0.004), ("S2", "BBB", 0.004), ("S3", "CCC", -0.002)):
            prices = _prices_from_returns([step if day % 2 else -step / 2 for day in range(260)])
            for day, price in enumerate(prices):
                date = start + dt.timedelta(days=day)
                # S1's raw split-like price move is corrected only by adjusted_close.
                close = price / 2 if security_id == "S1" and day >= 70 else price
                # This early high falls outside the trailing-252 window near cutoff.
                adjusted = 300.0 if day == 0 else price
                available = dt.datetime.combine(date, dt.time(22))
                if security_id == "S3" and day == 80:
                    available += dt.timedelta(days=3)
                if security_id == "S3" and day == 100:
                    available = dt.datetime.combine(start + dt.timedelta(days=256), dt.time(22))
                bars.append(_bar(security_id, symbol, date, close, adjusted_close=adjusted,
                                 volume=1_000 + 100 * day, av=available))
                _insert_bar(tmp_store, security_id=security_id, symbol=symbol, date=date,
                            close=close, adj=adjusted, volume=1_000 + 100 * day, av=available)

        cutoff = start + dt.timedelta(days=255)
        eligible = pd.DataFrame(bars)
        eligible = eligible[(eligible.trade_date.dt.date <= cutoff) & (
            eligible.available_at <= pd.Timestamp(dt.datetime.combine(cutoff, dt.time.max))
        )]
        expected = compute_equity_price_metrics(eligible, run_id="sql-parity")
        expected = expected.sort_values(["security_id", "trade_date"]).reset_index(drop=True)
        rows = refresh_equity_price_metrics(tmp_store, EquityPriceMetricsOptions(
            as_of_date=cutoff, run_id="sql-parity",
        ))
        actual = tmp_store.con.execute("""
            SELECT metric_id, source, security_id, symbol, trade_date, close, adjusted_close,
                   volume, dollar_volume, daily_return, log_return, gap_return,
                   realized_vol_20d, realized_vol_60d, momentum_21d, momentum_126d,
                   pct_from_high_252d, avg_dollar_volume_21d, amihud_illiquidity_21d,
                   max_drawdown_126d, downside_deviation_60d, market_return_ew, beta_60d,
                   market_correlation_60d, idiosyncratic_vol_60d,
                   daily_return_cs_pct_rank, momentum_21d_cs_pct_rank,
                   realized_vol_20d_cs_pct_rank, dollar_volume_cs_pct_rank,
                   amihud_illiquidity_21d_cs_pct_rank, is_latest_revision, as_of_date,
                   available_at, run_id
            FROM equity_price_metrics ORDER BY security_id, trade_date
        """).df()
        assert rows == len(expected)
        # DuckDB returns DATE as midnight datetime64 while the pure transform
        # intentionally exposes Python dates. Compare the same date semantics.
        for frame in (actual, expected):
            for column in ("trade_date", "as_of_date"):
                frame[column] = pd.to_datetime(frame[column]).dt.normalize()
        # Residual variance is a subtraction of nearly equal rolling moments.  Compare
        # its squared annualized volatility so DuckDB's ~1.5e-9 sqrt-roundoff is held
        # to a 3e-18 variance tolerance, while non-zero risk remains at 1e-10 relative.
        actual_idio = actual.pop("idiosyncratic_vol_60d")
        expected_idio = expected.pop("idiosyncratic_vol_60d")
        pd.testing.assert_series_equal(
            actual_idio.pow(2), expected_idio.pow(2), check_dtype=False,
            rtol=1e-10, atol=3e-18,
        )
        pd.testing.assert_frame_equal(actual, expected, check_dtype=False, check_like=False,
                                      rtol=1e-10, atol=1e-12)
        # The delayed peer clock remains part of the market proxy's availability.
        delayed_date = start + dt.timedelta(days=80)
        for date in (delayed_date, delayed_date + dt.timedelta(days=1)):
            assert actual[(actual.security_id == "S1") & (actual.trade_date == pd.Timestamp(date))].iloc[0].available_at == pd.Timestamp("2020-03-25 22:00:00")

    def test_empty_scope_replaces_source_and_failed_publish_preserves_previous_rows(self, tmp_store, monkeypatch):
        av = dt.datetime(2020, 1, 2, 22)
        _insert_bar(tmp_store, security_id="S1", symbol="AAA", date=dt.date(2020, 1, 2),
                    close=100, adj=100, volume=1000, av=av)
        refresh_equity_price_metrics(tmp_store, EquityPriceMetricsOptions())
        assert refresh_equity_price_metrics(tmp_store, EquityPriceMetricsOptions(symbols=())) == 0
        assert tmp_store.con.execute("SELECT count(*) FROM equity_price_metrics").fetchone()[0] == 0

        refresh_equity_price_metrics(tmp_store, EquityPriceMetricsOptions())
        # A retained foreign row is valid before a shadow-build failure.  The live
        # source and foreign row must both survive because no swap has occurred.
        tmp_store.con.execute("""
            INSERT INTO equity_price_metrics (
                metric_id, source, security_id, trade_date, is_latest_revision,
                as_of_date, available_at
            ) VALUES ('foreign-metric-id', 'foreign-source', 'foreign', DATE '2020-01-02', true,
                      DATE '2020-01-02', TIMESTAMP '2020-01-02 22:00:00')
        """)

        def fail_shadow(*_args, **_kwargs):
            raise RuntimeError("injected shadow-build failure")

        monkeypatch.setattr("atx_db.equity_price_metrics._build_publication_shadow", fail_shadow)
        with pytest.raises(RuntimeError, match="injected shadow-build failure"):
            refresh_equity_price_metrics(tmp_store, EquityPriceMetricsOptions())
        assert tmp_store.con.execute(
            "SELECT count(*) FROM equity_price_metrics WHERE source = ?", ["derived_equity_price_metrics_v1"]
        ).fetchone()[0] == 1
        assert tmp_store.con.execute(
            "SELECT count(*) FROM equity_price_metrics WHERE metric_id = 'foreign-metric-id'"
        ).fetchone()[0] == 1


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


class TestLiquidityFactors:
    def test_avg_dollar_volume_and_zero_amihud_on_flat_series(self):
        # 25 flat bars: constant close=100, volume=1000 -> dollar_volume=100,000,
        # daily_return=0 -> Amihud (|ret|/dvol) = 0.
        bars = _series("L1", "LIQ", [100.0] * 25)
        for b in bars:
            b["volume"] = 1000
        out = compute_equity_price_metrics(pd.DataFrame(bars)).sort_values("trade_date")
        last = out.iloc[-1]
        assert last["avg_dollar_volume_21d"] == pytest.approx(100_000.0)
        assert last["amihud_illiquidity_21d"] == pytest.approx(0.0)

    def test_amihud_warmup_and_positive_on_moving_series(self):
        import numpy as np

        rng = [100.0]
        for i in range(1, 30):
            rng.append(rng[-1] * (1.03 if i % 2 else 0.97))  # alternating +-3% moves
        bars = _series("L2", "MOV", rng)
        for b in bars:
            b["volume"] = 2000
        out = compute_equity_price_metrics(pd.DataFrame(bars)).sort_values("trade_date").reset_index(drop=True)
        # Warmup: first 20 rows (need a full 21-day window) carry no value.
        assert out["amihud_illiquidity_21d"].iloc[:20].isna().all()
        assert out["avg_dollar_volume_21d"].iloc[:20].isna().all()
        last = out.iloc[-1]
        assert last["amihud_illiquidity_21d"] > 0
        assert np.isfinite(last["amihud_illiquidity_21d"])
        # Independent recompute of the trailing-21 Amihud average at the last bar.
        close = out["close"].astype(float)
        ret = close.pct_change().abs()
        dvol = close * out["volume"].astype(float)
        daily = (ret / dvol).replace([np.inf, -np.inf], np.nan)
        expected = daily.rolling(21, min_periods=21).mean().iloc[-1] * 1e9
        assert last["amihud_illiquidity_21d"] == pytest.approx(expected, rel=1e-6)


class TestRiskFactors:
    def _ramp_series(self):
        # 200 flat bars at 100, then a linear decline to 80 over 60 bars.
        closes = [100.0] * 200 + list(np.linspace(99.5, 80.0, 60))
        return _series("R1", "RISK", closes)

    def test_max_drawdown_126d(self):
        out = compute_equity_price_metrics(pd.DataFrame(self._ramp_series())).sort_values("trade_date").reset_index(drop=True)
        # Expanding peak stays 100; deepest trailing-126 drawdown at the end = 80/100 - 1.
        assert out["max_drawdown_126d"].iloc[:125].isna().all()
        assert out["max_drawdown_126d"].iloc[-1] == pytest.approx(-0.20, abs=1e-6)

    def test_downside_deviation_60d_matches_independent_calc(self):
        out = compute_equity_price_metrics(pd.DataFrame(self._ramp_series())).sort_values("trade_date").reset_index(drop=True)
        assert out["downside_deviation_60d"].iloc[:59].isna().all()
        ret = out["close"].astype(float).pct_change()
        downside = ret.clip(upper=0.0)
        expected = float(np.sqrt((downside**2).rolling(60, min_periods=60).mean().iloc[-1]) * np.sqrt(252))
        assert out["downside_deviation_60d"].iloc[-1] == pytest.approx(expected, rel=1e-9)
        assert (out["downside_deviation_60d"].dropna() >= 0).all()


class TestMarketRelativeFactors:
    def _market_rows(self):
        market_returns = [0.001 if i % 2 else -0.0015 for i in range(70)]
        s1 = _prices_from_returns([2.0 * r for r in market_returns], start=100.0)
        s2 = _prices_from_returns([0.0 for _ in market_returns], start=50.0)
        return (
            _series("M1", "MKT1", s1, start=dt.date(2013, 1, 2))
            + _series("M2", "MKT2", s2, start=dt.date(2013, 1, 2))
        )

    def test_equal_weight_market_proxy_and_beta(self):
        out = compute_equity_price_metrics(pd.DataFrame(self._market_rows()))
        one = out[out["security_id"] == "M1"].sort_values("trade_date").reset_index(drop=True)
        two = out[out["security_id"] == "M2"].sort_values("trade_date").reset_index(drop=True)

        assert one["market_return_ew"].iloc[1] == pytest.approx(-0.0015)
        assert one["beta_60d"].iloc[:60].isna().all()
        assert one["beta_60d"].iloc[-1] == pytest.approx(2.0, rel=1e-9)
        assert one["market_correlation_60d"].iloc[-1] == pytest.approx(1.0, rel=1e-9)
        assert abs(one["idiosyncratic_vol_60d"].iloc[-1]) < 1e-8

        assert two["beta_60d"].iloc[-1] == pytest.approx(0.0, abs=1e-12)
        assert pd.isna(two["market_correlation_60d"].iloc[-1])
        assert two["idiosyncratic_vol_60d"].iloc[-1] == pytest.approx(0.0, abs=1e-12)

    def test_market_proxy_delays_available_at_to_latest_same_day_bar(self):
        rows = [
            _bar("M1", "MKT1", dt.date(2013, 1, 2), 100.0, av="2013-01-02 22:00:00"),
            _bar("M2", "MKT2", dt.date(2013, 1, 2), 50.0, av="2013-01-02 23:30:00"),
            _bar("M1", "MKT1", dt.date(2013, 1, 3), 102.0, av="2013-01-03 22:00:00"),
            _bar("M2", "MKT2", dt.date(2013, 1, 3), 49.0, av="2013-01-03 23:30:00"),
        ]
        out = compute_equity_price_metrics(pd.DataFrame(rows))
        row = out[(out["security_id"] == "M1") & (out["trade_date"] == dt.date(2013, 1, 3))].iloc[0]
        assert row["available_at"] == pd.Timestamp("2013-01-03 23:30:00")


class TestCrossSectionalRanks:
    def test_daily_return_and_dollar_volume_pct_ranks(self):
        rows = [
            _bar("R1", "LOW", dt.date(2013, 1, 2), 100.0, volume=1000),
            _bar("R2", "MID", dt.date(2013, 1, 2), 100.0, volume=1000),
            _bar("R3", "HIGH", dt.date(2013, 1, 2), 100.0, volume=1000),
            _bar("R1", "LOW", dt.date(2013, 1, 3), 90.0, volume=1000),
            _bar("R2", "MID", dt.date(2013, 1, 3), 100.0, volume=2000),
            _bar("R3", "HIGH", dt.date(2013, 1, 3), 110.0, volume=3000),
        ]
        out = compute_equity_price_metrics(pd.DataFrame(rows))
        day2 = out[out["trade_date"] == dt.date(2013, 1, 3)].set_index("symbol")

        assert day2.loc["LOW", "daily_return_cs_pct_rank"] == pytest.approx(1.0 / 3.0)
        assert day2.loc["MID", "daily_return_cs_pct_rank"] == pytest.approx(2.0 / 3.0)
        assert day2.loc["HIGH", "daily_return_cs_pct_rank"] == pytest.approx(1.0)
        assert day2.loc["LOW", "dollar_volume_cs_pct_rank"] == pytest.approx(1.0 / 3.0)
        assert day2.loc["MID", "dollar_volume_cs_pct_rank"] == pytest.approx(2.0 / 3.0)
        assert day2.loc["HIGH", "dollar_volume_cs_pct_rank"] == pytest.approx(1.0)
        assert out[out["trade_date"] == dt.date(2013, 1, 2)]["daily_return_cs_pct_rank"].isna().all()

    def test_momentum_pct_rank_after_warmup(self):
        rows = (
            _series("R1", "UP", [100.0 + i for i in range(25)])
            + _series("R2", "FLAT", [100.0] * 25)
            + _series("R3", "DOWN", [100.0 - i for i in range(25)])
        )
        out = compute_equity_price_metrics(pd.DataFrame(rows))
        last_date = out["trade_date"].max()
        last = out[out["trade_date"] == last_date].set_index("symbol")

        assert last.loc["DOWN", "momentum_21d_cs_pct_rank"] == pytest.approx(1.0 / 3.0)
        assert last.loc["FLAT", "momentum_21d_cs_pct_rank"] == pytest.approx(2.0 / 3.0)
        assert last.loc["UP", "momentum_21d_cs_pct_rank"] == pytest.approx(1.0)
