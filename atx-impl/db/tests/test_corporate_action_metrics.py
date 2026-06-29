"""Tests for derived corporate-action metric datasets (S18/S19).

The engine splits into a pure, DB-free transform (``compute_dividend_metrics``) that maps
cash-dividend events (joined to the ex-date close) to typed metric rows (spot/TTM yield,
TTM dividend sum/count, YoY dividend growth), and a thin DuckDB materializer
(``refresh_dividend_metrics`` / ``CorporateActionDividendMetricsDataset``) that joins
``corporate_actions`` to ``equity_daily_bars`` and writes the result.

S19 adds the same split for ``compute_split_metrics`` / ``CorporateActionSplitMetricsDataset``:
one row per normalized split event, reconciled against adjacent rows in
``daily_adjustment_factors``.

No network: metrics derive purely from already-cached warehouse tables.
"""
from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest

from db.corporate_action_metrics import (
    CorporateActionDividendMetricsDataset,
    CorporateActionDividendMetricsOptions,
    CorporateActionSplitMetricsDataset,
    CorporateActionSplitMetricsOptions,
    compute_dividend_metrics,
    compute_split_metrics,
    corporate_action_dividend_metrics_asof,
    corporate_action_split_metrics_asof,
    refresh_dividend_metrics,
    refresh_split_metrics,
)


def _ts(s: str) -> pd.Timestamp:
    return pd.Timestamp(s)


def _div(security_id, symbol, ex_date, cash, close, av="2012-04-03"):
    return {
        "security_id": security_id,
        "symbol": symbol,
        "ex_date": pd.Timestamp(ex_date),
        "record_date": None,
        "payable_date": None,
        "cash_amount": cash,
        "close_on_ex": close,
        "available_at": _ts(av),
    }


def _quarterly(security_id, symbol, start, cash_list, close=100.0):
    rows = []
    d = pd.Timestamp(start)
    for i, cash in enumerate(cash_list):
        ex = d + pd.DateOffset(months=3 * i)
        rows.append(_div(security_id, symbol, ex.date(), cash, close, av=str(ex.date())))
    return rows


def _split_event(security_id="S1", symbol="AAA", *, post_close=50.0):
    return {
        "security_id": security_id,
        "symbol": symbol,
        "ex_date": pd.Timestamp("2024-01-10"),
        "event_ref_id": "split-event-1",
        "source_action_source": "test corporate actions",
        "classification_reason": "explicit_split_ratio",
        "factor_price": 0.5,
        "factor_shares": 2.0,
        "ratio_numerator": 2.0,
        "ratio_denominator": 1.0,
        "bar_source": "test",
        "pre_trade_date": pd.Timestamp("2024-01-09"),
        "post_trade_date": pd.Timestamp("2024-01-10"),
        "pre_raw_close": 100.0,
        "post_raw_close": post_close,
        "pre_split_adjusted_close": 50.0,
        "post_split_adjusted_close": post_close,
        "pre_split_price_factor": 0.5,
        "post_split_price_factor": 1.0,
        "pre_split_share_factor": 2.0,
        "post_split_share_factor": 1.0,
        "event_available_at": _ts("2024-01-10 22:00:00"),
        "pre_available_at": _ts("2024-01-11 22:00:00"),
        "post_available_at": _ts("2024-01-11 22:00:00"),
    }


