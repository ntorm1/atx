"""Consensus signals derived from point-in-time 13F amendment rates."""

from __future__ import annotations

import datetime as dt
import time
from dataclasses import dataclass

from .connection import DuckDBStore

DEFAULT_MAX_SIGNAL_RANK_PER_QUARTER = 20


@dataclass(frozen=True)
class ThirteenFSignalRefreshResult:
    regime_rows: int
    signal_rows: int
    outcome_rows: int
    elapsed_seconds: float


def ensure_thirteenf_signal_schema(store: DuckDBStore) -> None:
    con = store.con
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS thirteenf_amendment_market_regimes (
            report_period DATE PRIMARY KEY,
            as_of_date DATE,
            available_at TIMESTAMP NOT NULL,
            manager_count INTEGER NOT NULL,
            amending_manager_count INTEGER NOT NULL,
            spike_manager_count INTEGER NOT NULL,
            amending_manager_share DOUBLE NOT NULL,
            trailing_history_quarters INTEGER NOT NULL,
            trailing_24q_mean DOUBLE,
            trailing_24q_stddev DOUBLE,
            stress_zscore DOUBLE,
            is_stress BOOLEAN NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS thirteenf_consensus_amendment_signals (
            signal_id VARCHAR PRIMARY KEY,
            report_period DATE NOT NULL,
            as_of_date DATE,
            available_at TIMESTAMP,
            cusip VARCHAR NOT NULL,
            security_id VARCHAR,
            signal_available_at TIMESTAMP NOT NULL,
            distinct_filer_count INTEGER NOT NULL,
            average_zscore DOUBLE NOT NULL,
            minimum_zscore DOUBLE NOT NULL,
            maximum_zscore DOUBLE NOT NULL,
            corrected_position_events INTEGER NOT NULL,
            manager_ciks_json VARCHAR NOT NULL,
            signal_rank INTEGER NOT NULL,
            is_stress_quarter BOOLEAN NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS thirteenf_consensus_signal_outcomes (
            signal_id VARCHAR PRIMARY KEY,
            report_period DATE NOT NULL,
            as_of_date DATE,
            available_at TIMESTAMP,
            cusip VARCHAR NOT NULL,
            signal_available_at TIMESTAMP NOT NULL,
            followup_manager_count INTEGER NOT NULL,
            exited_manager_count INTEGER NOT NULL,
            disclosed_exit_rate DOUBLE,
            exits_disclosed_within_47_trading_days INTEGER,
            median_trading_days_to_followup DOUBLE,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute("ALTER TABLE thirteenf_amendment_market_regimes ADD COLUMN IF NOT EXISTS as_of_date DATE")
    for table in (
        "thirteenf_consensus_amendment_signals",
        "thirteenf_consensus_signal_outcomes",
    ):
        con.execute(f"ALTER TABLE {table} ADD COLUMN IF NOT EXISTS as_of_date DATE")
        con.execute(f"ALTER TABLE {table} ADD COLUMN IF NOT EXISTS available_at TIMESTAMP")
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_13f_signal_period_rank ON thirteenf_consensus_amendment_signals(report_period, signal_rank)",
        "CREATE INDEX IF NOT EXISTS idx_13f_signal_cusip_period ON thirteenf_consensus_amendment_signals(cusip, report_period)",
        "CREATE INDEX IF NOT EXISTS idx_13f_outcome_period ON thirteenf_consensus_signal_outcomes(report_period)",
    ):
        con.execute(statement)


def _date_filter(column: str, start: dt.date | None, end: dt.date | None) -> str:
    clauses: list[str] = []
    if start is not None:
        clauses.append(f"{column} >= DATE '{start.isoformat()}'")
    if end is not None:
        clauses.append(f"{column} <= DATE '{end.isoformat()}'")
    return " AND ".join(clauses) if clauses else "true"


def refresh_thirteenf_consensus_signals(
    store: DuckDBStore,
    *,
    start: dt.date | None = None,
    end: dt.date | None = None,
    minimum_distinct_filers: int = 3,
    stress_zscore: float = 1.5,
    run_id: str | None = None,
) -> ThirteenFSignalRefreshResult:
    """Build consensus signals and disclosed-next-filing exit outcomes.

    A signal becomes available only when the last required amendment is filed.
    Market stress uses a trailing-only 24-quarter baseline. Exit outcomes are
    deliberately named *disclosed* exits: quarterly 13F data cannot reveal the
    actual trade date between report dates.
    """
    if minimum_distinct_filers < 2:
        raise ValueError("minimum_distinct_filers must be at least 2")
    if stress_zscore <= 0:
        raise ValueError("stress_zscore must be positive")
    started = time.perf_counter()
    ensure_thirteenf_signal_schema(store)
    con = store.con
    target = _date_filter("report_period", start, end)

    with store.transaction():
        for table in (
            "thirteenf_consensus_signal_outcomes",
            "thirteenf_consensus_amendment_signals",
            "thirteenf_amendment_market_regimes",
        ):
            con.execute(f"DELETE FROM {table} WHERE {target}")

        con.execute(
            f"""
            INSERT INTO thirteenf_amendment_market_regimes (
                report_period, as_of_date, available_at, manager_count, amending_manager_count,
                spike_manager_count, amending_manager_share, trailing_history_quarters,
                trailing_24q_mean, trailing_24q_stddev, stress_zscore, is_stress, run_id
            )
            WITH quarter_rates AS (
                SELECT
                    report_period,
                    max(available_at) AS available_at,
                    count(*)::INTEGER AS manager_count,
                    count(*) FILTER (WHERE amendment_count > 0)::INTEGER AS amending_manager_count,
                    count(*) FILTER (WHERE is_spike)::INTEGER AS spike_manager_count,
                    count(*) FILTER (WHERE amendment_count > 0)::DOUBLE / count(*) AS amending_manager_share
                FROM thirteenf_amendment_rates
                WHERE is_latest_revision
                GROUP BY report_period
            ),
            history AS (
                SELECT
                    *,
                    (count(*) OVER window_24)::INTEGER AS trailing_history_quarters,
                    avg(amending_manager_share) OVER window_24 AS trailing_mean,
                    stddev_samp(amending_manager_share) OVER window_24 AS trailing_stddev
                FROM quarter_rates
                WINDOW window_24 AS (
                    ORDER BY report_period ROWS BETWEEN 24 PRECEDING AND 1 PRECEDING
                )
            ),
            scored AS (
                SELECT
                    *,
                    CASE WHEN trailing_stddev > 0
                         THEN (amending_manager_share - trailing_mean) / trailing_stddev
                    END AS regime_zscore
                FROM history
            )
            SELECT
                report_period, report_period, available_at, manager_count, amending_manager_count,
                spike_manager_count, amending_manager_share, trailing_history_quarters,
                trailing_mean, trailing_stddev, regime_zscore,
                coalesce(trailing_history_quarters >= 8 AND regime_zscore >= {float(stress_zscore)}, false), ?
            FROM scored
            WHERE {target}
            """,
            [run_id],
        )
        con.execute(
            f"""
            INSERT INTO thirteenf_consensus_amendment_signals (
                signal_id, report_period, as_of_date, available_at,
                cusip, security_id, signal_available_at,
                distinct_filer_count, average_zscore, minimum_zscore, maximum_zscore,
                corrected_position_events, manager_ciks_json, signal_rank,
                is_stress_quarter, run_id
            )
            WITH manager_security AS (
                SELECT
                    c.report_period,
                    c.cusip,
                    any_value(c.security_id) AS security_id,
                    c.manager_cik,
                    max(c.available_at) AS manager_signal_available_at,
                    max(r.amendment_rate_zscore) AS amendment_rate_zscore,
                    count(*)::INTEGER AS correction_events
                FROM thirteenf_amendment_corrections c
                JOIN thirteenf_amendment_rates r
                  ON r.manager_cik = c.manager_cik
                 AND r.report_period = c.report_period
                 AND r.is_latest_revision
                 AND r.is_spike
                WHERE c.is_latest_revision
                GROUP BY c.report_period, c.cusip, c.manager_cik
            ),
            consensus AS (
                SELECT
                    report_period,
                    cusip,
                    any_value(security_id) AS security_id,
                    max(manager_signal_available_at) AS signal_available_at,
                    count(*)::INTEGER AS distinct_filer_count,
                    avg(amendment_rate_zscore) AS average_zscore,
                    min(amendment_rate_zscore) AS minimum_zscore,
                    max(amendment_rate_zscore) AS maximum_zscore,
                    sum(correction_events)::INTEGER AS corrected_position_events,
                    to_json(list(manager_cik ORDER BY manager_cik))::VARCHAR AS manager_ciks_json
                FROM manager_security
                GROUP BY report_period, cusip
                HAVING count(*) >= {int(minimum_distinct_filers)}
            ),
            ranked AS (
                SELECT
                    *,
                    row_number() OVER (
                        PARTITION BY report_period
                        ORDER BY average_zscore DESC, distinct_filer_count DESC, cusip
                    )::INTEGER AS signal_rank
                FROM consensus
            )
            SELECT
                md5(cast(r.report_period AS VARCHAR) || '|' || r.cusip),
                r.report_period, r.report_period, r.signal_available_at,
                r.cusip, r.security_id, r.signal_available_at,
                r.distinct_filer_count, r.average_zscore, r.minimum_zscore,
                r.maximum_zscore, r.corrected_position_events, r.manager_ciks_json,
                r.signal_rank, coalesce(m.is_stress, false), ?
            FROM ranked r
            LEFT JOIN thirteenf_amendment_market_regimes m USING (report_period)
            WHERE {_date_filter("r.report_period", start, end)}
            """,
            [run_id],
        )
        con.execute(
            f"""
            INSERT INTO thirteenf_consensus_signal_outcomes (
                signal_id, report_period, as_of_date, available_at,
                cusip, signal_available_at,
                followup_manager_count, exited_manager_count, disclosed_exit_rate,
                exits_disclosed_within_47_trading_days, median_trading_days_to_followup,
                run_id
            )
            WITH signal_managers AS (
                SELECT DISTINCT
                    s.signal_id,
                    s.report_period,
                    s.cusip,
                    s.signal_available_at,
                    c.manager_cik
                FROM thirteenf_consensus_amendment_signals s
                JOIN thirteenf_amendment_corrections c
                  ON c.report_period = s.report_period
                 AND c.cusip = s.cusip
                 AND c.is_latest_revision
                JOIN thirteenf_amendment_rates r
                  ON r.manager_cik = c.manager_cik
                 AND r.report_period = c.report_period
                 AND r.is_latest_revision
                 AND r.is_spike
                WHERE {_date_filter("s.report_period", start, end)}
            ),
            next_reports AS (
                SELECT
                    sm.*,
                    next_rate.report_period AS next_report_period,
                    next_rate.available_at AS next_available_at
                FROM signal_managers sm
                LEFT JOIN LATERAL (
                    SELECT report_period, available_at
                    FROM thirteenf_amendment_rates r
                    WHERE r.manager_cik = sm.manager_cik
                      AND r.report_period > sm.report_period
                      AND r.is_latest_revision
                    ORDER BY r.report_period
                    LIMIT 1
                ) next_rate ON true
            ),
            ordered_next_filings AS (
                SELECT
                    nr.signal_id,
                    nr.manager_cik,
                    nr.cusip,
                    s.accession_number,
                    s.source_period,
                    CASE
                        WHEN NOT coalesce(
                            lower(trim(c.is_amendment)) IN ('true', '1', 'yes', 'y'),
                            upper(trim(s.submission_type)) LIKE '%/A'
                        ) THEN 'FULL'
                        WHEN upper(coalesce(c.amendment_type, '')) LIKE '%RESTATEMENT%' THEN 'FULL'
                        ELSE 'ADD'
                    END AS filing_mode,
                    row_number() OVER (
                        PARTITION BY nr.signal_id, nr.manager_cik
                        ORDER BY s.filing_date, coalesce(try_cast(c.amendment_no AS INTEGER), 0), s.accession_number
                    )::INTEGER AS filing_sequence
                FROM next_reports nr
                JOIN thirteenf_submissions s
                  ON lpad(trim(s.cik), 10, '0') = nr.manager_cik
                 AND s.period_of_report = nr.next_report_period
                LEFT JOIN thirteenf_cover_pages c
                  ON c.accession_number = s.accession_number
                 AND c.source_period = s.source_period
                WHERE nr.next_report_period IS NOT NULL
                  AND upper(trim(s.submission_type)) IN ('13F-HR', '13F-HR/A')
            ),
            next_bases AS (
                SELECT
                    signal_id,
                    manager_cik,
                    cusip,
                    max(filing_sequence) AS cutoff_sequence,
                    max(filing_sequence) FILTER (WHERE filing_mode = 'FULL') AS base_sequence
                FROM ordered_next_filings
                GROUP BY signal_id, manager_cik, cusip
            ),
            eligible_next_accessions AS (
                SELECT f.signal_id, f.manager_cik, f.cusip, f.accession_number, f.source_period
                FROM next_bases b
                JOIN ordered_next_filings f
                  ON f.signal_id = b.signal_id
                 AND f.manager_cik = b.manager_cik
                 AND (
                        f.filing_sequence = b.base_sequence
                        OR (
                            f.filing_mode = 'ADD'
                            AND f.filing_sequence > b.base_sequence
                            AND f.filing_sequence <= b.cutoff_sequence
                        )
                     )
                WHERE b.base_sequence IS NOT NULL
            ),
            present_positions AS (
                SELECT DISTINCT e.signal_id, e.manager_cik
                FROM eligible_next_accessions e
                JOIN thirteenf_holdings h
                  ON h.accession_number = e.accession_number
                 AND h.source_period = e.source_period
                 AND upper(trim(h.cusip)) = e.cusip
            ),
            manager_outcomes AS (
                SELECT
                    nr.*,
                    nr.next_report_period IS NOT NULL
                      AND present.manager_cik IS NULL AS disclosed_exit,
                    CASE
                        WHEN nr.next_available_at IS NULL THEN NULL
                        WHEN EXISTS (
                            SELECT 1 FROM equity_daily_bars b
                            WHERE b.trade_date > cast(nr.signal_available_at AS DATE)
                              AND b.trade_date <= cast(nr.next_available_at AS DATE)
                        ) THEN (
                            SELECT count(DISTINCT b.trade_date)
                            FROM equity_daily_bars b
                            WHERE b.trade_date > cast(nr.signal_available_at AS DATE)
                              AND b.trade_date <= cast(nr.next_available_at AS DATE)
                        )
                    END AS trading_days_to_followup
                FROM next_reports nr
                LEFT JOIN present_positions present
                  ON present.signal_id = nr.signal_id
                 AND present.manager_cik = nr.manager_cik
            )
            SELECT
                signal_id,
                any_value(report_period),
                any_value(report_period),
                greatest(
                    any_value(signal_available_at),
                    coalesce(max(next_available_at), any_value(signal_available_at))
                ),
                any_value(cusip),
                any_value(signal_available_at),
                count(*) FILTER (WHERE next_report_period IS NOT NULL)::INTEGER,
                count(*) FILTER (WHERE disclosed_exit)::INTEGER,
                count(*) FILTER (WHERE disclosed_exit)::DOUBLE
                    / nullif(count(*) FILTER (WHERE next_report_period IS NOT NULL), 0),
                count(*) FILTER (
                    WHERE disclosed_exit AND trading_days_to_followup <= 47
                )::INTEGER,
                median(trading_days_to_followup),
                ?
            FROM manager_outcomes
            GROUP BY signal_id
            """,
            [run_id],
        )

    count_rows = []
    for table in (
        "thirteenf_amendment_market_regimes",
        "thirteenf_consensus_amendment_signals",
        "thirteenf_consensus_signal_outcomes",
    ):
        row = con.execute(f"SELECT count(*) FROM {table} WHERE {target}").fetchone()
        if row is None:
            raise RuntimeError(f"Could not count refreshed {table} rows")
        count_rows.append(int(row[0]))
    regime_rows, signal_rows, outcome_rows = count_rows
    return ThirteenFSignalRefreshResult(
        regime_rows=regime_rows,
        signal_rows=signal_rows,
        outcome_rows=outcome_rows,
        elapsed_seconds=time.perf_counter() - started,
    )
