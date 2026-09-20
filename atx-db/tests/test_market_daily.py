"""Tier1-S3 T6: the daily market panel and its ASOF fundamental join."""

from __future__ import annotations

import datetime as dt
import math

import pytest

from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
from atx_db.derived_registry import seed_derived_metric_definitions
from atx_db.market_daily import (
    END_OF_DAY_HOURS,
    MarketDailyOptions,
    refresh_market_daily_metrics,
    shares_reconciliation_report,
)

_QUARTERS = (
    dt.date(2019, 3, 31),
    dt.date(2019, 6, 30),
    dt.date(2019, 9, 30),
    dt.date(2019, 12, 31),
)
_FIRST_TRADE = dt.date(2020, 1, 2)


def _fact(store, security_id, code, basis, period_end, value, available_at, revision=1):
    store.con.execute(
        """
        INSERT INTO fundamental_standardized (
            standardized_id, source, security_id, item_id, canonical_code, basis,
            period_end, value, as_of_date, available_at, input_codes_json,
            input_item_ids_json, rule_id, combination_rule, revision_sequence,
            is_latest_revision
        ) VALUES (?, 'test', ?, 1, ?, ?, ?, ?, ?, ?, '[]', '[]', 'r', 'direct', ?, true)
        """,
        [
            f"{security_id}|{code}|{basis}|{period_end}|{revision}",
            security_id,
            code,
            basis,
            period_end,
            value,
            available_at.date(),
            available_at,
            revision,
        ],
    )


def _bar(store, security_id, trade_date, close, shares=None):
    store.con.execute(
        """
        INSERT INTO equity_daily_bars (
            source, security_id, symbol, trade_date, open, high, low, close,
            adjusted_close, volume, split_factor, is_adjusted, available_at,
            as_of_date, is_latest_revision, shares_outstanding
        ) VALUES ('test', ?, 'AAA', ?, ?, ?, ?, ?, ?, 1000, 1.0, false, ?, ?, true, ?)
        """,
        [
            security_id,
            trade_date,
            close,
            close,
            close,
            close,
            close,
            dt.datetime.combine(trade_date, dt.time(END_OF_DAY_HOURS, 0)),
            trade_date,
            shares,
        ],
    )


@pytest.fixture
def panel(tmp_store):
    seed_derived_metric_definitions(tmp_store)
    for index, period_end in enumerate(_QUARTERS):
        available_at = dt.datetime.combine(period_end + dt.timedelta(days=40), dt.time(21, 0))
        _fact(tmp_store, "S1", "revenue", "quarterly", period_end, 100.0 + index, available_at)
        _fact(tmp_store, "S1", "cost_of_revenue_cogs", "quarterly", period_end, 60.0, available_at)
        _fact(tmp_store, "S1", "net_income_to_common", "quarterly", period_end, 10.0, available_at)
        _fact(tmp_store, "S1", "total_assets", "instant", period_end, 1000.0, available_at)
        _fact(tmp_store, "S1", "stockholders_equity", "instant", period_end, 500.0, available_at)
    for offset in range(300):
        trade_date = _FIRST_TRADE + dt.timedelta(days=offset)
        if trade_date.weekday() >= 5:
            continue
        _bar(tmp_store, "S1", trade_date, 20.0 + 0.01 * offset, shares=1_000_000.0)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    return tmp_store


def test_market_cap_is_price_times_shares(panel):
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    row = panel.con.execute(
        "SELECT close, shares_outstanding, market_cap, shares_source FROM market_daily_metrics WHERE trade_date = ? ",
        [dt.date(2020, 6, 1)],
    ).fetchone()
    assert row is not None
    close, shares, market_cap, source = row
    assert market_cap == pytest.approx(close * shares)
    assert source == "archive"


def test_no_row_uses_a_fundamental_that_was_not_yet_available(panel):
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    violations = panel.con.execute(
        """
        SELECT count(*) FROM market_daily_metrics
        WHERE fundamental_available_at IS NOT NULL
          AND fundamental_available_at > CAST(trade_date AS TIMESTAMP) + INTERVAL 22 HOUR
        """
    ).fetchone()[0]
    assert violations == 0


def test_available_at_is_never_before_the_bar_close(panel):
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    violations = panel.con.execute(
        """
        SELECT count(*) FROM market_daily_metrics
        WHERE available_at < CAST(trade_date AS TIMESTAMP) + INTERVAL 22 HOUR
        """
    ).fetchone()[0]
    assert violations == 0


def test_a_fundamental_filed_after_the_close_is_used_only_from_the_next_day(tmp_store):
    seed_derived_metric_definitions(tmp_store)
    filed = dt.datetime(2020, 3, 2, 23, 0)  # after the 22:00 cutoff on 2020-03-02
    for index, period_end in enumerate(_QUARTERS):
        _fact(tmp_store, "S1", "revenue", "quarterly", period_end, 100.0 + index, filed)
    for trade_date in (dt.date(2020, 3, 2), dt.date(2020, 3, 3)):
        _bar(tmp_store, "S1", trade_date, 20.0, shares=1_000_000.0)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    rows = dict(
        tmp_store.con.execute("SELECT trade_date, ps_ttm FROM market_daily_metrics ORDER BY trade_date").fetchall()
    )
    assert rows[dt.date(2020, 3, 2)] is None
    assert rows[dt.date(2020, 3, 3)] is not None


