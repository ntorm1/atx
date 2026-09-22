"""FC1 conservative SEC date policy on existing PIT readers and metadata."""

from __future__ import annotations

import duckdb

from .._fundamental_clock import EFFECTIVE_FUNDAMENTAL_POINTS_SQL, FUNDAMENTAL_CLOCK_POLICY
from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _fundamental_effective_clock(conn: duckdb.DuckDBPyConnection) -> None:
    # Same view columns, grain, membership predicates, and aggregation as 0142;
    # only the raw fundamental relation changes. No fact UPDATE or source replay.
    conn.execute(f"""
        CREATE OR REPLACE VIEW v_price_fundamental_overlap AS
        WITH price_days AS (
            SELECT
                security_id,
                max(symbol) AS symbol,
                trade_date,
                max(available_at) AS price_available_at,
                count(*) AS price_row_count
            FROM equity_daily_bars
            WHERE security_id IS NOT NULL
              AND trade_date IS NOT NULL
              AND close IS NOT NULL
              AND close > 0
            GROUP BY security_id, trade_date
        ),
        member_price_days AS (
            SELECT
                u.universe_id,
                p.security_id,
                coalesce(u.symbol, p.symbol) AS symbol,
                p.trade_date,
                p.price_available_at,
                p.price_row_count
            FROM price_days p
            JOIN universe_membership u
              ON u.security_id = p.security_id
             AND u.valid_from <= p.trade_date
             AND (u.valid_to IS NULL OR u.valid_to >= p.trade_date)
             AND u.as_of_date <= p.trade_date
             AND u.is_member
             AND u.is_latest_revision
             AND (
                 u.available_at IS NULL
                 OR p.price_available_at IS NULL
                 OR u.available_at <= p.price_available_at
             )
        ),
        overlap_days AS (
            SELECT
                mp.*,
                EXISTS (
                    SELECT 1
                    FROM {EFFECTIVE_FUNDAMENTAL_POINTS_SQL} f
                    WHERE f.security_id = mp.security_id
                      AND f.period_end IS NOT NULL
                      AND f.period_end <= mp.trade_date
                      AND f.value IS NOT NULL
                      AND (
                          f.available_at IS NULL
                          OR mp.price_available_at IS NULL
                          OR f.available_at <= mp.price_available_at
                      )
                ) AS has_visible_fundamental
            FROM member_price_days mp
        )
        SELECT
            universe_id,
            CAST(date_trunc('month', trade_date) AS DATE) AS overlap_month,
            min(trade_date) AS first_trade_date,
            max(trade_date) AS last_trade_date,
            count(*) AS universe_price_days,
            sum(CASE WHEN has_visible_fundamental THEN 1 ELSE 0 END) AS price_fundamental_days,
            count(DISTINCT security_id) AS universe_priced_security_count,
            count(DISTINCT CASE WHEN has_visible_fundamental THEN security_id ELSE NULL END)
                AS overlapped_security_count,
            sum(price_row_count) AS price_row_count,
            CASE
                WHEN count(*) = 0 THEN NULL
                ELSE CAST(sum(CASE WHEN has_visible_fundamental THEN 1 ELSE 0 END) AS DOUBLE)
                    / CAST(count(*) AS DOUBLE)
            END AS overlap_ratio
        FROM overlap_days
        GROUP BY universe_id, CAST(date_trunc('month', trade_date) AS DATE)
        ORDER BY universe_id, overlap_month
    """)
    policy_note = (
        f"FC1 {FUNDAMENTAL_CLOCK_POLICY}: effective SEC companyfacts eligibility is "
        "max(stored available_at, filed date + 46 hours); unresolved/null/non-finite "
        "filed dates are excluded from effective readers. Non-SEC clocks are unchanged. "
        "This is a conservative date policy, not exact acceptance or delivery. "
        "Raw clocks and receipts remain loader provenance; existing materializations "
        "require a full dependent rebuild. Filing-date corrections, source arrival, "
        "identity assignment and observation vintages remain unresolved limits."
    )
    # Append rather than replace the P1/AF1 state and arithmetic descriptions.
    for table in (
        "sec_company_facts", "fundamental_points", "fundamental_fact_revisions",
        "fundamental_statement_points", "fundamental_periods", "fundamental_ttm_points",
        "fundamental_calendar_map", "fundamental_calendar_ttm", "fundamental_standardized",
        "derived_metric_values", "shares_outstanding_history", "market_daily_metrics",
        "fundamental_xbrl_metric", "est_actual",
        "v_price_fundamental_overlap",
    ):
        conn.execute("""
            UPDATE table_catalog
            SET pit_notes=concat_ws(' ', nullif(pit_notes, ''), ?), updated_at=now()
            WHERE table_name=? AND strpos(coalesce(pit_notes, ''), ?) = 0
        """, [policy_note, table, FUNDAMENTAL_CLOCK_POLICY])
    for table in ("sec_company_facts", "fundamental_points"):
        conn.execute("""
            UPDATE field_catalog
            SET description=concat_ws(' ', nullif(description, ''), ?), updated_at=now()
            WHERE table_name=? AND field_name='available_at'
              AND strpos(coalesce(description, ''), ?) = 0
        """, [
            f"FC1 {FUNDAMENTAL_CLOCK_POLICY} leaves this raw stored clock unchanged; "
            "PIT consumers project effective eligibility using the source filing date.",
            table, FUNDAMENTAL_CLOCK_POLICY,
        ])
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=319, name="fundamental_effective_clock", up=_fundamental_effective_clock)]