class TestComputeDividendMetrics:
    def test_spot_yield_and_ordinal(self):
        out = compute_dividend_metrics(pd.DataFrame([
            _div("S1", "AAA", dt.date(2012, 4, 3), 0.50, 100.0),
        ]))
        r = out.iloc[0]
        assert r["dividend_yield_spot"] == pytest.approx(0.005)
        assert r["dividend_ordinal"] == 1
        assert r["ttm_dividend"] == pytest.approx(0.50)
        assert r["ttm_dividend_count"] == 1

    def test_ttm_dividend_sum_and_yield(self):
        out = compute_dividend_metrics(pd.DataFrame(_quarterly("S1", "AAA", "2012-04-03", [0.5, 0.5, 0.5, 0.5])))
        by = {r.ex_date: r for r in out.itertuples(index=False)}
        last = by[dt.date(2013, 1, 3)]
        assert last.ttm_dividend == pytest.approx(2.0)       # four trailing quarters
        assert last.ttm_dividend_count == 4
        assert last.dividend_yield_ttm == pytest.approx(2.0 / 100.0)
        # the first dividend only sees itself in its trailing window
        assert by[dt.date(2012, 4, 3)].ttm_dividend == pytest.approx(0.5)
        assert by[dt.date(2012, 4, 3)].ttm_dividend_count == 1

    def test_dividend_growth_yoy(self):
        # five quarterly dividends; the fifth (one year after the first) raised 0.5 -> 0.6
        rows = _quarterly("S1", "AAA", "2012-04-03", [0.5, 0.5, 0.5, 0.5, 0.6])
        out = compute_dividend_metrics(pd.DataFrame(rows))
        by = {r.ex_date: r for r in out.itertuples(index=False)}
        fifth = by[dt.date(2013, 4, 3)]
        assert fifth.dividend_growth_yoy == pytest.approx((0.6 - 0.5) / 0.5)
        # the first dividend has no ~1y-prior event -> NaN
        assert pd.isna(by[dt.date(2012, 4, 3)].dividend_growth_yoy)

    def test_yield_nan_when_close_missing(self):
        out = compute_dividend_metrics(pd.DataFrame([_div("S1", "AAA", dt.date(2012, 4, 3), 0.5, None)]))
        assert pd.isna(out.iloc[0]["dividend_yield_spot"])

    def test_per_security_isolation(self):
        rows = _quarterly("S1", "AAA", "2012-04-03", [0.5, 0.5]) + _quarterly("S2", "BBB", "2012-04-03", [1.0, 1.0])
        out = compute_dividend_metrics(pd.DataFrame(rows))
        by = {(r.security_id, r.ex_date): r for r in out.itertuples(index=False)}
        assert by[("S1", dt.date(2012, 7, 3))].ttm_dividend == pytest.approx(1.0)
        assert by[("S2", dt.date(2012, 7, 3))].ttm_dividend == pytest.approx(2.0)

    def test_metric_id_deterministic_and_unique(self):
        rows = pd.DataFrame(_quarterly("S1", "AAA", "2012-04-03", [0.5, 0.5]))
        a = compute_dividend_metrics(rows)
        b = compute_dividend_metrics(rows)
        assert list(a["metric_id"]) == list(b["metric_id"])
        assert a["metric_id"].is_unique

    def test_empty_returns_typed_empty(self):
        out = compute_dividend_metrics(pd.DataFrame())
        assert out.empty
        assert "dividend_yield_ttm" in out.columns


class TestComputeSplitMetrics:
    def test_reconciles_daily_factor_step_and_split_adjusted_return(self):
        out = compute_split_metrics(pd.DataFrame([_split_event()]))
        r = out.iloc[0]
        assert r["observed_factor_price_step"] == pytest.approx(0.5)
        assert r["observed_factor_share_step"] == pytest.approx(2.0)
        assert r["factor_price_error"] == pytest.approx(0.0)
        assert r["factor_share_error"] == pytest.approx(0.0)
        assert r["raw_close_return"] == pytest.approx(-0.5)
        assert r["split_adjusted_return"] == pytest.approx(0.0)
        assert r["reconciliation_status"] == "RECONCILED"
        assert bool(r["is_reconciled"]) is True

    def test_flags_factor_mismatch(self):
        row = _split_event()
        row["pre_split_price_factor"] = 0.49
        out = compute_split_metrics(pd.DataFrame([row]))
        r = out.iloc[0]
        assert r["reconciliation_status"] == "MISMATCH"
        assert bool(r["is_reconciled"]) is False

    def test_preserves_missing_daily_factor_status(self):
        row = _split_event()
        for col in (
            "pre_split_price_factor", "post_split_price_factor",
            "pre_split_share_factor", "post_split_share_factor",
        ):
            row[col] = None
        out = compute_split_metrics(pd.DataFrame([row]))
        r = out.iloc[0]
        assert r["reconciliation_status"] == "MISSING_DAILY_FACTOR"
        assert bool(r["is_reconciled"]) is False

    def test_split_empty_returns_typed_empty(self):
        out = compute_split_metrics(pd.DataFrame())
        assert out.empty
        assert "reconciliation_status" in out.columns


# --------------------------------------------------------------------------- #
# Integration: corporate_actions x equity_daily_bars -> dividend metrics
# --------------------------------------------------------------------------- #


def _insert_corp_div(store, *, security_id, symbol, ex_date, cash, av):
    store.con.execute(
        """
        INSERT INTO corporate_actions (
            source, security_id, symbol, action_type, ex_date, cash_amount, available_at, source_loaded_at
        ) VALUES ('test', ?, ?, 'cash_dividend_inferred', ?, ?, ?, ?)
        """,
        [security_id, symbol, ex_date, cash, av, av],
    )