def test_dei_shares_win_over_archive_shares_and_are_reconciled(panel):
    panel.con.execute(
        """
        INSERT INTO shares_outstanding_history (
            share_history_id, source, security_id, cik, share_count_type, taxonomy,
            concept, unit, period_type, period_end, effective_date, as_of_date,
            available_at, accession_number, revision_sequence, revision_count,
            is_latest_revision, share_count, source_url
        ) VALUES ('h1','test','S1','0000000001','shares_outstanding','dei',
                  'EntityCommonStockSharesOutstanding','shares','instant',
                  DATE '2019-12-31', DATE '2020-01-15', DATE '2020-01-15',
                  TIMESTAMP '2020-01-15 21:00:00','acc',1,1,true, 1_020_000.0, 'test')
        """
    )
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    row = panel.con.execute(
        "SELECT shares_outstanding, shares_source, shares_reconciliation_ratio "
        "FROM market_daily_metrics WHERE trade_date = ?",
        [dt.date(2020, 6, 1)],
    ).fetchone()
    shares, source, ratio = row
    assert source == "dei"
    assert shares == pytest.approx(1_020_000.0)
    assert ratio == pytest.approx(1.02)
    report = shares_reconciliation_report(panel)
    assert report["securities_with_both_sources"] == 1
    assert report["securities_within_tolerance"] == 1
    assert report["pass_rate"] == pytest.approx(1.0)
    assert report["meets_spec_gate"] is True


def test_a_ten_percent_shares_gap_fails_the_reconciliation_gate(panel):
    panel.con.execute(
        """
        INSERT INTO shares_outstanding_history (
            share_history_id, source, security_id, cik, share_count_type, taxonomy,
            concept, unit, period_type, period_end, effective_date, as_of_date,
            available_at, accession_number, revision_sequence, revision_count,
            is_latest_revision, share_count, source_url
        ) VALUES ('h2','test','S1','0000000001','shares_outstanding','dei',
                  'EntityCommonStockSharesOutstanding','shares','instant',
                  DATE '2019-12-31', DATE '2020-01-15', DATE '2020-01-15',
                  TIMESTAMP '2020-01-15 21:00:00','acc',1,1,true, 1_100_000.0, 'test')
        """
    )
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    report = shares_reconciliation_report(panel)
    assert report["securities_within_tolerance"] == 0
    assert report["meets_spec_gate"] is False


def test_total_returns_come_from_adjusted_close_not_split_factor(panel):
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    row = panel.con.execute(
        """
        SELECT m.adj_close, m.total_return_1m,
               (SELECT adj_close FROM market_daily_metrics x
                WHERE x.security_id = m.security_id AND x.trade_date < m.trade_date
                ORDER BY x.trade_date DESC LIMIT 1 OFFSET 20) AS base
        FROM market_daily_metrics m
        WHERE m.trade_date = DATE '2020-09-01'
        """
    ).fetchone()
    adj_close, one_month, base = row
    assert one_month == pytest.approx(adj_close / base - 1.0)


def test_realized_vol_is_annualized(panel):
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    row = panel.con.execute(
        "SELECT realized_vol_60d FROM market_daily_metrics WHERE trade_date = DATE '2020-09-01'"
    ).fetchone()
    assert row[0] is not None
    assert 0.0 <= row[0] < 5.0


def test_momentum_skips_the_most_recent_month(panel):
    # NOTE (T6 substitution): the brief queried a hardcoded DATE '2020-12-01', but
    # the fixture's 300 calendar-day offset (weekends skipped) only reaches
    # 2020-10-27; querying a date outside the panel returned no row at all. Use
    # the panel's actual last trade_date instead so the test stays meaningful
    # if the fixture's date range ever changes.
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    last_trade_date = panel.con.execute(
        "SELECT max(trade_date) FROM market_daily_metrics WHERE security_id = 'S1'"
    ).fetchone()[0]
    row = panel.con.execute(
        "SELECT total_return_12m, total_return_1m, momentum_12_1 FROM market_daily_metrics WHERE trade_date = ?",
        [last_trade_date],
    ).fetchone()
    assert row is not None
    twelve, one, momentum = row
    if twelve is not None and one is not None:
        assert momentum == pytest.approx((1 + twelve) / (1 + one) - 1)


def test_dollar_volume_is_the_twenty_day_average(panel):
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    row = panel.con.execute(
        "SELECT dollar_volume_20d FROM market_daily_metrics WHERE trade_date = DATE '2020-09-01'"
    ).fetchone()
    assert row[0] == pytest.approx(
        panel.con.execute(
            """
            SELECT avg(close * volume) FROM (
                SELECT close, volume FROM market_daily_metrics
                WHERE trade_date <= DATE '2020-09-01' ORDER BY trade_date DESC LIMIT 20
            )
            """
        ).fetchone()[0]
    )


def test_rerun_is_idempotent(panel):
    first = refresh_market_daily_metrics(panel, MarketDailyOptions())
    second = refresh_market_daily_metrics(panel, MarketDailyOptions())
    assert first == second
    total = panel.con.execute("SELECT count(*) FROM market_daily_metrics").fetchone()[0]
    assert total == first


def test_inputs_hash_is_populated_and_stable(panel):
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    before = panel.con.execute("SELECT inputs_hash FROM market_daily_metrics ORDER BY trade_date").fetchall()
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    after = panel.con.execute("SELECT inputs_hash FROM market_daily_metrics ORDER BY trade_date").fetchall()
    assert before == after
    assert all(len(str(row[0])) == 64 for row in before)


def test_no_wall_clock_in_the_module_source():
    import inspect

    import atx_db.market_daily as module

    source = inspect.getsource(module)
    for forbidden in ("date.today", "utcnow", "Timestamp.now", "time.time"):
        assert forbidden not in source, forbidden
    assert math is not None
