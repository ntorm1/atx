"""Tests for the derived ``short_interest_metrics`` dataset (S11).

The engine splits into a pure, DB-free transform (``compute_short_interest_metrics``)
that maps deduped FINRA short-interest rows to typed metric rows (change,
days-to-cover, short-%-float, cross-sectional percentiles), and a thin DuckDB
materializer (``refresh_short_interest_metrics`` / ``ShortInterestMetricsDataset``)
that pulls the latest-vintage source rows (+ PIT shares) and writes the result.

No network: metrics derive purely from already-cached warehouse tables.
"""
from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest

from atx_db.short_interest_metrics import (
    DEFAULT_SOURCE,
    ShortInterestMetricsDataset,
    ShortInterestMetricsOptions,
    compute_short_interest_metrics,
    refresh_short_interest_metrics,
    short_interest_metrics_asof,
)


def _ts(s: str) -> pd.Timestamp:
    return pd.Timestamp(s)


def _row(security_id, symbol, current, previous, adv, settlement, av, **over):
    base = {
        "security_id": security_id,
        "symbol": symbol,
        "issue_name": f"{symbol} Common Stock",
        "settlement_date": settlement,
        "current_short_position": current,
        "previous_short_position": previous,
        "average_daily_volume": adv,
        "days_to_cover_source": None,
        "available_at": av,
    }
    base.update(over)
    return base


