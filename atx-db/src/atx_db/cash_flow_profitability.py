"""Point-in-time TTM cash-flow profitability and total-accrual quality.

Both features use the latest trailing-twelve-month observations visible at a
governed monthly universe decision and scale by average total assets.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any, cast

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import winsorize, zscore
from .universe import DEFAULT_UNIVERSE_ID
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT cash-flow profitability v1"
FACTOR_IDS = (
    "profitability_operating_cash_flow_to_assets",
    "quality_low_total_accruals",
)

FACTOR_METADATA: dict[str, dict[str, object]] = {
    "profitability_operating_cash_flow_to_assets": {
        "factor_name": "PIT operating cash-flow profitability",
        "family": "fundamental_profitability",
        "raw_column": "cash_flow_profitability",
        "formula": "operating_cash_flow_ttm / average_total_assets",
    },
    "quality_low_total_accruals": {
        "factor_name": "PIT low total accruals",
        "family": "fundamental_quality",
        "raw_column": "low_total_accruals",
        "formula": "(operating_cash_flow_ttm - net_income_ttm) / average_total_assets",
    },
}

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
class CashFlowProfitabilityOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    universe_id: str = DEFAULT_UNIVERSE_ID
    maximum_fundamental_age_days: int = 550
    maximum_absolute_accruals: float = 10.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(
    source: str,
    factor_id: str,
    security_id: str,
    as_of_date: Any,
) -> str:
    payload = "|".join(str(part) for part in (source, factor_id, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def _date_filter(options: CashFlowProfitabilityOptions) -> tuple[str, list[object]]:
    predicates: list[str] = []
    params: list[object] = []
    if options.start_date is not None:
        predicates.append("trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("trade_date <= ?")
        params.append(options.end_date)
    return (" AND " + " AND ".join(predicates) if predicates else "", params)


def load_cash_flow_profitability_inputs(
    store: DuckDBStore,
    options: CashFlowProfitabilityOptions | None = None,
) -> pd.DataFrame:
    """Assemble visible TTM cash flow, income, and average assets each month."""

    options = options or CashFlowProfitabilityOptions()
    date_sql, date_params = _date_filter(options)
    sql = f"""
        WITH eligible_securities AS (
            SELECT security_id
            FROM fundamental_ttm_points
            WHERE canonical_metric IN ('net_income', 'operating_cash_flow')
              AND unit = 'USD'
            GROUP BY security_id
            HAVING count(DISTINCT canonical_metric) = 2
        ),
        price_dedup AS (
            SELECT
                security_id,
                any_value(symbol) AS symbol,
                trade_date,
                max(available_at) AS price_available_at
            FROM equity_daily_bars
            WHERE security_id IN (SELECT security_id FROM eligible_securities)
              AND "close" > 0
              AND trade_date IS NOT NULL
              AND available_at IS NOT NULL
            GROUP BY security_id, trade_date
        ),
        price_months AS (
            SELECT
                *,
                row_number() OVER (
                    PARTITION BY security_id, year(trade_date), month(trade_date)
                    ORDER BY trade_date DESC
                ) AS month_rank
            FROM price_dedup
        ),
        rebalances AS (
            SELECT * FROM price_months
            WHERE month_rank = 1 {date_sql}
        ),
        governed_rebalances AS (
            SELECT
                p.*,
                u.valid_from AS universe_valid_from,
                u.valid_to AS universe_valid_to,
                u.universe_id,
                u.available_at AS universe_available_at,
                u.source AS universe_source,
                u.rules_json AS universe_rules_json,
                row_number() OVER (
                    PARTITION BY p.security_id, p.trade_date
                    ORDER BY u.valid_from DESC,
                             u.available_at DESC NULLS LAST,
                             u.source_loaded_at DESC,
                             u.source DESC
                ) AS universe_rank
            FROM rebalances p
            JOIN universe_membership u
              ON u.universe_id = ?
             AND u.security_id = p.security_id
             AND u.valid_from <= p.trade_date
             AND (u.valid_to IS NULL OR u.valid_to >= p.trade_date)
             AND u.as_of_date <= p.trade_date
             AND u.is_member
             AND u.is_latest_revision
             AND (u.available_at IS NULL OR u.available_at <= p.price_available_at)
        ),
        ttm_by_end AS (
            SELECT
                d.security_id,
                d.symbol,
                d.trade_date,
                d.price_available_at,
                d.universe_valid_from,
                d.universe_valid_to,
                d.universe_id,
                d.universe_available_at,
                d.universe_source,
                d.universe_rules_json,
                t.ttm_end_date,
                arg_max(
                    t.ttm_value,
                    (t.available_at, t.revision_sequence, t.ttm_point_id)
                ) FILTER (WHERE t.canonical_metric = 'net_income') AS net_income_ttm,
                arg_max(
                    t.ttm_point_id,
                    (t.available_at, t.revision_sequence, t.ttm_point_id)
                ) FILTER (WHERE t.canonical_metric = 'net_income') AS net_income_ttm_id,
                max(t.available_at) FILTER (WHERE t.canonical_metric = 'net_income')
                    AS net_income_available_at,
                arg_max(
                    t.ttm_value,
                    (t.available_at, t.revision_sequence, t.ttm_point_id)
                ) FILTER (WHERE t.canonical_metric = 'operating_cash_flow')
                    AS operating_cash_flow_ttm,
                arg_max(
                    t.ttm_point_id,
                    (t.available_at, t.revision_sequence, t.ttm_point_id)
                ) FILTER (WHERE t.canonical_metric = 'operating_cash_flow')
                    AS operating_cash_flow_ttm_id,
                max(t.available_at) FILTER (WHERE t.canonical_metric = 'operating_cash_flow')
                    AS operating_cash_flow_available_at
            FROM governed_rebalances d
            JOIN fundamental_ttm_points t
              ON t.security_id = d.security_id
             AND t.canonical_metric IN ('net_income', 'operating_cash_flow')
             AND t.unit = 'USD'
             AND t.ttm_end_date <= d.trade_date
             AND t.available_at <= d.price_available_at
             AND d.trade_date - t.ttm_end_date <= ?
            WHERE d.universe_rank = 1
            GROUP BY
                d.security_id,
                d.symbol,
                d.trade_date,
                d.price_available_at,
                d.universe_valid_from,
                d.universe_valid_to,
                d.universe_id,
                d.universe_available_at,
                d.universe_source,
                d.universe_rules_json,
                t.ttm_end_date
        ),
        latest_ttm AS (
            SELECT
                *,
                row_number() OVER (
                    PARTITION BY security_id, trade_date
                    ORDER BY ttm_end_date DESC,
                             greatest(
                                 net_income_available_at,
                                 operating_cash_flow_available_at
                             ) DESC
                ) AS ttm_rank
            FROM ttm_by_end
            WHERE net_income_ttm IS NOT NULL
              AND operating_cash_flow_ttm IS NOT NULL
        ),
        current_assets AS (
            SELECT
                t.*,
                a.statement_point_id AS asset_id,
                a.period_end AS asset_period_end,
                a.value AS assets,
                a.available_at AS asset_available_at,
                row_number() OVER (
                    PARTITION BY t.security_id, t.trade_date
                    ORDER BY abs(a.period_end - t.ttm_end_date),
                             a.available_at DESC,
                             a.revision_sequence DESC,
                             a.statement_point_id DESC
                ) AS asset_rank
            FROM latest_ttm t
            JOIN fundamental_statement_points a
              ON a.security_id = t.security_id
             AND a.canonical_metric = 'total_assets'
             AND a.unit = 'USD'
             AND a.value > 0
             AND isfinite(a.value)
             AND a.period_end BETWEEN t.ttm_end_date - 31 AND t.ttm_end_date + 31
             AND a.available_at <= t.price_available_at
            WHERE t.ttm_rank = 1
        ),
        prior_assets AS (
            SELECT
                c.*,
                p.statement_point_id AS prior_asset_id,
                p.period_end AS prior_asset_period_end,
                p.value AS prior_assets,
                p.available_at AS prior_asset_available_at,
                row_number() OVER (
                    PARTITION BY c.security_id, c.trade_date
                    ORDER BY abs((c.asset_period_end - p.period_end) - 365),
                             p.period_end DESC,
                             p.available_at DESC,
                             p.revision_sequence DESC,
                             p.statement_point_id DESC
                ) AS prior_asset_rank
            FROM current_assets c
            JOIN fundamental_statement_points p
              ON p.security_id = c.security_id
             AND p.canonical_metric = 'total_assets'
             AND p.unit = 'USD'
             AND p.value > 0
             AND isfinite(p.value)
             AND c.asset_period_end - p.period_end BETWEEN 300 AND 430
             AND p.available_at <= c.price_available_at
            WHERE c.asset_rank = 1
        ),
        calculated AS (
            SELECT
                *,
                (assets + prior_assets) / 2.0 AS average_total_assets,
                operating_cash_flow_ttm / ((assets + prior_assets) / 2.0)
                    AS cash_flow_profitability,
                (operating_cash_flow_ttm - net_income_ttm)
                    / ((assets + prior_assets) / 2.0) AS low_total_accruals
            FROM prior_assets
            WHERE prior_asset_rank = 1
        )
        SELECT * FROM calculated
        WHERE abs(low_total_accruals) <= ?
          AND isfinite(cash_flow_profitability)
          AND isfinite(low_total_accruals)
        ORDER BY trade_date, security_id
    """
    return store.con.execute(
        sql,
        [
            *date_params,
            options.universe_id,
            options.maximum_fundamental_age_days,
            options.maximum_absolute_accruals,
        ],
    ).df()


def _lineage(row: pd.Series, factor_id: str) -> str:
    metadata = FACTOR_METADATA[factor_id]
    return json_dumps(
        {
            "method": "ttm_cash_flow_statement_accrual_quality",
            "factor_id": factor_id,
            "formula": metadata["formula"],
            "decision": {
                "trade_date": row["trade_date"],
                "available_at": row["price_available_at"],
                "universe_id": row["universe_id"],
                "universe_valid_from": row["universe_valid_from"],
                "universe_valid_to": row["universe_valid_to"],
                "universe_available_at": row["universe_available_at"],
                "universe_source": row["universe_source"],
            },
            "ttm": {
                "period_end": row["ttm_end_date"],
                "net_income_ttm_id": row["net_income_ttm_id"],
                "net_income_ttm": row["net_income_ttm"],
                "net_income_available_at": row["net_income_available_at"],
                "operating_cash_flow_ttm_id": row["operating_cash_flow_ttm_id"],
                "operating_cash_flow_ttm": row["operating_cash_flow_ttm"],
                "operating_cash_flow_available_at": row["operating_cash_flow_available_at"],
            },
            "assets": {
                "current_id": row["asset_id"],
                "current_period_end": row["asset_period_end"],
                "current_value": row["assets"],
                "current_available_at": row["asset_available_at"],
                "prior_id": row["prior_asset_id"],
                "prior_period_end": row["prior_asset_period_end"],
                "prior_value": row["prior_assets"],
                "prior_available_at": row["prior_asset_available_at"],
                "average_total_assets": row["average_total_assets"],
            },
        }
    )


def compute_cash_flow_profitability_rows(
    inputs: pd.DataFrame,
    options: CashFlowProfitabilityOptions | None = None,
) -> pd.DataFrame:
    """Winsorize and z-score cash-flow profitability and low total accruals."""

    options = options or CashFlowProfitabilityOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id",
        "symbol",
        "trade_date",
        "price_available_at",
        "cash_flow_profitability",
        "low_total_accruals",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"cash-flow profitability inputs missing columns: {missing}")
    base = inputs.copy()
    base["as_of_date"] = pd.to_datetime(base["trade_date"], errors="coerce").dt.date
    base["available_at"] = pd.to_datetime(base["price_available_at"], errors="coerce")
    parts: list[pd.DataFrame] = []
    for factor_id in FACTOR_IDS:
        metadata = FACTOR_METADATA[factor_id]
        raw_column = cast(str, metadata["raw_column"])
        part = base.copy()
        part[raw_column] = pd.to_numeric(part[raw_column], errors="coerce")
        part = part.dropna(subset=["security_id", "as_of_date", "available_at", raw_column])
        part = part[part[raw_column].map(math.isfinite)].copy()
        counts = part.groupby("as_of_date")["security_id"].transform("nunique")
        part = part[counts >= options.minimum_names_per_date].copy()
        if part.empty:
            continue
        part["factor_id"] = factor_id
        part["factor_name"] = cast(str, metadata["factor_name"])
        part["family"] = cast(str, metadata["family"])
        part["raw_value"] = part[raw_column].astype(float)
        part = winsorize(
            part,
            value_column="raw_value",
            output_column="winsorized_value",
            partition_columns=("factor_id", "as_of_date"),
            limits=options.winsor_limit,
        )
        part = zscore(
            part,
            value_column="winsorized_value",
            output_column="value",
            partition_columns=("factor_id", "as_of_date"),
        )
        part["input_ids_json"] = json_dumps(
            [
                "metric:net_income_ttm",
                "metric:operating_cash_flow_ttm",
                "metric:total_assets",
                f"universe:{options.universe_id}",
            ]
        )
        part["input_lineage_json"] = part.apply(
            lambda row, selected_factor_id=factor_id: _lineage(row, selected_factor_id),
            axis=1,
        )
        part["is_latest_revision"] = True
        part["run_id"] = options.run_id
        part["source"] = options.source
        part["factor_value_id"] = [
            _factor_value_id(options.source, factor_id, security_id, as_of_date)
            for security_id, as_of_date in zip(part["security_id"], part["as_of_date"], strict=True)
        ]
        parts.append(part[_OUTPUT_COLUMNS])
    if not parts:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    return (
        pd.concat(parts, ignore_index=True)
        .dropna(subset=["value"])
        .sort_values(["factor_id", "as_of_date", "security_id"], kind="stable")
        .reset_index(drop=True)
    )


def refresh_cash_flow_profitability_values(
    store: DuckDBStore,
    options: CashFlowProfitabilityOptions | None = None,
) -> int:
    """Materialize monthly cash-flow profitability and low total accruals."""

    options = options or CashFlowProfitabilityOptions()
    store.initialize()
    rows = compute_cash_flow_profitability_rows(load_cash_flow_profitability_inputs(store, options), options)
    predicates = ["source = ?", "factor_id IN (?, ?)"]
    params: list[object] = [options.source, *FACTOR_IDS]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    with store.transaction():
        store.con.execute(
            f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(predicates)}",
            params,
        )
        if not rows.empty:
            insert_frame(
                store,
                rows,
                "fundamental_factor_values",
                "cash_flow_profitability_insert",
            )
    return len(rows)
