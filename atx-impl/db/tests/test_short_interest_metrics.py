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

from db.short_interest_metrics import (
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


# --------------------------------------------------------------------------- #
# Integration: pivot real finra_short_interest rows -> metrics
# --------------------------------------------------------------------------- #


@pytest.fixture
def finra_store(tmp_store):
    """tmp_store with the FINRA source table created (it is built lazily by the
    upstream FinraShortInterestDataset, not by the bootstrap schema template)."""
    from db.finra import FinraShortInterestDataset

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
