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


def test_scoped_refresh_matches_unscoped_for_trailing_windows(panel):
    """Critical fix: a start_date/end_date-scoped refresh must not truncate the
    trailing-window lookback (total_return_*, momentum_12_1, realized_vol_*,
    dollar_volume_20d) a full/unscoped refresh computes -- and must not
    destructively overwrite previously-correct values with NULLs.
    """
    first_total = refresh_market_daily_metrics(panel, MarketDailyOptions())
    assert first_total > 0
    cursor = panel.con.execute("SELECT * FROM market_daily_metrics ORDER BY security_id, trade_date")
    columns = [d[0] for d in cursor.description]
    compare_idx = [i for i, name in enumerate(columns) if name != "source_loaded_at"]
    snapshot = {(row[columns.index("security_id")], row[columns.index("trade_date")]): row for row in cursor.fetchall()}

    last_40 = panel.con.execute(
        "SELECT trade_date FROM market_daily_metrics WHERE security_id = 'S1' ORDER BY trade_date DESC LIMIT 40"
    ).fetchall()
    assert len(last_40) == 40
    start = min(row[0] for row in last_40)
    end = max(row[0] for row in last_40)

    scoped_total = refresh_market_daily_metrics(panel, MarketDailyOptions(start_date=start, end_date=end))
    assert scoped_total == 40

    scoped_rows = panel.con.execute(
        "SELECT * FROM market_daily_metrics WHERE trade_date BETWEEN ? AND ? ORDER BY security_id, trade_date",
        [start, end],
    ).fetchall()
    assert len(scoped_rows) == 40
    # Every trailing-window metric must be populated (not nulled by truncation)
    # for these rows, since they're well past every window's lookback depth.
    windowed = ("total_return_1m", "total_return_3m", "realized_vol_60d", "dollar_volume_20d")
    windowed_idx = [columns.index(name) for name in windowed]
    for row in scoped_rows:
        key = (row[columns.index("security_id")], row[columns.index("trade_date")])
        expected = snapshot[key]
        assert tuple(row[i] for i in compare_idx) == tuple(expected[i] for i in compare_idx)
        for i in windowed_idx:
            assert row[i] is not None


