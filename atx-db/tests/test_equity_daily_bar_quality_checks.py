"""duplicate_equity_daily_bar_keys: (source, security_id, trade_date) uniqueness.

Registered by migration 0300 (bodies_0300.py) as a critical, gate-worthy check
distinct from ``duplicate_equity_daily_bars``. It exists as a check rather than a
unique index/constraint because the bulk publisher deliberately emits duplicate
``(source, symbol, trade_date)`` rows for recycled tickers -- see the migration's
docstring / task-3 addendum for the full rationale.
"""

from __future__ import annotations

import datetime as dt

from atx_db.quality import run_warehouse_quality_checks


def test_duplicate_equity_daily_bar_keys_is_registered(tmp_store):
    row = tmp_store.con.execute(
        "SELECT dataset_id, table_name, severity, threshold_value, comparator, enabled, "
        "failure_status, source "
        "FROM quality_check_registry WHERE check_name='duplicate_equity_daily_bar_keys'"
    ).fetchone()
    assert row == (
        "tbltickerhistory_daily",
        "equity_daily_bars",
        "critical",
        0.0,
        "eq",
        True,
        "failed",
        "atx_tier1_parity",
    )


def test_duplicate_equity_daily_bar_keys_passes_on_an_empty_table(tmp_store):
    results = {
        r.check_name: r
        for r in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            check_names=("duplicate_equity_daily_bar_keys",),
        )
    }
    result = results["duplicate_equity_daily_bar_keys"]
    assert result.status == "passed"
    assert result.observed_value == 0.0


def test_duplicate_equity_daily_bar_keys_fails_on_a_duplicate_key_pair(tmp_store):
    con = tmp_store.con
    con.execute(
        "INSERT INTO equity_daily_bars (source, security_id, symbol, trade_date, close, available_at) "
        "VALUES ('test', 'SEC-1', 'ABC', DATE '2024-01-02', 10.0, ?), "
        "       ('test', 'SEC-1', 'ABC', DATE '2024-01-02', 10.5, ?)",
        [
            dt.datetime(2024, 1, 2, 22, 0),
            dt.datetime(2024, 1, 2, 22, 5),
        ],
    )

    results = {
        r.check_name: r
        for r in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            check_names=("duplicate_equity_daily_bar_keys",),
        )
    }
    result = results["duplicate_equity_daily_bar_keys"]
    assert result.status == "failed"
    assert result.observed_value is not None and result.observed_value > 0.0


def test_duplicate_equity_daily_bar_keys_and_the_legacy_check_share_the_invariant(tmp_store):
    """Both checks run the same (source, security_id, trade_date) SQL (a shared
    module-level constant in checks_market_reference.py), so they must always agree
    on whether equity_daily_bars has duplicate keys -- one cannot drift out of sync
    with the other."""
    con = tmp_store.con
    con.execute(
        "INSERT INTO equity_daily_bars (source, security_id, symbol, trade_date, close, available_at) "
        "VALUES ('test', 'SEC-1', 'ABC', DATE '2024-01-02', 10.0, ?), "
        "       ('test', 'SEC-1', 'ABC', DATE '2024-01-02', 10.5, ?)",
        [
            dt.datetime(2024, 1, 2, 22, 0),
            dt.datetime(2024, 1, 2, 22, 5),
        ],
    )

    results = {
        r.check_name: r
        for r in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            check_names=("duplicate_equity_daily_bar_keys", "duplicate_equity_daily_bars"),
        )
    }
    new_check = results["duplicate_equity_daily_bar_keys"]
    legacy_check = results["duplicate_equity_daily_bars"]
    assert new_check.status == "failed"
    assert legacy_check.status == "failed"
    assert new_check.observed_value == legacy_check.observed_value == 1.0
