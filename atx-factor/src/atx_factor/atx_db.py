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
    predicates = [f"p.factor_id IN ({','.join('?' for _ in factors)})"]
    params: list[object] = list(factors)
    if start_date is not None:
        predicates.append("p.as_of_date >= ?")
        params.append(start_date)
    if end_date is not None:
        predicates.append("p.as_of_date <= ?")
        params.append(end_date)
    connection = duckdb.connect(str(Path(db_path)), read_only=True)
    try:
        connection.execute(f"SET memory_limit='{memory_limit}'")
        connection.execute(f"SET threads={int(threads)}")
        result = connection.execute(
            f"""
            WITH factor_rows AS (
                SELECT
                    p.security_id,
                    p.as_of_date,
                    p.factor_id,
                    p.value AS signal,
                    p.available_at
                FROM v_factor_panel p
                WHERE {' AND '.join(predicates)}
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
