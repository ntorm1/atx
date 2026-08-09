"""Point-in-time standardized unexpected quarterly revenue.

The signal follows a seasonal random walk with drift. It subtracts the mean of
the prior eight year-over-year quarterly revenue changes from the current
seasonal change, then divides by their sample standard deviation.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import winsorize, zscore
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT standardized unexpected revenue v1"
FACTOR_ID = "earnings_standardized_unexpected_revenue"
FACTOR_NAME = "PIT standardized unexpected quarterly revenue"
FACTOR_FAMILY = "fundamental_earnings"

_OUTPUT_COLUMNS = [
    "factor_value_id",
    "factor_id",
    "factor_name",
    "family",
    "security_id",
    "symbol",
    "as_of_date",
    "raw_value",
    "value",
    "available_at",
    "input_ids_json",
    "input_lineage_json",
    "is_latest_revision",
    "run_id",
    "source",
]


@dataclass(frozen=True)
class RevenueSurpriseOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_market_cap_usd: float = 100_000_000.0
    minimum_adv21_usd: float = 1_000_000.0
    maximum_signal_age_days: int = 150
    history_observations: int = 8
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    maximum_absolute_surprise: float = 25.0
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_revenue_surprise_inputs(
    store: DuckDBStore,
    options: RevenueSurpriseOptions | None = None,
) -> pd.DataFrame:
    """Resolve first-filed seasonal revenue surprises at monthly decision dates."""

    options = options or RevenueSurpriseOptions()
    history = int(options.history_observations)
    if history < 4:
        raise ValueError("revenue-surprise history_observations must be at least four")
    date_predicates: list[str] = []
    params: list[object] = []
    if options.start_date is not None:
        date_predicates.append("trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        date_predicates.append("trade_date <= ?")
        params.append(options.end_date)
    date_sql = " AND " + " AND ".join(date_predicates) if date_predicates else ""

    sql = f"""
        WITH quarter_candidates AS (
            SELECT
                statement_point_id,
                security_id,
                symbol,
                period_start,
                period_end,
                as_of_date AS filed_date,
                available_at,
                accession_number,
                value AS revenue,
                row_number() OVER (
                    PARTITION BY security_id, period_end
                    ORDER BY available_at, revision_sequence, statement_point_id
                ) AS first_period_rank
            FROM fundamental_statement_points
            WHERE canonical_metric = 'revenue'
              AND unit_type = 'monetary'
              AND unit = 'USD'
              AND period_start IS NOT NULL
              AND period_end - period_start BETWEEN 70 AND 115
              AND value IS NOT NULL
              AND isfinite(value)
              AND available_at IS NOT NULL
              AND revision_sequence = 1
              AND form IN ('10-Q', '10-Q/A', '10-K', '10-K/A')
        ),
        seasonal_candidates AS (
            SELECT
                current.*,
                prior.statement_point_id AS prior_statement_point_id,
                prior.period_end AS prior_period_end,
                prior.available_at AS prior_available_at,
                prior.accession_number AS prior_accession_number,
                prior.revenue AS prior_revenue,
                row_number() OVER (
                    PARTITION BY current.statement_point_id
                    ORDER BY abs((current.period_end - prior.period_end) - 365),
                             prior.period_end DESC,
                             prior.available_at
                ) AS seasonal_rank
            FROM quarter_candidates current
            JOIN quarter_candidates prior
              ON prior.security_id = current.security_id
             AND prior.first_period_rank = 1
             AND current.period_end - prior.period_end BETWEEN 350 AND 380
             AND prior.available_at <= current.available_at
            WHERE current.first_period_rank = 1
        ),
        seasonal_changes AS (
            SELECT *, revenue - prior_revenue AS seasonal_revenue_change
            FROM seasonal_candidates
            WHERE seasonal_rank = 1
        ),
        standardized AS (
            SELECT
                *,
                count(seasonal_revenue_change) OVER history_window AS history_observations,
                avg(seasonal_revenue_change) OVER history_window AS historical_drift,
                stddev_samp(seasonal_revenue_change) OVER history_window AS historical_std
            FROM seasonal_changes
            WINDOW history_window AS (
                PARTITION BY security_id
                ORDER BY period_end, available_at, statement_point_id
                ROWS BETWEEN {history} PRECEDING AND 1 PRECEDING
            )
        ),
        signals AS (
            SELECT
                *,
                (seasonal_revenue_change - historical_drift) / nullif(historical_std, 0)
                    AS revenue_surprise
            FROM standardized
            WHERE history_observations = {history} AND historical_std > 0
        ),
        price_dedup AS (
            SELECT
                security_id,
                any_value(symbol) AS symbol,
                trade_date,
                arg_max("close", available_at) AS "close",
                arg_max(volume, available_at) AS volume,
                arg_max(split_factor, available_at) AS split_factor,
                max(available_at) AS price_available_at
            FROM equity_daily_bars
            WHERE security_id IN (SELECT DISTINCT security_id FROM signals)
              AND "close" > 0 AND trade_date IS NOT NULL AND available_at IS NOT NULL
            GROUP BY security_id, trade_date
        ),
        price_features AS (
            SELECT
                *,
                product(
                    CASE WHEN split_factor > 0
                               AND (split_factor <= 0.8 OR split_factor >= 1.25)
                         THEN split_factor ELSE 1.0 END
                ) OVER (
                    PARTITION BY security_id ORDER BY trade_date
                    ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
                ) AS split_index,
                avg("close" * volume) OVER (
                    PARTITION BY security_id ORDER BY trade_date
                    ROWS BETWEEN 20 PRECEDING AND CURRENT ROW
                ) AS adv21_usd,
                row_number() OVER (
                    PARTITION BY security_id, year(trade_date), month(trade_date)
                    ORDER BY trade_date DESC
                ) AS month_rank
            FROM price_dedup
        ),
        rebalances AS (
            SELECT * FROM price_features WHERE month_rank = 1 {date_sql}
        ),
        signals_at_rebalance AS (
            SELECT
                p.*,
                s.* EXCLUDE (security_id, symbol),
                row_number() OVER (
                    PARTITION BY p.security_id, p.trade_date
                    ORDER BY s.available_at DESC, s.period_end DESC, s.statement_point_id DESC
                ) AS signal_rank
            FROM rebalances p
            JOIN signals s
              ON s.security_id = p.security_id
             AND s.period_end <= p.trade_date
             AND s.available_at <= p.price_available_at
        ),
        share_observations AS (
            SELECT
                share_history_id, security_id, taxonomy, concept, effective_date,
                available_at AS share_available_at, share_count,
                accession_number AS share_accession_number, revision_sequence
            FROM shares_outstanding_history
            WHERE share_count_type = 'shares_outstanding' AND share_count > 0
              AND concept IN ('EntityCommonStockSharesOutstanding', 'CommonStockSharesOutstanding')
              AND effective_date IS NOT NULL AND available_at IS NOT NULL
        ),
        share_with_split_index AS (
            SELECT s.*, coalesce(p.split_index, 1.0) AS share_split_index
            FROM share_observations s
            ASOF LEFT JOIN price_features p
              ON s.security_id = p.security_id AND s.effective_date >= p.trade_date
        ),
        shares_at_rebalance AS (
            SELECT
                p.security_id, p.trade_date, s.* EXCLUDE (security_id),
                row_number() OVER (
                    PARTITION BY p.security_id, p.trade_date
                    ORDER BY s.effective_date DESC, s.share_available_at DESC,
                             CASE WHEN s.taxonomy = 'dei' THEN 0 ELSE 1 END,
                             s.revision_sequence DESC, s.share_history_id DESC
                ) AS share_rank
            FROM rebalances p
            JOIN share_with_split_index s
              ON s.security_id = p.security_id
             AND s.effective_date <= p.trade_date
             AND s.share_available_at <= p.price_available_at
        ),
        combined AS (
            SELECT
                e.* EXCLUDE (signal_rank),
                s.share_history_id, s.effective_date AS share_effective_date,
                s.share_available_at, s.share_count, s.share_split_index,
                s.share_count * s.share_split_index / nullif(e.split_index, 0)
                    AS decision_share_count
            FROM signals_at_rebalance e
            JOIN shares_at_rebalance s USING (security_id, trade_date)
            WHERE e.signal_rank = 1 AND s.share_rank = 1
        )
        SELECT *, "close" * decision_share_count AS market_cap_usd
        FROM combined
        WHERE "close" * decision_share_count >= ?
          AND adv21_usd >= ?
          AND trade_date - period_end <= ?
          AND abs(revenue_surprise) <= ?
        ORDER BY trade_date, security_id
    """
    return store.con.execute(
        sql,
        [
            *params,
            options.minimum_market_cap_usd,
            options.minimum_adv21_usd,
            options.maximum_signal_age_days,
            options.maximum_absolute_surprise,
        ],
    ).df()


def _lineage(row: pd.Series) -> str:
    return json_dumps(
        {
            "formula": "((revenue_q-revenue_q_minus_4)-mean_prior_8_seasonal_changes)/std_prior_8_seasonal_changes",
            "revision_policy": "first_filed_period_value_only",
            "currency_policy": "USD_only",
            "scale_note": "Raw revenue is used because within-security standardization is scale invariant and avoids share/split measurement noise.",
            "current": {
                "statement_point_id": row["statement_point_id"],
                "period_end": row["period_end"],
                "available_at": row["available_at"],
                "accession_number": row["accession_number"],
                "revenue": row["revenue"],
            },
            "seasonal_prior": {
                "statement_point_id": row["prior_statement_point_id"],
                "period_end": row["prior_period_end"],
                "available_at": row["prior_available_at"],
                "accession_number": row["prior_accession_number"],
                "revenue": row["prior_revenue"],
            },
            "standardization": {
                "seasonal_revenue_change": row["seasonal_revenue_change"],
                "history_observations": row["history_observations"],
                "historical_drift": row["historical_drift"],
                "historical_std": row["historical_std"],
            },
            "decision": {
                "trade_date": row["trade_date"],
                "price_available_at": row["price_available_at"],
                "market_cap_usd": row["market_cap_usd"],
                "adv21_usd": row["adv21_usd"],
            },
        }
    )


def compute_revenue_surprise_rows(
    inputs: pd.DataFrame,
    options: RevenueSurpriseOptions | None = None,
) -> pd.DataFrame:
    """Convert PIT revenue-surprise inputs into monthly cross-sectional scores."""

    options = options or RevenueSurpriseOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {"security_id", "symbol", "trade_date", "price_available_at", "revenue_surprise"}
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"revenue surprise inputs missing columns: {missing}")
    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["trade_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["price_available_at"], errors="coerce")
    rows["revenue_surprise"] = pd.to_numeric(rows["revenue_surprise"], errors="coerce")
    rows = rows.dropna(subset=["security_id", "as_of_date", "available_at", "revenue_surprise"])
    rows = rows[rows["revenue_surprise"].map(math.isfinite)].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["revenue_surprise"].astype(float)
    rows = winsorize(
        rows,
        value_column="raw_value",
        output_column="winsorized_value",
        partition_columns=("factor_id", "as_of_date"),
        limits=options.winsor_limit,
    )
    rows = zscore(
        rows,
        value_column="winsorized_value",
        output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    rows["input_ids_json"] = json_dumps(["metric:revenue_quarterly"])
    rows["input_lineage_json"] = rows.apply(_lineage, axis=1)
    rows["is_latest_revision"] = True
    rows["run_id"] = options.run_id
    rows["source"] = options.source
    rows["factor_value_id"] = [
        _factor_value_id(options.source, security_id, as_of_date)
        for security_id, as_of_date in zip(rows["security_id"], rows["as_of_date"], strict=True)
    ]
    return (
        rows[_OUTPUT_COLUMNS]
        .dropna(subset=["value"])
        .sort_values(["as_of_date", "security_id"], kind="stable")
        .reset_index(drop=True)
    )


def refresh_revenue_surprise_values(
    store: DuckDBStore,
    options: RevenueSurpriseOptions | None = None,
) -> int:
    """Materialize the monthly first-filed revenue-surprise factor."""

    options = options or RevenueSurpriseOptions()
    store.initialize()
    rows = compute_revenue_surprise_rows(load_revenue_surprise_inputs(store, options), options)
    predicates = ["source = ?", "factor_id = ?"]
    params: list[object] = [options.source, FACTOR_ID]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    with store.transaction():
        store.con.execute(f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(predicates)}", params)
        if not rows.empty:
            insert_frame(store, rows, "fundamental_factor_values", "revenue_surprise_values_insert")
    return len(rows)