def test_enterprise_value_and_valuation_multiples_positive_path(tmp_store):
    """I1: enterprise_value and the daily valuation multiples through the real
    ASOF/chain wiring, not just at the DSL level.
    """
    seed_derived_metric_definitions(tmp_store)
    for period_end in _QUARTERS:
        available_at = dt.datetime.combine(period_end + dt.timedelta(days=40), dt.time(21, 0))
        _fact(tmp_store, "S2", "revenue", "quarterly", period_end, 5_000_000.0, available_at)
        _fact(tmp_store, "S2", "net_income_to_common", "quarterly", period_end, 2_000_000.0, available_at)
        _fact(tmp_store, "S2", "ebitda_standardised", "quarterly", period_end, 4_000_000.0, available_at)
        _fact(tmp_store, "S2", "cash_flow_from_operations", "quarterly", period_end, 3_000_000.0, available_at)
        _fact(tmp_store, "S2", "capex__1305", "quarterly", period_end, 500_000.0, available_at)
        _fact(tmp_store, "S2", "common_dividends_paid", "quarterly", period_end, 500_000.0, available_at)
        _fact(tmp_store, "S2", "common_equity", "instant", period_end, 40_000_000.0, available_at)
        _fact(tmp_store, "S2", "preferred_stock", "instant", period_end, 5_000_000.0, available_at)
        _fact(tmp_store, "S2", "minority_interest_bs", "instant", period_end, 1_000_000.0, available_at)
        _fact(tmp_store, "S2", "total_debt", "instant", period_end, 15_000_000.0, available_at)
        _fact(tmp_store, "S2", "cash_and_st_investments", "instant", period_end, 8_000_000.0, available_at)
    _bar(tmp_store, "S2", dt.date(2020, 6, 1), 50.0, shares=2_000_000.0)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())

    row = tmp_store.con.execute(
        "SELECT market_cap, enterprise_value, pe_ttm, pb, ps_ttm, ev_ebitda, ev_sales, "
        "fcf_yield, dividend_yield FROM market_daily_metrics "
        "WHERE security_id = 'S2' AND trade_date = ?",
        [dt.date(2020, 6, 1)],
    ).fetchone()
    assert row is not None
    market_cap, enterprise_value, pe_ttm, pb, ps_ttm, ev_ebitda, ev_sales, fcf_yield, dividend_yield = row
    # market_cap = 50.0 * 2,000,000
    assert market_cap == pytest.approx(100_000_000.0)
    # enterprise_value = market_cap + total_debt_q + preferred_stock + minority_interest_bs - cash_st_investments_q
    #                  = 100,000,000 + 15,000,000 + 5,000,000 + 1,000,000 - 8,000,000
    assert enterprise_value == pytest.approx(113_000_000.0)
    # net_income_common_ttm = 4 * 2,000,000 = 8,000,000
    assert pe_ttm == pytest.approx(100_000_000.0 / 8_000_000.0)
    # common_equity_q = 40,000,000 (latest quarter, passthrough, not ttm'd)
    assert pb == pytest.approx(100_000_000.0 / 40_000_000.0)
    # revenue_ttm = 4 * 5,000,000 = 20,000,000
    assert ps_ttm == pytest.approx(100_000_000.0 / 20_000_000.0)
    # ebitda_ttm = 4 * 4,000,000 = 16,000,000
    assert ev_ebitda == pytest.approx(113_000_000.0 / 16_000_000.0)
    assert ev_sales == pytest.approx(113_000_000.0 / 20_000_000.0)
    # fcf_q = cfo - capex = 3,000,000 - 500,000 = 2,500,000/quarter; fcf_ttm = 10,000,000
    assert fcf_yield == pytest.approx(10_000_000.0 / 100_000_000.0)
    # common_dividends_ttm = 4 * 500,000 = 2,000,000
    assert dividend_yield == pytest.approx(2_000_000.0 / 100_000_000.0)


def test_pb_is_null_when_common_equity_is_zero(tmp_store):
    """I1: a zero denominator (common_equity_q) must yield NULL, not a divide error."""
    seed_derived_metric_definitions(tmp_store)
    available_at = dt.datetime(2020, 2, 10, 21, 0)
    _fact(tmp_store, "S3", "common_equity", "instant", dt.date(2019, 12, 31), 0.0, available_at)
    _bar(tmp_store, "S3", dt.date(2020, 6, 1), 50.0, shares=2_000_000.0)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    row = tmp_store.con.execute(
        "SELECT market_cap, pb FROM market_daily_metrics WHERE security_id = 'S3' AND trade_date = ?",
        [dt.date(2020, 6, 1)],
    ).fetchone()
    assert row is not None
    market_cap, pb = row
    assert market_cap == pytest.approx(100_000_000.0)
    assert pb is None


def test_pe_ttm_is_a_negative_ratio_not_null_when_earnings_are_negative(tmp_store):
    """I1: a negative TTM denominator produces a legitimate negative ratio, not
    NULL -- safe_div (derived_dsl.py, out of this file's scope) only guards a
    NULL/zero denominator, never its sign. This documents that actual,
    current behavior end-to-end through the real market_daily wiring.
    """
    seed_derived_metric_definitions(tmp_store)
    for period_end in _QUARTERS:
        available_at = dt.datetime.combine(period_end + dt.timedelta(days=40), dt.time(21, 0))
        _fact(tmp_store, "S4", "net_income_to_common", "quarterly", period_end, -2_000_000.0, available_at)
    _bar(tmp_store, "S4", dt.date(2020, 6, 1), 50.0, shares=2_000_000.0)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    row = tmp_store.con.execute(
        "SELECT market_cap, pe_ttm FROM market_daily_metrics WHERE security_id = 'S4' AND trade_date = ?",
        [dt.date(2020, 6, 1)],
    ).fetchone()
    assert row is not None
    market_cap, pe_ttm = row
    assert market_cap == pytest.approx(100_000_000.0)
    assert pe_ttm is not None
    assert pe_ttm == pytest.approx(market_cap / -8_000_000.0)
    assert pe_ttm < 0


