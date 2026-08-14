"""Point-in-time amendment-chain reconstruction for SEC Form 13F filings.

The SEC publishes amendments in two materially different forms. A RESTATEMENT
replaces the information table, while an ADD NEW HOLDINGS amendment supplements
the latest full table. Raw filings therefore cannot be summed safely. This
module reconstructs each manager-quarter state and records position-level
changes without expanding every historical filing into a permanent snapshot.
"""

from __future__ import annotations

import datetime as dt
import logging
import time
from dataclasses import dataclass

from .connection import DuckDBStore

LOGGER = logging.getLogger(__name__)


@dataclass(frozen=True)
class ThirteenFAmendmentRefreshResult:
    effective_position_rows: int
    correction_rows: int
    manager_quarter_rows: int
    spike_rows: int
    elapsed_seconds: float


def ensure_thirteenf_amendment_schema(store: DuckDBStore) -> None:
    con = store.con
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS thirteenf_effective_positions (
            position_id VARCHAR PRIMARY KEY,
            manager_cik VARCHAR NOT NULL,
            report_period DATE NOT NULL,
            as_of_date DATE,
            position_key VARCHAR NOT NULL,
            security_id VARCHAR,
            cusip VARCHAR NOT NULL,
            name_of_issuer VARCHAR,
            title_of_class VARCHAR,
            share_quantity_type VARCHAR,
            put_call VARCHAR,
            investment_discretion VARCHAR,
            other_manager VARCHAR,
            value_usd DOUBLE,
            share_quantity DOUBLE,
            voting_auth_sole DOUBLE,
            voting_auth_shared DOUBLE,
            voting_auth_none DOUBLE,
            effective_accession VARCHAR NOT NULL,
            source_accessions VARCHAR NOT NULL,
            available_at TIMESTAMP NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS thirteenf_amendment_corrections (
            correction_id VARCHAR PRIMARY KEY,
            manager_cik VARCHAR NOT NULL,
            report_period DATE NOT NULL,
            as_of_date DATE,
            amendment_accession VARCHAR NOT NULL,
            prior_accession VARCHAR,
            amendment_sequence INTEGER NOT NULL,
            amendment_no INTEGER,
            amendment_type VARCHAR NOT NULL,
            filing_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            position_key VARCHAR NOT NULL,
            security_id VARCHAR,
            cusip VARCHAR NOT NULL,
            change_type VARCHAR NOT NULL,
            old_value_usd DOUBLE,
            new_value_usd DOUBLE,
            old_share_quantity DOUBLE,
            new_share_quantity DOUBLE,
            source_period VARCHAR NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    con.execute(
        """
        CREATE TABLE IF NOT EXISTS thirteenf_amendment_rates (
            rate_id VARCHAR PRIMARY KEY,
            manager_cik VARCHAR NOT NULL,
            report_period DATE NOT NULL,
            as_of_date DATE,
            available_at TIMESTAMP NOT NULL,
            filing_count INTEGER NOT NULL,
            amendment_count INTEGER NOT NULL,
            position_count INTEGER NOT NULL,
            corrected_position_count INTEGER NOT NULL,
            amendment_rate DOUBLE NOT NULL,
            trailing_history_quarters INTEGER NOT NULL,
            trailing_24q_mean DOUBLE,
            trailing_24q_stddev DOUBLE,
            amendment_rate_zscore DOUBLE,
            is_spike BOOLEAN NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for table in (
        "thirteenf_effective_positions",
        "thirteenf_amendment_corrections",
        "thirteenf_amendment_rates",
    ):
        con.execute(f"ALTER TABLE {table} ADD COLUMN IF NOT EXISTS as_of_date DATE")
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_13f_effective_manager_period ON thirteenf_effective_positions(manager_cik, report_period)",
        "CREATE INDEX IF NOT EXISTS idx_13f_effective_cusip_period ON thirteenf_effective_positions(cusip, report_period)",
        "CREATE INDEX IF NOT EXISTS idx_13f_correction_manager_period ON thirteenf_amendment_corrections(manager_cik, report_period)",
        "CREATE INDEX IF NOT EXISTS idx_13f_correction_cusip_period ON thirteenf_amendment_corrections(cusip, report_period)",
        "CREATE INDEX IF NOT EXISTS idx_13f_rate_manager_period ON thirteenf_amendment_rates(manager_cik, report_period)",
        "CREATE INDEX IF NOT EXISTS idx_13f_rate_spike_period ON thirteenf_amendment_rates(is_spike, report_period)",
    ):
        con.execute(statement)
    con.execute(
        """
        CREATE OR REPLACE VIEW v_thirteenf_amendment_spikes AS
        SELECT *
        FROM thirteenf_amendment_rates
        WHERE is_latest_revision AND is_spike
        """
    )


def _report_filter(alias: str, start: dt.date | None, end: dt.date | None) -> str:
    clauses: list[str] = []
    if start is not None:
        clauses.append(f"{alias}.period_of_report >= DATE '{start.isoformat()}'")
    if end is not None:
        clauses.append(f"{alias}.period_of_report <= DATE '{end.isoformat()}'")
    return " AND ".join(clauses) if clauses else "true"


def _target_filter(column: str, start: dt.date | None, end: dt.date | None) -> str:
    clauses: list[str] = []
    if start is not None:
        clauses.append(f"{column} >= DATE '{start.isoformat()}'")
    if end is not None:
        clauses.append(f"{column} <= DATE '{end.isoformat()}'")
    return " AND ".join(clauses) if clauses else "true"


def refresh_thirteenf_amendments(
    store: DuckDBStore,
    *,
    start: dt.date | None = None,
    end: dt.date | None = None,
    minimum_history_quarters: int = 24,
    spike_zscore: float = 2.0,
    zero_variance_zscore: float = 10.0,
    materialize_effective_positions: bool = True,
    run_id: str | None = None,
) -> ThirteenFAmendmentRefreshResult:
    """Rebuild effective positions, corrections, and manager-quarter rates.

    The date range is the 13F report period, not the filing date. Windowed
    z-scores always read the complete rate history already materialized in the
    target table, so incremental refreshes retain prior-quarter context.
    """
    if minimum_history_quarters < 2:
        raise ValueError("minimum_history_quarters must be at least 2")
    if spike_zscore <= 0:
        raise ValueError("spike_zscore must be positive")
    if zero_variance_zscore < spike_zscore:
        raise ValueError("zero_variance_zscore must be at least spike_zscore")

    started = time.perf_counter()
    stage_started = started

    def log_stage(stage: str) -> None:
        nonlocal stage_started
        now = time.perf_counter()
        LOGGER.info("completed 13F amendment stage %s in %.1fs", stage, now - stage_started)
        stage_started = now

    ensure_thirteenf_amendment_schema(store)
    con = store.con
    source_filter = _report_filter("s", start, end)
    target_filter = _target_filter("report_period", start, end)
    relevant_filing_filter = (
        "true"
        if materialize_effective_positions
        else """
            EXISTS (
                SELECT 1
                FROM _atx_13f_filings amendment
                WHERE amendment.manager_cik = f.manager_cik
                  AND amendment.report_period = f.report_period
                  AND amendment.is_amendment
            )
        """
    )
    holding_projection = (
        """
                any_value(h.security_id) AS security_id,
                upper(trim(h.cusip)) AS cusip,
                any_value(h.name_of_issuer) AS name_of_issuer,
                any_value(h.title_of_class) AS title_of_class,
                any_value(h.share_quantity_type) AS share_quantity_type,
                any_value(h.put_call) AS put_call,
                any_value(h.investment_discretion) AS investment_discretion,
                any_value(h.other_manager) AS other_manager,
                sum(h.value_usd) AS value_usd,
                sum(h.share_quantity) AS share_quantity,
                sum(h.voting_auth_sole) AS voting_auth_sole,
                sum(h.voting_auth_shared) AS voting_auth_shared,
                sum(h.voting_auth_none) AS voting_auth_none
        """
        if materialize_effective_positions
        else """
                any_value(h.security_id) AS security_id,
                upper(trim(h.cusip)) AS cusip,
                sum(h.value_usd) AS value_usd,
                sum(h.share_quantity) AS share_quantity
        """
    )

    with store.transaction():
        con.execute(
            f"""
            CREATE OR REPLACE TEMP TABLE _atx_13f_filings AS
            WITH normalized AS (
                SELECT
                    s.accession_number,
                    s.source_period,
                    lpad(trim(s.cik), 10, '0') AS manager_cik,
                    s.period_of_report AS report_period,
                    s.filing_date,
                    coalesce(
                        lower(trim(c.is_amendment)) IN ('true', '1', 'yes', 'y'),
                        upper(trim(s.submission_type)) LIKE '%/A'
                    ) AS is_amendment,
                    try_cast(nullif(trim(c.amendment_no), '') AS INTEGER) AS amendment_no,
                    upper(coalesce(nullif(trim(c.amendment_type), ''), '')) AS amendment_type
                FROM thirteenf_submissions s
                LEFT JOIN thirteenf_cover_pages c
                  ON c.accession_number = s.accession_number
                 AND c.source_period = s.source_period
                WHERE upper(trim(s.submission_type)) IN ('13F-HR', '13F-HR/A')
                  AND s.cik IS NOT NULL
                  AND s.period_of_report IS NOT NULL
                  AND s.filing_date IS NOT NULL
                  AND {source_filter}
            )
            SELECT
                *,
                CASE
                    WHEN NOT is_amendment THEN 'FULL'
                    WHEN amendment_type LIKE '%RESTATEMENT%' THEN 'FULL'
                    ELSE 'ADD'
                END AS filing_mode,
                row_number() OVER (
                    PARTITION BY manager_cik, report_period
                    ORDER BY filing_date, coalesce(amendment_no, 0), accession_number
                )::INTEGER AS filing_sequence,
                lag(accession_number) OVER (
                    PARTITION BY manager_cik, report_period
                    ORDER BY filing_date, coalesce(amendment_no, 0), accession_number
                ) AS prior_accession
            FROM normalized
            """
        )
        log_stage("filings")
        con.execute(
            f"""
            CREATE OR REPLACE TEMP TABLE _atx_13f_relevant_filings AS
            SELECT f.*
            FROM _atx_13f_filings f
            WHERE {relevant_filing_filter}
            """
        )
        log_stage("relevant-filings")
        con.execute(
            f"""
            CREATE OR REPLACE TEMP TABLE _atx_13f_holding_rows AS
            SELECT
                f.manager_cik,
                f.report_period,
                f.accession_number,
                f.source_period,
                f.filing_sequence,
                f.filing_mode,
                md5(concat_ws('|',
                    upper(coalesce(trim(h.cusip), '')),
                    upper(coalesce(trim(h.title_of_class), '')),
                    upper(coalesce(trim(h.share_quantity_type), '')),
                    upper(coalesce(trim(h.put_call), '')),
                    upper(coalesce(trim(h.investment_discretion), '')),
                    upper(coalesce(trim(h.other_manager), ''))
                )) AS position_key,
                {holding_projection}
            FROM _atx_13f_relevant_filings f
            JOIN thirteenf_holdings h
              ON h.accession_number = f.accession_number
             AND h.source_period = f.source_period
            WHERE nullif(trim(h.cusip), '') IS NOT NULL
            GROUP BY ALL
            """
        )
        log_stage("holding-rows")
        con.execute(
            """
            CREATE OR REPLACE TEMP TABLE _atx_13f_final_counts AS
            WITH cutoffs AS (
                SELECT manager_cik, report_period, max(filing_sequence) AS cutoff_sequence
                FROM _atx_13f_filings
                GROUP BY manager_cik, report_period
            ),
            bases AS (
                SELECT
                    c.*,
                    max(f.filing_sequence) FILTER (
                        WHERE f.filing_mode = 'FULL' AND f.filing_sequence <= c.cutoff_sequence
                    ) AS base_sequence,
                    max(f.filing_date) AS available_date
                FROM cutoffs c
                JOIN _atx_13f_filings f
                  ON f.manager_cik = c.manager_cik
                 AND f.report_period = c.report_period
                 AND f.filing_sequence <= c.cutoff_sequence
                GROUP BY c.manager_cik, c.report_period, c.cutoff_sequence
            )
            SELECT
                b.manager_cik,
                b.report_period,
                coalesce(sum(sp.table_entry_total), 0)::INTEGER AS position_count,
                cast(b.available_date AS TIMESTAMP) AS available_at
            FROM bases b
            JOIN _atx_13f_filings f
              ON f.manager_cik = b.manager_cik
             AND f.report_period = b.report_period
             AND (
                    f.filing_sequence = b.base_sequence
                    OR (
                        f.filing_mode = 'ADD'
                        AND f.filing_sequence > b.base_sequence
                        AND f.filing_sequence <= b.cutoff_sequence
                    )
                 )
            LEFT JOIN thirteenf_summary_pages sp
              ON sp.accession_number = f.accession_number
             AND sp.source_period = f.source_period
            WHERE b.base_sequence IS NOT NULL
            GROUP BY b.manager_cik, b.report_period, b.available_date
            """
        )
        log_stage("final-counts")
        if materialize_effective_positions:
            con.execute(
                """
                CREATE OR REPLACE TEMP TABLE _atx_13f_final_states AS
                WITH cutoffs AS (
                    SELECT manager_cik, report_period, max(filing_sequence) AS cutoff_sequence
                    FROM _atx_13f_filings
                    GROUP BY manager_cik, report_period
                ),
                bases AS (
                    SELECT
                        c.*,
                        max(f.filing_sequence) FILTER (
                            WHERE f.filing_mode = 'FULL' AND f.filing_sequence <= c.cutoff_sequence
                        ) AS base_sequence,
                        max(f.filing_date) AS available_date
                    FROM cutoffs c
                    JOIN _atx_13f_filings f
                      ON f.manager_cik = c.manager_cik
                     AND f.report_period = c.report_period
                     AND f.filing_sequence <= c.cutoff_sequence
                    GROUP BY c.manager_cik, c.report_period, c.cutoff_sequence
                )
                SELECT
                    b.manager_cik,
                    b.report_period,
                    h.position_key,
                    any_value(h.security_id) AS security_id,
                    any_value(h.cusip) AS cusip,
                    any_value(h.name_of_issuer) AS name_of_issuer,
                    any_value(h.title_of_class) AS title_of_class,
                    any_value(h.share_quantity_type) AS share_quantity_type,
                    any_value(h.put_call) AS put_call,
                    any_value(h.investment_discretion) AS investment_discretion,
                    any_value(h.other_manager) AS other_manager,
                    sum(h.value_usd) AS value_usd,
                    sum(h.share_quantity) AS share_quantity,
                    sum(h.voting_auth_sole) AS voting_auth_sole,
                    sum(h.voting_auth_shared) AS voting_auth_shared,
                    sum(h.voting_auth_none) AS voting_auth_none,
                    max_by(f.accession_number, f.filing_sequence) AS effective_accession,
                    string_agg(DISTINCT f.accession_number, ',' ORDER BY f.accession_number) AS source_accessions,
                    cast(b.available_date AS TIMESTAMP) AS available_at
                FROM bases b
                JOIN _atx_13f_filings f
                  ON f.manager_cik = b.manager_cik
                 AND f.report_period = b.report_period
                 AND (
                        f.filing_sequence = b.base_sequence
                        OR (
                            f.filing_mode = 'ADD'
                            AND f.filing_sequence > b.base_sequence
                            AND f.filing_sequence <= b.cutoff_sequence
                        )
                     )
                JOIN _atx_13f_holding_rows h
                  ON h.accession_number = f.accession_number
                 AND h.source_period = f.source_period
                WHERE b.base_sequence IS NOT NULL
                GROUP BY b.manager_cik, b.report_period, h.position_key, b.available_date
                """
            )
            log_stage("final-states")
        con.execute(
            """
            CREATE OR REPLACE TEMP TABLE _atx_13f_event_states AS
            WITH events AS (
                SELECT *
                FROM _atx_13f_filings
                WHERE is_amendment
            ),
            cutoffs AS (
                SELECT
                    e.manager_cik,
                    e.report_period,
                    e.accession_number AS amendment_accession,
                    e.prior_accession,
                    e.filing_sequence AS amendment_sequence,
                    e.amendment_no,
                    coalesce(nullif(e.amendment_type, ''), 'ADD NEW HOLDINGS') AS amendment_type,
                    e.filing_date,
                    e.source_period,
                    state_kind,
                    CASE WHEN state_kind = 'old' THEN e.filing_sequence - 1 ELSE e.filing_sequence END AS cutoff_sequence
                FROM events e
                CROSS JOIN (VALUES ('old'), ('new')) state(state_kind)
            ),
            bases AS (
                SELECT
                    c.*,
                    max(f.filing_sequence) FILTER (
                        WHERE f.filing_mode = 'FULL' AND f.filing_sequence <= c.cutoff_sequence
                    ) AS base_sequence
                FROM cutoffs c
                LEFT JOIN _atx_13f_filings f
                  ON f.manager_cik = c.manager_cik
                 AND f.report_period = c.report_period
                 AND f.filing_sequence <= c.cutoff_sequence
                GROUP BY ALL
            )
            SELECT
                b.manager_cik,
                b.report_period,
                b.amendment_accession,
                b.prior_accession,
                b.amendment_sequence,
                b.amendment_no,
                b.amendment_type,
                b.filing_date,
                b.source_period,
                b.state_kind,
                h.position_key,
                any_value(h.security_id) AS security_id,
                any_value(h.cusip) AS cusip,
                sum(h.value_usd) AS value_usd,
                sum(h.share_quantity) AS share_quantity
            FROM bases b
            JOIN _atx_13f_filings f
              ON f.manager_cik = b.manager_cik
             AND f.report_period = b.report_period
             AND (
                    f.filing_sequence = b.base_sequence
                    OR (
                        f.filing_mode = 'ADD'
                        AND f.filing_sequence > b.base_sequence
                        AND f.filing_sequence <= b.cutoff_sequence
                    )
                 )
            JOIN _atx_13f_holding_rows h
              ON h.accession_number = f.accession_number
             AND h.source_period = f.source_period
            WHERE b.base_sequence IS NOT NULL
            GROUP BY
                b.manager_cik, b.report_period, b.amendment_accession, b.prior_accession,
                b.amendment_sequence, b.amendment_no, b.amendment_type, b.filing_date,
                b.source_period, b.state_kind, h.position_key
            """
        )
        log_stage("event-states")

        for table in ("thirteenf_amendment_corrections", "thirteenf_amendment_rates"):
            con.execute(f"DELETE FROM {table} WHERE {target_filter}")
        # Never leave snapshots from a prior partial refresh looking current.
        # Fast analytical refreshes intentionally leave this optional serving
        # table empty for their target range.
        con.execute(f"DELETE FROM thirteenf_effective_positions WHERE {target_filter}")
        if materialize_effective_positions:
            con.execute(
                """
                INSERT INTO thirteenf_effective_positions (
                position_id, manager_cik, report_period, as_of_date, position_key, security_id,
                cusip, name_of_issuer, title_of_class, share_quantity_type, put_call,
                investment_discretion, other_manager, value_usd, share_quantity,
                voting_auth_sole, voting_auth_shared, voting_auth_none,
                effective_accession, source_accessions, available_at, run_id
            )
                SELECT
                md5(manager_cik || '|' || cast(report_period AS VARCHAR) || '|' || position_key),
                manager_cik, report_period, report_period, position_key, security_id, cusip,
                name_of_issuer, title_of_class, share_quantity_type, put_call,
                investment_discretion, other_manager, value_usd, share_quantity,
                voting_auth_sole, voting_auth_shared, voting_auth_none,
                effective_accession, source_accessions, available_at, ?
                FROM _atx_13f_final_states
                """,
                [run_id],
            )
        con.execute(
            """
            INSERT INTO thirteenf_amendment_corrections (
                correction_id, manager_cik, report_period, as_of_date, amendment_accession,
                prior_accession, amendment_sequence, amendment_no, amendment_type,
                filing_date, available_at, position_key, security_id, cusip,
                change_type, old_value_usd, new_value_usd, old_share_quantity,
                new_share_quantity, source_period, run_id
            )
            WITH old_state AS (
                SELECT * FROM _atx_13f_event_states WHERE state_kind = 'old'
            ),
            new_state AS (
                SELECT * FROM _atx_13f_event_states WHERE state_kind = 'new'
            ),
            paired AS (
                SELECT
                    coalesce(n.manager_cik, o.manager_cik) AS manager_cik,
                    coalesce(n.report_period, o.report_period) AS report_period,
                    coalesce(n.amendment_accession, o.amendment_accession) AS amendment_accession,
                    coalesce(n.prior_accession, o.prior_accession) AS prior_accession,
                    coalesce(n.amendment_sequence, o.amendment_sequence) AS amendment_sequence,
                    coalesce(n.amendment_no, o.amendment_no) AS amendment_no,
                    coalesce(n.amendment_type, o.amendment_type) AS amendment_type,
                    coalesce(n.filing_date, o.filing_date) AS filing_date,
                    coalesce(n.source_period, o.source_period) AS source_period,
                    coalesce(n.position_key, o.position_key) AS position_key,
                    coalesce(n.security_id, o.security_id) AS security_id,
                    coalesce(n.cusip, o.cusip) AS cusip,
                    o.value_usd AS old_value_usd,
                    n.value_usd AS new_value_usd,
                    o.share_quantity AS old_share_quantity,
                    n.share_quantity AS new_share_quantity
                FROM old_state o
                FULL OUTER JOIN new_state n
                  ON n.amendment_accession = o.amendment_accession
                 AND n.position_key = o.position_key
            )
            SELECT
                md5(manager_cik || '|' || cast(report_period AS VARCHAR) || '|' || amendment_accession || '|' || position_key),
                manager_cik, report_period, report_period, amendment_accession, prior_accession,
                amendment_sequence, amendment_no, amendment_type, filing_date,
                cast(filing_date AS TIMESTAMP), position_key, security_id, cusip,
                CASE
                    WHEN old_value_usd IS NULL AND old_share_quantity IS NULL THEN 'ADDED'
                    WHEN new_value_usd IS NULL AND new_share_quantity IS NULL THEN 'REMOVED'
                    ELSE 'CHANGED'
                END,
                old_value_usd, new_value_usd, old_share_quantity, new_share_quantity,
                source_period, ?
            FROM paired
            WHERE old_value_usd IS DISTINCT FROM new_value_usd
               OR old_share_quantity IS DISTINCT FROM new_share_quantity
            """,
            [run_id],
        )
        log_stage("corrections")
        con.execute(
            f"""
            INSERT INTO thirteenf_amendment_rates (
                rate_id, manager_cik, report_period, as_of_date, available_at, filing_count,
                amendment_count, position_count, corrected_position_count,
                amendment_rate, trailing_history_quarters, trailing_24q_mean,
                trailing_24q_stddev, amendment_rate_zscore, is_spike, run_id
            )
            WITH filing_stats AS (
                SELECT
                    manager_cik,
                    report_period,
                    max(cast(filing_date AS TIMESTAMP)) AS available_at,
                    count(*)::INTEGER AS filing_count,
                    count(*) FILTER (WHERE is_amendment)::INTEGER AS amendment_count
                FROM _atx_13f_filings
                GROUP BY manager_cik, report_period
            ),
            position_stats AS (
                SELECT manager_cik, report_period, max(position_count)::INTEGER AS position_count
                FROM _atx_13f_final_counts
                GROUP BY manager_cik, report_period
            ),
            correction_stats AS (
                SELECT
                    manager_cik,
                    report_period,
                    count(DISTINCT position_key)::INTEGER AS corrected_position_count
                FROM thirteenf_amendment_corrections
                WHERE {target_filter}
                GROUP BY manager_cik, report_period
            ),
            current_rates AS (
                SELECT
                    f.manager_cik,
                    f.report_period,
                    f.available_at,
                    f.filing_count,
                    f.amendment_count,
                    coalesce(p.position_count, 0) AS position_count,
                    coalesce(c.corrected_position_count, 0) AS corrected_position_count,
                    CASE
                        WHEN coalesce(p.position_count, 0) = 0 THEN 0.0
                        ELSE coalesce(c.corrected_position_count, 0)::DOUBLE / p.position_count
                    END AS amendment_rate
                FROM filing_stats f
                LEFT JOIN position_stats p USING (manager_cik, report_period)
                LEFT JOIN correction_stats c USING (manager_cik, report_period)
            ),
            all_rates AS (
                SELECT manager_cik, report_period, amendment_rate
                FROM thirteenf_amendment_rates
                WHERE NOT ({target_filter})
                UNION ALL
                SELECT manager_cik, report_period, amendment_rate
                FROM current_rates
            ),
            history AS (
                SELECT
                    manager_cik,
                    report_period,
                    (count(*) OVER window_24)::INTEGER AS trailing_history_quarters,
                    avg(amendment_rate) OVER window_24 AS trailing_mean,
                    stddev_samp(amendment_rate) OVER window_24 AS trailing_stddev
                FROM all_rates
                WINDOW window_24 AS (
                    PARTITION BY manager_cik ORDER BY report_period
                    ROWS BETWEEN 24 PRECEDING AND 1 PRECEDING
                )
            ),
            scored AS (
                SELECT
                    r.*,
                    h.trailing_history_quarters,
                    h.trailing_mean,
                    h.trailing_stddev,
                    CASE
                        WHEN h.trailing_history_quarters >= {minimum_history_quarters}
                         AND h.trailing_stddev > 0
                        THEN (r.amendment_rate - h.trailing_mean) / h.trailing_stddev
                        WHEN h.trailing_history_quarters >= {minimum_history_quarters}
                         AND coalesce(h.trailing_stddev, 0) = 0
                         AND r.amendment_rate > coalesce(h.trailing_mean, 0)
                        THEN {float(zero_variance_zscore)}
                    END AS zscore
                FROM current_rates r
                JOIN history h USING (manager_cik, report_period)
            )
            SELECT
                md5(manager_cik || '|' || cast(report_period AS VARCHAR)),
                manager_cik, report_period, report_period, available_at, filing_count,
                amendment_count, position_count, corrected_position_count,
                amendment_rate, trailing_history_quarters, trailing_mean,
                trailing_stddev, zscore, coalesce(zscore >= {float(spike_zscore)}, false), ?
            FROM scored
            """,
            [run_id],
        )
        log_stage("rates")

    if materialize_effective_positions:
        effective_row = con.execute(
            f"SELECT count(*) FROM thirteenf_effective_positions WHERE {target_filter}"
        ).fetchone()
        if effective_row is None:
            raise RuntimeError("Could not count effective 13F positions")
        effective_rows = int(effective_row[0])
    else:
        effective_rows = 0
    correction_row = con.execute(
        f"SELECT count(*) FROM thirteenf_amendment_corrections WHERE {target_filter}"
    ).fetchone()
    if correction_row is None:
        raise RuntimeError("Could not count 13F amendment corrections")
    correction_rows = int(correction_row[0])
    rate_count_row = con.execute(
        f"""
        SELECT count(*)::BIGINT, count(*) FILTER (WHERE is_spike)::BIGINT
        FROM thirteenf_amendment_rates
        WHERE {target_filter}
        """
    ).fetchone()
    if rate_count_row is None:
        raise RuntimeError("Could not count 13F amendment rates")
    manager_quarter_rows, spike_rows = rate_count_row
    return ThirteenFAmendmentRefreshResult(
        effective_position_rows=effective_rows,
        correction_rows=correction_rows,
        manager_quarter_rows=int(manager_quarter_rows),
        spike_rows=int(spike_rows),
        elapsed_seconds=time.perf_counter() - started,
    )