class TestComputeShortInterestMetrics:
    def test_change_and_days_to_cover(self):
        out = compute_short_interest_metrics(pd.DataFrame([
            _row("S1", "AAA", 1200, 1000, 600, dt.date(2026, 4, 30), _ts("2026-05-10")),
        ]))
        r = out.iloc[0]
        assert r["short_interest_change"] == 200
        assert r["short_interest_change_pct"] == pytest.approx(0.20)
        assert r["days_to_cover"] == pytest.approx(1200 / 600)
        assert r["as_of_date"] == dt.date(2026, 4, 30)

    def test_change_pct_nan_when_previous_nonpositive(self):
        out = compute_short_interest_metrics(pd.DataFrame([
            _row("S1", "AAA", 1000, 0, 500, dt.date(2026, 4, 30), _ts("2026-05-10")),
        ]))
        assert pd.isna(out.iloc[0]["short_interest_change_pct"])

    def test_days_to_cover_nan_when_adv_zero(self):
        out = compute_short_interest_metrics(pd.DataFrame([
            _row("S1", "AAA", 1000, 900, 0, dt.date(2026, 4, 30), _ts("2026-05-10")),
        ]))
        assert pd.isna(out.iloc[0]["days_to_cover"])

    def test_short_pct_shares_outstanding(self):
        out = compute_short_interest_metrics(pd.DataFrame([
            _row("S1", "AAA", 200, 150, 100, dt.date(2026, 4, 30), _ts("2026-05-10"),
                 shares_outstanding=2000.0),
        ]))
        assert out.iloc[0]["short_pct_shares_outstanding"] == pytest.approx(0.10)

    def test_percentile_ranked_within_settlement_cohort(self):
        # two settlements; the heavily-shorted name should top its OWN cohort only
        rows = [
            # settlement A: days_to_cover 1, 2, 4 -> ranks 1/3, 2/3, 3/3
            _row("S1", "AAA", 100, 90, 100, dt.date(2026, 4, 30), _ts("2026-05-10")),  # dtc=1
            _row("S2", "BBB", 200, 90, 100, dt.date(2026, 4, 30), _ts("2026-05-10")),  # dtc=2
            _row("S3", "CCC", 400, 90, 100, dt.date(2026, 4, 30), _ts("2026-05-10")),  # dtc=4
            # settlement B: a single name -> percentile 1.0
            _row("S1", "AAA", 999, 90, 100, dt.date(2026, 5, 15), _ts("2026-05-25")),
        ]
        out = compute_short_interest_metrics(pd.DataFrame(rows))
        by = {(r.security_id, r.settlement_date): r for r in out.itertuples(index=False)}
        assert by[("S3", dt.date(2026, 4, 30))].days_to_cover_percentile == pytest.approx(1.0)
        assert by[("S1", dt.date(2026, 4, 30))].days_to_cover_percentile == pytest.approx(1 / 3)
        assert by[("S2", dt.date(2026, 4, 30))].days_to_cover_percentile == pytest.approx(2 / 3)
        assert by[("S1", dt.date(2026, 4, 30))].universe_count == 3
        # separate cohort: lone name is top of its own settlement
        assert by[("S1", dt.date(2026, 5, 15))].days_to_cover_percentile == pytest.approx(1.0)
        assert by[("S1", dt.date(2026, 5, 15))].universe_count == 1

    def test_cross_sectional_zscore_within_cohort(self):
        # cohort days_to_cover = {1, 2, 4} -> population mean 7/3, std 1.2472
        rows = [
            _row("S1", "AAA", 100, 90, 100, dt.date(2026, 4, 30), _ts("2026-05-10")),  # dtc=1
            _row("S2", "BBB", 200, 90, 100, dt.date(2026, 4, 30), _ts("2026-05-10")),  # dtc=2
            _row("S3", "CCC", 400, 90, 100, dt.date(2026, 4, 30), _ts("2026-05-10")),  # dtc=4
        ]
        out = compute_short_interest_metrics(pd.DataFrame(rows))
        by = {r.security_id: r for r in out.itertuples(index=False)}
        mean = 7 / 3
        std = (((1 - mean) ** 2 + (2 - mean) ** 2 + (4 - mean) ** 2) / 3) ** 0.5
        assert by["S3"].days_to_cover_zscore == pytest.approx((4 - mean) / std)
        assert by["S1"].days_to_cover_zscore == pytest.approx((1 - mean) / std)

    def test_zscore_nan_for_single_name_cohort(self):
        out = compute_short_interest_metrics(pd.DataFrame([
            _row("S1", "AAA", 100, 90, 100, dt.date(2026, 4, 30), _ts("2026-05-10")),
        ]))
        assert pd.isna(out.iloc[0]["days_to_cover_zscore"])  # std=0 -> undefined

    def test_days_to_cover_trend_vs_prior_settlement(self):
        # same security across two settlements; trend = dtc_t - dtc_{t-1}
        rows = [
            _row("S1", "AAA", 100, 90, 100, dt.date(2026, 4, 30), _ts("2026-05-10")),  # dtc=1.0
            _row("S1", "AAA", 250, 100, 100, dt.date(2026, 5, 15), _ts("2026-05-25")),  # dtc=2.5
        ]
        out = compute_short_interest_metrics(pd.DataFrame(rows))
        by = {r.settlement_date: r for r in out.itertuples(index=False)}
        # earliest settlement has no prior
        assert pd.isna(by[dt.date(2026, 4, 30)].days_to_cover_prev)
        assert pd.isna(by[dt.date(2026, 4, 30)].days_to_cover_change)
        # later settlement: prior dtc = 1.0, change = +1.5
        assert by[dt.date(2026, 5, 15)].days_to_cover_prev == pytest.approx(1.0)
        assert by[dt.date(2026, 5, 15)].days_to_cover_change == pytest.approx(1.5)

    def test_multi_settlement_momentum_and_change(self):
        # four settlements for one security; 3-settlement momentum/change lag back 3 rows
        dates = [dt.date(2026, 3, 15), dt.date(2026, 3, 31), dt.date(2026, 4, 15), dt.date(2026, 4, 30)]
        avs = ["2026-03-25", "2026-04-10", "2026-04-25", "2026-05-10"]
        shorts = [1000, 1100, 1200, 1500]  # adv=100 -> days_to_cover 10,11,12,15
        rows = [_row("S1", "AAA", s, s - 50, 100, d, _ts(a)) for s, d, a in zip(shorts, dates, avs)]
        out = compute_short_interest_metrics(pd.DataFrame(rows))
        last = {r.settlement_date: r for r in out.itertuples(index=False)}[dt.date(2026, 4, 30)]
        assert last.short_interest_momentum_3 == pytest.approx((1500 - 1000) / 1000)
        assert last.days_to_cover_change_3 == pytest.approx(15.0 - 10.0)
        # earliest settlement has no 3-back history
        first = {r.settlement_date: r for r in out.itertuples(index=False)}[dt.date(2026, 3, 15)]
        assert pd.isna(first.short_interest_momentum_3)

    def test_long_momentum_uses_six_settlement_lag(self):
        dates = [
            dt.date(2026, 1, 15),
            dt.date(2026, 1, 31),
            dt.date(2026, 2, 15),
            dt.date(2026, 2, 28),
            dt.date(2026, 3, 15),
            dt.date(2026, 3, 31),
            dt.date(2026, 4, 15),
        ]
        shorts = [1000, 1050, 1100, 1200, 1300, 1400, 1700]
        rows = [
            _row("S1", "AAA", s, s - 50, 100, d, _ts(f"2026-04-{i + 1:02d}"))
            for i, (s, d) in enumerate(zip(shorts, dates))
        ]
        out = compute_short_interest_metrics(pd.DataFrame(rows))
        by = {r.settlement_date: r for r in out.itertuples(index=False)}
        first = by[dt.date(2026, 1, 15)]
        last = by[dt.date(2026, 4, 15)]
        assert pd.isna(first.short_interest_momentum_6)
        assert pd.isna(first.days_to_cover_change_6)
        assert last.short_interest_momentum_6 == pytest.approx((1700 - 1000) / 1000)
        assert last.days_to_cover_change_6 == pytest.approx(17.0 - 10.0)

    def test_change_acceleration_uses_prior_settlement_change(self):
        rows = [
            _row("S1", "AAA", 100, 100, 100, dt.date(2026, 3, 31), _ts("2026-04-10")),
            _row("S1", "AAA", 150, 100, 100, dt.date(2026, 4, 15), _ts("2026-04-25")),
            _row("S1", "AAA", 225, 150, 100, dt.date(2026, 4, 30), _ts("2026-05-10")),
        ]
        out = compute_short_interest_metrics(pd.DataFrame(rows))
        by = {r.settlement_date: r for r in out.itertuples(index=False)}
        mid = by[dt.date(2026, 4, 15)]
        last = by[dt.date(2026, 4, 30)]
        assert mid.short_interest_change_pct_accel == pytest.approx(0.5)
        assert pd.isna(mid.days_to_cover_change_accel)
        assert last.short_interest_change_pct_accel == pytest.approx(0.0)
        assert last.days_to_cover_change_accel == pytest.approx(0.25)

    def test_squeeze_candidate_flag(self):
        # single-security cohort -> days_to_cover_percentile = 1.0 (>= 0.9); a rising
        # days-to-cover (positive lag-1 change) then flags a squeeze candidate.
        rising = [_row("S1", "AAA", s, s - 50, 100, d, _ts(a)) for s, d, a in [
            (1000, dt.date(2026, 4, 15), "2026-04-25"),
            (1500, dt.date(2026, 4, 30), "2026-05-10"),
        ]]
        out = compute_short_interest_metrics(pd.DataFrame(rising))
        by = {r.settlement_date: r for r in out.itertuples(index=False)}
        assert bool(by[dt.date(2026, 4, 30)].is_squeeze_candidate) is True
        # a falling days-to-cover is not a squeeze candidate
        falling = [_row("S1", "AAA", s, s + 50, 100, d, _ts(a)) for s, d, a in [
            (1500, dt.date(2026, 4, 15), "2026-04-25"),
            (1000, dt.date(2026, 4, 30), "2026-05-10"),
        ]]
        out2 = compute_short_interest_metrics(pd.DataFrame(falling))
        by2 = {r.settlement_date: r for r in out2.itertuples(index=False)}
        assert bool(by2[dt.date(2026, 4, 30)].is_squeeze_candidate) is False

    def test_pressure_score_and_persistent_pressure_flag(self):
        rows = [_row("S1", "AAA", s, s - 50, 100, d, _ts(a)) for s, d, a in [
            (1000, dt.date(2026, 3, 15), "2026-03-25"),
            (1100, dt.date(2026, 3, 31), "2026-04-10"),
            (1200, dt.date(2026, 4, 15), "2026-04-25"),
            (1500, dt.date(2026, 4, 30), "2026-05-10"),
        ]]
        out = compute_short_interest_metrics(pd.DataFrame(rows))
        by = {r.settlement_date: r for r in out.itertuples(index=False)}
        first = by[dt.date(2026, 3, 15)]
        last = by[dt.date(2026, 4, 30)]
        assert first.short_pressure_score == pytest.approx(80.0)
        assert bool(first.is_persistent_short_pressure) is False
        assert last.short_pressure_score == pytest.approx(100.0)
        assert bool(last.is_squeeze_candidate) is True
        assert bool(last.is_persistent_short_pressure) is True

    def test_winsorized_days_to_cover_and_liquidity_rank(self):
        rows = [
            _row("S1", "AAA", 10, 9, 10, dt.date(2026, 4, 30), _ts("2026-05-10")),
            _row("S2", "BBB", 40, 30, 20, dt.date(2026, 4, 30), _ts("2026-05-10")),
            _row("S3", "CCC", 120, 100, 30, dt.date(2026, 4, 30), _ts("2026-05-10")),
            _row("S4", "DDD", 40000, 100, 40, dt.date(2026, 4, 30), _ts("2026-05-10")),
        ]
        out = compute_short_interest_metrics(pd.DataFrame(rows))
        by = {r.security_id: r for r in out.itertuples(index=False)}
        cap = pd.Series([1.0, 2.0, 4.0, 1000.0]).quantile(0.99)
        win = pd.Series([1.0, 2.0, 4.0, cap])
        mean = win.mean()
        std = (((win - mean) ** 2).mean()) ** 0.5
        assert by["S1"].average_daily_volume_percentile == pytest.approx(0.25)
        assert by["S4"].average_daily_volume_percentile == pytest.approx(1.0)
        assert by["S4"].days_to_cover == pytest.approx(1000.0)
        assert by["S4"].days_to_cover_winsorized == pytest.approx(cap)
        assert by["S4"].days_to_cover_winsorized_zscore == pytest.approx((cap - mean) / std)

    def test_liquid_short_pressure_requires_tradeability_floor(self):
        rows = [
            _row("S1", "LIQ", 100000, 90000, 50000, dt.date(2026, 4, 15), _ts("2026-04-25")),
            _row("S2", "ILL", 100000, 90000, 100, dt.date(2026, 4, 15), _ts("2026-04-25")),
            _row("S3", "LOW", 10000, 9000, 10000, dt.date(2026, 4, 15), _ts("2026-04-25")),
            _row("S1", "LIQ", 150000000, 100000000, 50000, dt.date(2026, 4, 30), _ts("2026-05-10")),
            _row("S2", "ILL", 200000, 100000, 100, dt.date(2026, 4, 30), _ts("2026-05-10")),
            _row("S3", "LOW", 10000, 10000, 10000, dt.date(2026, 4, 30), _ts("2026-05-10")),
        ]
        out = compute_short_interest_metrics(pd.DataFrame(rows))
        latest = {
            r.security_id: r
            for r in out.itertuples(index=False)
            if r.settlement_date == dt.date(2026, 4, 30)
        }
        assert bool(latest["S1"].is_squeeze_candidate) is True
        assert latest["S1"].short_pressure_score >= 70.0
        assert latest["S1"].liquid_short_pressure_score == pytest.approx(latest["S1"].short_pressure_score)
        assert bool(latest["S1"].is_liquid_short_pressure) is True
        assert bool(latest["S2"].is_liquid_short_pressure) is False
        assert pd.isna(latest["S2"].liquid_short_pressure_score)
        assert bool(latest["S3"].is_liquid_short_pressure) is False

    def test_metric_id_deterministic_and_unique(self):
        rows = pd.DataFrame([
            _row("S1", "AAA", 100, 90, 100, dt.date(2026, 4, 30), _ts("2026-05-10")),
            _row("S2", "BBB", 200, 90, 100, dt.date(2026, 4, 30), _ts("2026-05-10")),
        ])
        a = compute_short_interest_metrics(rows)
        b = compute_short_interest_metrics(rows)
        assert list(a["metric_id"]) == list(b["metric_id"])
        assert a["metric_id"].is_unique

    def test_empty_returns_typed_empty(self):
        out = compute_short_interest_metrics(pd.DataFrame())
        assert out.empty
        assert "days_to_cover_percentile" in out.columns
        assert "short_pressure_score" in out.columns
        assert "is_persistent_short_pressure" in out.columns
        assert "liquid_short_pressure_score" in out.columns
        assert "is_liquid_short_pressure" in out.columns