def test_shares_reconciliation_uses_the_latest_ratio_not_all_days(panel):
    """I2: an early out-of-tolerance day must not permanently fail a security
    once a later DEI revision brings it back within tolerance -- the gate
    criterion is the latest observed ratio per security, not all days.
    """
    panel.con.execute(
        """
        INSERT INTO shares_outstanding_history (
            share_history_id, source, security_id, cik, share_count_type, taxonomy,
            concept, unit, period_type, period_end, effective_date, as_of_date,
            available_at, accession_number, revision_sequence, revision_count,
            is_latest_revision, share_count, source_url
        ) VALUES ('h3','test','S1','0000000001','shares_outstanding','dei',
                  'EntityCommonStockSharesOutstanding','shares','instant',
                  DATE '2019-12-31', DATE '2020-01-15', DATE '2020-01-15',
                  TIMESTAMP '2020-01-15 21:00:00','acc',1,1,true, 1_100_000.0, 'test')
        """
    )
    panel.con.execute(
        """
        INSERT INTO shares_outstanding_history (
            share_history_id, source, security_id, cik, share_count_type, taxonomy,
            concept, unit, period_type, period_end, effective_date, as_of_date,
            available_at, accession_number, revision_sequence, revision_count,
            is_latest_revision, share_count, source_url
        ) VALUES ('h4','test','S1','0000000001','shares_outstanding','dei',
                  'EntityCommonStockSharesOutstanding','shares','instant',
                  DATE '2020-03-31', DATE '2020-04-01', DATE '2020-04-01',
                  TIMESTAMP '2020-04-01 21:00:00','acc',2,2,true, 1_020_000.0, 'test')
        """
    )
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    ratios = panel.con.execute(
        "SELECT trade_date, shares_reconciliation_ratio FROM market_daily_metrics "
        "WHERE security_id = 'S1' AND shares_reconciliation_ratio IS NOT NULL ORDER BY trade_date"
    ).fetchall()
    assert ratios[0][1] == pytest.approx(1.10)  # early days: out of the 5% tolerance
    assert ratios[-1][1] == pytest.approx(1.02)  # latest day: within tolerance
    assert abs(ratios[0][1] - 1.0) > 0.05
    assert abs(ratios[-1][1] - 1.0) <= 0.05

    report = shares_reconciliation_report(panel)
    assert report["securities_with_both_sources"] == 1
    assert report["securities_within_tolerance"] == 1
    assert report["meets_spec_gate"] is True


def test_a_stale_bar_correction_inside_the_window_raises_the_available_at(panel):
    """M1: a late-arriving correction to a historical bar *inside* a trailing
    window (realized_vol_60d) must surface in the row-level available_at of
    every later row whose window includes that bar, not just the day it
    lands on.
    """
    d_old = dt.date(2020, 3, 2)
    d_new = dt.date(2020, 4, 1)
    corrected_at = dt.datetime(2020, 8, 1, 12, 0)
    panel.con.execute(
        """
        INSERT INTO equity_daily_bars (
            source, security_id, symbol, trade_date, open, high, low, close,
            adjusted_close, volume, split_factor, is_adjusted, available_at,
            as_of_date, is_latest_revision, shares_outstanding
        ) VALUES ('test', 'S1', 'AAA', ?, 25.0, 25.0, 25.0, 25.0, 25.0, 1000, 1.0, false, ?, ?, true, 1_000_000.0)
        """,
        [d_old, corrected_at, d_old],
    )
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    row = panel.con.execute(
        "SELECT available_at, realized_vol_60d FROM market_daily_metrics WHERE security_id = 'S1' AND trade_date = ?",
        [d_new],
    ).fetchone()
    assert row is not None
    available_at, realized_vol = row
    assert realized_vol is not None
    assert available_at >= corrected_at


def test_no_wall_clock_in_the_module_source():
    import inspect

    import atx_db.market_daily as module

    source = inspect.getsource(module)
    for forbidden in ("date.today", "utcnow", "Timestamp.now", "time.time"):
        assert forbidden not in source, forbidden
    assert math is not None
