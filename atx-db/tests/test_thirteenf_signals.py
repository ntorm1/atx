from __future__ import annotations

import duckdb
import pytest

from atx_db.connection import DuckDBStore
from atx_db.thirteenf_amendments import ensure_thirteenf_amendment_schema
from atx_db.thirteenf_signals import refresh_thirteenf_consensus_signals


@pytest.fixture
def signal_store():
    store = DuckDBStore(":memory:")
    store.connection = duckdb.connect(":memory:")
    ensure_thirteenf_amendment_schema(store)
    store.con.execute("CREATE TABLE equity_daily_bars (trade_date DATE)")
    store.con.execute(
        """
        CREATE TABLE thirteenf_submissions (
            accession_number VARCHAR, filing_date DATE, submission_type VARCHAR,
            cik VARCHAR, period_of_report DATE, source_period VARCHAR
        )
        """
    )
    store.con.execute(
        """
        CREATE TABLE thirteenf_cover_pages (
            accession_number VARCHAR, is_amendment VARCHAR, amendment_no VARCHAR,
            amendment_type VARCHAR, source_period VARCHAR
        )
        """
    )
    store.con.execute(
        """
        CREATE TABLE thirteenf_holdings (
            accession_number VARCHAR, source_period VARCHAR, cusip VARCHAR
        )
        """
    )
    try:
        yield store
    finally:
        store.connection.close()
        store.connection = None


def test_consensus_signal_and_disclosed_exit_outcome(signal_store: DuckDBStore) -> None:
    rate_rows = []
    for manager, zscore in (("0000000001", 3.0), ("0000000002", 4.0), ("0000000003", 5.0)):
        rate_rows.append(
            (
                f"rate-{manager}-q1",
                manager,
                "2020-03-31",
                "2020-05-20",
                2,
                1,
                100,
                10,
                0.1,
                24,
                0.01,
                0.02,
                zscore,
                True,
            )
        )
        rate_rows.append(
            (
                f"rate-{manager}-q2",
                manager,
                "2020-06-30",
                "2020-08-15",
                1,
                0,
                100,
                0,
                0.0,
                24,
                0.01,
                0.02,
                -0.5,
                False,
            )
        )
    signal_store.con.executemany(
        """
        INSERT INTO thirteenf_amendment_rates (
            rate_id, manager_cik, report_period, available_at, filing_count,
            amendment_count, position_count, corrected_position_count,
            amendment_rate, trailing_history_quarters, trailing_24q_mean,
            trailing_24q_stddev, amendment_rate_zscore, is_spike
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        rate_rows,
    )
    signal_store.con.executemany(
        """
        INSERT INTO thirteenf_amendment_corrections (
            correction_id, manager_cik, report_period, amendment_accession,
            amendment_sequence, amendment_type, filing_date, available_at,
            position_key, security_id, cusip, change_type, source_period
        ) VALUES (?, ?, DATE '2020-03-31', ?, 2, 'RESTATEMENT', DATE '2020-05-20',
                  TIMESTAMP '2020-05-20', 'position-key', 'US-CUSIP-037833100',
                  '037833100', 'CHANGED', '2020q2_form13f')
        """,
        [
            (f"correction-{manager}", manager, f"accession-{manager}")
            for manager in ("0000000001", "0000000002", "0000000003")
        ],
    )
    signal_store.con.executemany(
        """
        INSERT INTO thirteenf_submissions
        VALUES (?, DATE '2020-08-15', '13F-HR', ?, DATE '2020-06-30', '2020q3_form13f')
        """
        ,
        [
            (f"next-accession-{manager}", manager)
            for manager in ("0000000001", "0000000002", "0000000003")
        ],
    )
    signal_store.con.executemany(
        "INSERT INTO thirteenf_cover_pages VALUES (?, 'false', NULL, NULL, '2020q3_form13f')",
        [(f"next-accession-{manager}",) for manager in ("0000000001", "0000000002", "0000000003")],
    )
    signal_store.con.execute(
        """
        INSERT INTO thirteenf_holdings
        VALUES ('next-accession-0000000001', '2020q3_form13f', '037833100')
        """
    )

    result = refresh_thirteenf_consensus_signals(
        signal_store,
        start=None,
        end=None,
        minimum_distinct_filers=3,
        run_id="test-signals",
    )

    assert result.signal_rows == 1
    assert signal_store.con.execute(
        """
        SELECT cusip, distinct_filer_count, average_zscore, signal_rank, is_stress_quarter
        FROM thirteenf_consensus_amendment_signals
        """
    ).fetchone() == ("037833100", 3, 4.0, 1, False)
    followups, exits, exit_rate, within_47, median_days = signal_store.con.execute(
        """
        SELECT followup_manager_count, exited_manager_count, disclosed_exit_rate,
               exits_disclosed_within_47_trading_days, median_trading_days_to_followup
        FROM thirteenf_consensus_signal_outcomes
        """
    ).fetchone()
    assert (followups, exits, within_47, median_days) == (3, 2, 0, None)
    assert exit_rate == pytest.approx(2 / 3)