# --------------------------------------------------------------------------- #
# Integration: pivot real finra_short_interest rows -> metrics
# --------------------------------------------------------------------------- #


@pytest.fixture
def finra_store(tmp_store):
    """tmp_store with the FINRA source table created (it is built lazily by the
    upstream FinraShortInterestDataset, not by the bootstrap schema template)."""
    from atx_db.finra import FinraShortInterestDataset

    FinraShortInterestDataset().ensure_schema(tmp_store)
    return tmp_store


def _insert_finra(store, *, security_id, symbol, current, previous, adv, settlement, av,
                  days_to_cover=None, revision_flag=None):
    store.con.execute(
        """
        INSERT INTO finra_short_interest (
            settlement_date, symbol, issue_name, current_short_position_quantity,
            previous_short_position_quantity, average_daily_volume_quantity,
            days_to_cover_quantity, revision_flag, security_id, available_at,
            source_loaded_at, source_url
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'https://finra.test')
        """,
        [settlement, symbol, f"{symbol} Common Stock", current, previous, adv,
         days_to_cover, revision_flag, security_id, av, av],
    )


def _insert_shares(store, *, security_id, count, effective, av):
    store.con.execute(
        """
        INSERT INTO shares_outstanding_history (
            share_history_id, source, security_id, cik, share_count_type, taxonomy,
            concept, unit, period_type, period_end, effective_date, as_of_date,
            accession_number, revision_sequence, revision_count, is_latest_revision,
            share_count, source_url, available_at, source_loaded_at, updated_at
        ) VALUES (?, 'test', ?, '0000000', 'shares_outstanding', 'dei', 'EntityCommonStockSharesOutstanding',
                  'shares', 'instant', ?, ?, ?, 'acc', 0, 1, true, ?, 'http://x', ?, ?, ?)
        """,
        [f"{security_id}|{effective}", security_id, effective, effective, effective, count, av, av, av],
    )


