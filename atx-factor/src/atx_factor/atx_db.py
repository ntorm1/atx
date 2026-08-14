"""Read-only DuckDB adapter for governed atx-db factor panels."""

from __future__ import annotations

import datetime as dt
from collections.abc import Iterable
from pathlib import Path

import duckdb
import polars as pl


def load_factor_panel(
    db_path: str | Path,
    factor_ids: Iterable[str],
    *,
    horizon_days: int = 21,
    start_date: dt.date | None = None,
    end_date: dt.date | None = None,
    memory_limit: str = "2GB",
    threads: int = 2,
) -> pl.DataFrame:
    """Load governed signals and split-adjusted forward returns into canonical long form."""

    factors = tuple(dict.fromkeys(str(factor_id) for factor_id in factor_ids))
    if not factors:
        raise ValueError("factor_ids cannot be empty")
    if horizon_days < 1:
        raise ValueError("horizon_days must be positive")
    factor_placeholders = ",".join("?" for _ in factors)
    predicates: list[str] = []
    date_params: list[object] = []
    if start_date is not None:
        predicates.append("p.as_of_date >= ?")
        date_params.append(start_date)
    if end_date is not None:
        predicates.append("p.as_of_date <= ?")
        date_params.append(end_date)
    date_sql = f"WHERE {' AND '.join(predicates)}" if predicates else ""
    params: list[object] = [*factors, *factors, *date_params]
    connection = duckdb.connect(str(Path(db_path)), read_only=True)
    try:
        connection.execute(f"SET memory_limit='{memory_limit}'")
        connection.execute(f"SET threads={int(threads)}")
        result = connection.execute(
            f"""
            WITH raw_factor_values AS (
                SELECT
                    security_id,as_of_date,factor_id,value,available_at,
                    source_loaded_at,input_lineage_json
                FROM fundamental_factor_values
                WHERE is_latest_revision
                  AND value IS NOT NULL
                  AND factor_id IN ({factor_placeholders})
                UNION ALL
                SELECT
                    security_id,as_of_date,factor_id,value,available_at,
                    source_loaded_at,input_lineage_json
                FROM cross_domain_factor_values
                WHERE is_latest_revision
                  AND value IS NOT NULL
                  AND factor_id IN ({factor_placeholders})
            ),
            factor_values AS (
                SELECT
                    security_id,
                    greatest(as_of_date,cast(available_at AS DATE)) AS as_of_date,
                    factor_id,value,available_at,source_loaded_at,input_lineage_json
                FROM raw_factor_values
            ),
            scoped_factors AS (
                SELECT * FROM factor_values p
                {date_sql}
            ),
            universe_filtered AS (
                SELECT
                    f.*,
                    row_number() OVER (
                        PARTITION BY f.security_id,f.as_of_date,f.factor_id
                        ORDER BY u.valid_from DESC,u.available_at DESC NULLS LAST,
                                 u.source_loaded_at DESC NULLS LAST,u.source DESC
                    ) AS universe_rank
                FROM scoped_factors f
                JOIN universe_membership u
                  ON u.universe_id='us_common_equity_liquid_v1'
                 AND u.security_id=f.security_id
                 AND u.valid_from<=f.as_of_date
                 AND (u.valid_to IS NULL OR u.valid_to>=f.as_of_date)
                 AND u.as_of_date<=f.as_of_date
                 AND u.is_member
                 AND u.is_latest_revision
                 AND (u.available_at IS NULL OR cast(u.available_at AS DATE)<=f.as_of_date)
            ),
            ranked_factor_rows AS (
                SELECT
                    *,
                    row_number() OVER (
                        PARTITION BY security_id,as_of_date,factor_id
                        ORDER BY available_at DESC,source_loaded_at DESC,
                                 input_lineage_json DESC
                    ) AS factor_rank
                FROM universe_filtered
                WHERE universe_rank=1
                  AND cast(available_at AS DATE)<=as_of_date
            ),
            factor_rows AS (
                SELECT security_id,as_of_date,factor_id,value AS signal,available_at
                FROM ranked_factor_rows
                WHERE factor_rank=1
            ),
            factor_securities AS (
                SELECT DISTINCT security_id FROM factor_rows
            ),
            adjusted AS (
                SELECT
                    b.security_id,
                    b.trade_date AS date,
                    b.close * coalesce(
                        product(
                            CASE WHEN b.split_factor IS NOT NULL AND b.split_factor > 0
                                 THEN b.split_factor ELSE 1.0 END
                        ) OVER (
                            PARTITION BY b.security_id ORDER BY b.trade_date
                            ROWS BETWEEN 1 FOLLOWING AND UNBOUNDED FOLLOWING
                        ),1.0
                    ) AS adjusted_close,
                    avg(b.close*b.volume) OVER (
                        PARTITION BY b.security_id ORDER BY b.trade_date
                        ROWS BETWEEN 19 PRECEDING AND CURRENT ROW
                    ) AS adv_usd
                FROM equity_daily_bars b
                SEMI JOIN factor_securities s USING (security_id)
                WHERE b.close > 0
            ),
            forward AS (
                SELECT
                    security_id,
                    date,
                    lead(date,{horizon_days}) OVER (
                        PARTITION BY security_id ORDER BY date
                    ) AS forward_end_date,
                    lead(adjusted_close,{horizon_days}) OVER (
                        PARTITION BY security_id ORDER BY date
                    )/adjusted_close-1.0 AS forward_return,
                    adv_usd
                FROM adjusted
            )
            SELECT
                f.as_of_date AS date,
                f.security_id AS asset_id,
                f.factor_id AS signal_id,
                f.signal,
                r.forward_return,
                f.available_at,
                r.forward_end_date,
                r.adv_usd
            FROM factor_rows f
            JOIN forward r
              ON r.security_id=f.security_id AND r.date=f.as_of_date
            WHERE r.forward_return IS NOT NULL
              AND isfinite(r.forward_return)
              AND f.available_at < CAST(f.as_of_date AS TIMESTAMP)+INTERVAL '1 day'
            ORDER BY f.factor_id,f.as_of_date,f.security_id
            """,
            params,
        ).pl()
    finally:
        connection.close()
    if result.is_empty():
        raise ValueError("no governed factor/forward-return rows matched the request")
    return result


def select_signal(panel: pl.DataFrame, factor_id: str) -> pl.DataFrame:
    """Select one factor from a long multi-signal panel and drop its identifier."""

    if "signal_id" not in panel.columns:
        raise ValueError("multi-signal panel is missing signal_id")
    selected = panel.filter(pl.col("signal_id") == factor_id).drop("signal_id")
    if selected.is_empty():
        raise ValueError(f"factor {factor_id!r} is absent from panel")
    return selected