def _insert_corp_split(store, *, security_id, symbol, ex_date, split_from, split_to, av):
    store.con.execute(
        """
        INSERT INTO corporate_actions (
            source, security_id, symbol, action_type, ex_date, split_from, split_to, available_at, source_loaded_at
        ) VALUES ('test corporate actions', ?, ?, 'stock_split', ?, ?, ?, ?, ?)
        """,
        [security_id, symbol, ex_date, split_from, split_to, av, av],
    )


def _insert_bar(store, *, security_id, symbol, trade_date, close, av):
    store.con.execute(
        """
        INSERT INTO equity_daily_bars (
            source, security_id, symbol, trade_date, close, is_adjusted, available_at, source_loaded_at
        ) VALUES ('test', ?, ?, ?, ?, true, ?, ?)
        """,
        [security_id, symbol, trade_date, close, av, av],
    )


def _seed_split_inputs(store):
    from db.adjustment_factors import refresh_adjustment_factor_history
    from db.daily_adjustments import DailyAdjustmentFactorOptions, refresh_daily_adjustment_factors

    _insert_bar(
        store,
        security_id="S1",
        symbol="AAA",
        trade_date=dt.date(2024, 1, 9),
        close=100.0,
        av=dt.datetime(2024, 1, 9, 22, 0),
    )
    _insert_bar(
        store,
        security_id="S1",
        symbol="AAA",
        trade_date=dt.date(2024, 1, 10),
        close=50.0,
        av=dt.datetime(2024, 1, 10, 22, 0),
    )
    _insert_corp_split(
        store,
        security_id="S1",
        symbol="AAA",
        ex_date=dt.date(2024, 1, 10),
        split_from=1.0,
        split_to=2.0,
        av=dt.datetime(2024, 1, 10, 22, 0),
    )
    refresh_adjustment_factor_history(store)
    refresh_daily_adjustment_factors(
        store,
        DailyAdjustmentFactorOptions(as_of_date=dt.date(2024, 1, 11)),
    )