class TestRefreshIntegration:
    def test_materializes_metrics_and_percentiles(self, finra_store):
        s = dt.date(2026, 4, 30)
        av = dt.datetime(2026, 5, 10, 22, 0)
        _insert_finra(finra_store, security_id="S1", symbol="AAA", current=100, previous=90, adv=100, settlement=s, av=av)
        _insert_finra(finra_store, security_id="S2", symbol="BBB", current=400, previous=90, adv=100, settlement=s, av=av)
        _insert_shares(finra_store, security_id="S1", count=10000.0, effective=dt.date(2026, 3, 31), av=dt.datetime(2026, 4, 1))

        n = refresh_short_interest_metrics(finra_store, ShortInterestMetricsOptions())
        assert n == 2
        df = finra_store.con.execute(
            "SELECT symbol, days_to_cover, days_to_cover_percentile, short_pct_shares_outstanding, universe_count "
            "FROM short_interest_metrics ORDER BY symbol"
        ).df()
        rows = {r["symbol"]: r for _, r in df.iterrows()}
        assert rows["BBB"]["days_to_cover"] == pytest.approx(4.0)
        assert rows["BBB"]["days_to_cover_percentile"] == pytest.approx(1.0)  # most-shorted in cohort
        assert rows["AAA"]["days_to_cover_percentile"] == pytest.approx(0.5)
        assert rows["AAA"]["universe_count"] == 2
        # short-%-float resolved from the PIT shares row for S1 only
        assert rows["AAA"]["short_pct_shares_outstanding"] == pytest.approx(100 / 10000)
        assert pd.isna(rows["BBB"]["short_pct_shares_outstanding"])

    def test_latest_vintage_supersedes_revision(self, finra_store):
        s = dt.date(2026, 4, 30)
        _insert_finra(finra_store, security_id="S1", symbol="AAA", current=100, previous=90, adv=100,
                      settlement=s, av=dt.datetime(2026, 5, 10, 22, 0))
        _insert_finra(finra_store, security_id="S1", symbol="AAA", current=150, previous=90, adv=100,
                      settlement=s, av=dt.datetime(2026, 5, 20, 22, 0), revision_flag="R")
        refresh_short_interest_metrics(finra_store, ShortInterestMetricsOptions())
        df = finra_store.con.execute("SELECT current_short_position FROM short_interest_metrics").df()
        assert len(df) == 1
        assert df.iloc[0]["current_short_position"] == 150  # the later revision wins

    def test_dataset_run_is_idempotent(self, finra_store):
        s = dt.date(2026, 4, 30)
        av = dt.datetime(2026, 5, 10, 22, 0)
        _insert_finra(finra_store, security_id="S1", symbol="AAA", current=100, previous=90, adv=100, settlement=s, av=av)
        ds = ShortInterestMetricsDataset()
        r1 = ds.run(finra_store, ShortInterestMetricsOptions())
        n1 = finra_store.con.execute("SELECT count(*) FROM short_interest_metrics").fetchone()[0]
        r2 = ds.run(finra_store, ShortInterestMetricsOptions())
        n2 = finra_store.con.execute("SELECT count(*) FROM short_interest_metrics").fetchone()[0]
        assert r1.rows_loaded == r2.rows_loaded
        assert n1 == n2 == 1


class TestAsofReader:
    def test_filters_by_available_at(self, finra_store):
        s = dt.date(2026, 4, 30)
        av = dt.datetime(2026, 5, 10, 22, 0)
        _insert_finra(finra_store, security_id="S1", symbol="AAA", current=100, previous=90, adv=100, settlement=s, av=av)
        ShortInterestMetricsDataset().run(finra_store, ShortInterestMetricsOptions())
        # before publication: nothing visible
        early = short_interest_metrics_asof(dt.date(2026, 5, 1), store=finra_store, symbols=["AAA"])
        assert early.empty
        # after publication: visible
        late = short_interest_metrics_asof(dt.date(2026, 5, 31), store=finra_store, symbols=["AAA"])
        assert not late.empty
        assert set(late["symbol"]) == {"AAA"}