class TestRefreshIntegration:
    def test_materializes_yield_from_join(self, tmp_store):
        av = dt.datetime(2012, 4, 3, 22, 0)
        _insert_corp_div(tmp_store, security_id="S1", symbol="AAA", ex_date=dt.date(2012, 4, 3), cash=0.5, av=av)
        _insert_bar(tmp_store, security_id="S1", symbol="AAA", trade_date=dt.date(2012, 4, 3), close=100.0, av=av)
        n = refresh_dividend_metrics(tmp_store, CorporateActionDividendMetricsOptions())
        assert n == 1
        r = tmp_store.con.execute(
            "SELECT close_on_ex, dividend_yield_spot FROM corporate_action_dividend_metrics"
        ).df().iloc[0]
        assert r["close_on_ex"] == pytest.approx(100.0)
        assert r["dividend_yield_spot"] == pytest.approx(0.005)

    def test_split_artifact_excluded(self, tmp_store):
        # the upstream feed misinfers a 2:1 split as a ~50%-of-price "cash dividend";
        # it must be dropped so it never pollutes the TTM dividend sum.
        av = dt.datetime(2012, 8, 13, 22, 0)
        _insert_corp_div(tmp_store, security_id="S1", symbol="AAA", ex_date=dt.date(2012, 4, 3), cash=0.5, av=dt.datetime(2012, 4, 3, 22, 0))
        _insert_bar(tmp_store, security_id="S1", symbol="AAA", trade_date=dt.date(2012, 4, 3), close=100.0, av=dt.datetime(2012, 4, 3, 22, 0))
        _insert_corp_div(tmp_store, security_id="S1", symbol="AAA", ex_date=dt.date(2012, 8, 13), cash=40.0, av=av)  # split artifact
        _insert_bar(tmp_store, security_id="S1", symbol="AAA", trade_date=dt.date(2012, 8, 13), close=80.0, av=av)
        refresh_dividend_metrics(tmp_store, CorporateActionDividendMetricsOptions())
        df = tmp_store.con.execute(
            "SELECT ex_date FROM corporate_action_dividend_metrics ORDER BY ex_date"
        ).df()
        assert len(df) == 1  # only the real $0.50 dividend survives; the $40 split artifact is dropped
        assert pd.Timestamp(df.iloc[0]["ex_date"]) == pd.Timestamp("2012-04-03")

    def test_dataset_run_is_idempotent(self, tmp_store):
        av = dt.datetime(2012, 4, 3, 22, 0)
        _insert_corp_div(tmp_store, security_id="S1", symbol="AAA", ex_date=dt.date(2012, 4, 3), cash=0.5, av=av)
        _insert_bar(tmp_store, security_id="S1", symbol="AAA", trade_date=dt.date(2012, 4, 3), close=100.0, av=av)
        ds = CorporateActionDividendMetricsDataset()
        r1 = ds.run(tmp_store, CorporateActionDividendMetricsOptions())
        n1 = tmp_store.con.execute("SELECT count(*) FROM corporate_action_dividend_metrics").fetchone()[0]
        r2 = ds.run(tmp_store, CorporateActionDividendMetricsOptions())
        n2 = tmp_store.con.execute("SELECT count(*) FROM corporate_action_dividend_metrics").fetchone()[0]
        assert r1.rows_loaded == r2.rows_loaded
        assert n1 == n2 == 1

    def test_materializes_split_reconciliation_from_daily_adjustments(self, tmp_store):
        _seed_split_inputs(tmp_store)
        n = refresh_split_metrics(tmp_store, CorporateActionSplitMetricsOptions())
        assert n == 1
        r = tmp_store.con.execute(
            """
            SELECT
                pre_trade_date,
                post_trade_date,
                raw_close_return,
                split_adjusted_return,
                observed_factor_price_step,
                observed_factor_share_step,
                reconciliation_status,
                is_reconciled
            FROM corporate_action_split_metrics
            """
        ).df().iloc[0]
        assert pd.Timestamp(r["pre_trade_date"]) == pd.Timestamp("2024-01-09")
        assert pd.Timestamp(r["post_trade_date"]) == pd.Timestamp("2024-01-10")
        assert r["raw_close_return"] == pytest.approx(-0.5)
        assert r["split_adjusted_return"] == pytest.approx(0.0)
        assert r["observed_factor_price_step"] == pytest.approx(0.5)
        assert r["observed_factor_share_step"] == pytest.approx(2.0)
        assert r["reconciliation_status"] == "RECONCILED"
        assert bool(r["is_reconciled"]) is True

    def test_split_dataset_run_is_idempotent(self, tmp_store):
        _seed_split_inputs(tmp_store)
        ds = CorporateActionSplitMetricsDataset()
        r1 = ds.run(tmp_store, CorporateActionSplitMetricsOptions())
        n1 = tmp_store.con.execute("SELECT count(*) FROM corporate_action_split_metrics").fetchone()[0]
        r2 = ds.run(tmp_store, CorporateActionSplitMetricsOptions())
        n2 = tmp_store.con.execute("SELECT count(*) FROM corporate_action_split_metrics").fetchone()[0]
        assert r1.rows_loaded == r2.rows_loaded
        assert n1 == n2 == 1


class TestAsofReader:
    def test_filters_by_available_at(self, tmp_store):
        av = dt.datetime(2012, 4, 13, 22, 0)
        _insert_corp_div(tmp_store, security_id="S1", symbol="AAA", ex_date=dt.date(2012, 4, 3), cash=0.5, av=av)
        _insert_bar(tmp_store, security_id="S1", symbol="AAA", trade_date=dt.date(2012, 4, 3), close=100.0, av=av)
        CorporateActionDividendMetricsDataset().run(tmp_store, CorporateActionDividendMetricsOptions())
        early = corporate_action_dividend_metrics_asof(dt.date(2012, 4, 5), store=tmp_store, symbols=["AAA"])
        assert early.empty
        late = corporate_action_dividend_metrics_asof(dt.date(2012, 5, 1), store=tmp_store, symbols=["AAA"])
        assert not late.empty
        assert set(late["symbol"]) == {"AAA"}

    def test_split_filters_by_available_at(self, tmp_store):
        _seed_split_inputs(tmp_store)
        CorporateActionSplitMetricsDataset().run(tmp_store, CorporateActionSplitMetricsOptions())
        early = corporate_action_split_metrics_asof(dt.date(2024, 1, 10), store=tmp_store, symbols=["AAA"])
        assert early.empty
        late = corporate_action_split_metrics_asof(dt.date(2024, 1, 12), store=tmp_store, symbols=["AAA"])
        assert not late.empty
        assert set(late["symbol"]) == {"AAA"}
