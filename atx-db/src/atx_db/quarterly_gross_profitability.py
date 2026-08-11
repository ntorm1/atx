"""Point-in-time quarterly gross profits-to-lagged-assets factor."""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import winsorize, zscore
from .universe import DEFAULT_UNIVERSE_ID
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT quarterly gross profitability v1"
FACTOR_ID = "profitability_quarterly_gross_profitability_lagged_assets"
FACTOR_NAME = "PIT quarterly gross profits-to-lagged assets"
FACTOR_FAMILY = "fundamental_profitability"
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
class QuarterlyGrossProfitabilityOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    universe_id: str = DEFAULT_UNIVERSE_ID
    maximum_reporting_age_days: int = 200
    maximum_absolute_raw_value: float = 5.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def _date_filter(
    options: QuarterlyGrossProfitabilityOptions,
) -> tuple[str, list[object]]:
    predicates: list[str] = []
    params: list[object] = []
    if options.start_date is not None:
        predicates.append("trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("trade_date <= ?")
        params.append(options.end_date)
    return (" AND " + " AND ".join(predicates) if predicates else "", params)


def load_quarterly_gross_profitability_inputs(
    store: DuckDBStore,
    options: QuarterlyGrossProfitabilityOptions | None = None,
) -> pd.DataFrame:
    """Resolve the latest visible quarterly gross profit and prior-quarter assets."""

    options = options or QuarterlyGrossProfitabilityOptions()
    date_sql, date_params = _date_filter(options)
    sql = f"""
        WITH quarterly_components AS (
            SELECT
                security_id,
                any_value(symbol) AS fundamental_symbol,
                accession_number,
                period_start,
                period_end,
                arg_max(value, (available_at, revision_sequence, statement_point_id))
                    FILTER (WHERE canonical_metric = 'revenue') AS revenue,
                arg_max(statement_point_id,
                        (available_at, revision_sequence, statement_point_id))
                    FILTER (WHERE canonical_metric = 'revenue') AS revenue_id,
                max(available_at) FILTER (WHERE canonical_metric = 'revenue')
                    AS revenue_available_at,
                arg_max(value, (available_at, revision_sequence, statement_point_id))
                    FILTER (WHERE canonical_metric = 'cogs') AS cogs,
                arg_max(statement_point_id,
                        (available_at, revision_sequence, statement_point_id))
                    FILTER (WHERE canonical_metric = 'cogs') AS cogs_id,
                max(available_at) FILTER (WHERE canonical_metric = 'cogs')
                    AS cogs_available_at,
                arg_max(value, (available_at, revision_sequence, statement_point_id))
                    FILTER (WHERE canonical_metric = 'gross_profit') AS gross_profit,
                arg_max(statement_point_id,
                        (available_at, revision_sequence, statement_point_id))
                    FILTER (WHERE canonical_metric = 'gross_profit') AS gross_profit_id,
                max(available_at) FILTER (WHERE canonical_metric = 'gross_profit')
                    AS gross_profit_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric IN ('revenue', 'cogs', 'gross_profit')
              AND unit = 'USD'
              AND period_type = 'duration'
              AND period_start IS NOT NULL
              AND period_end IS NOT NULL
              AND date_diff('day', period_start, period_end) + 1 BETWEEN 70 AND 115
              AND value IS NOT NULL
              AND isfinite(value)
              AND accession_number IS NOT NULL
              AND form IN (
                  '10-Q', '10-Q/A', '10-QT',
                  '10-K', '10-K/A', '10-KT',
                  '20-F', '20-F/A', '40-F', '40-F/A',
                  '6-K', '6-K/A'
              )
            GROUP BY security_id, accession_number, period_start, period_end
        ),
        quarterly_gross_profit AS (
            SELECT
                *,
                CASE
                    WHEN revenue IS NOT NULL AND cogs IS NOT NULL
                        THEN revenue - cogs
                    ELSE gross_profit
                END AS resolved_gross_profit,
                CASE
                    WHEN revenue IS NOT NULL AND cogs IS NOT NULL
                        THEN 'revenue_minus_cogs'
                    ELSE 'reported_gross_profit'
                END AS gross_profit_method,
                CASE
                    WHEN revenue IS NOT NULL AND cogs IS NOT NULL
                        THEN greatest(revenue_available_at, cogs_available_at)
                    ELSE gross_profit_available_at
                END AS reporting_available_at
            FROM quarterly_components
            WHERE (revenue IS NOT NULL AND cogs IS NOT NULL)
               OR gross_profit IS NOT NULL
        ),
        quarterly_assets AS (
            SELECT
                security_id,
                accession_number,
                period_end,
                arg_max(value, (available_at, revision_sequence, statement_point_id))
                    AS total_assets,
                arg_max(statement_point_id,
                        (available_at, revision_sequence, statement_point_id))
                    AS total_assets_id,
                max(available_at) AS total_assets_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric = 'total_assets'
              AND unit = 'USD'
              AND period_type = 'instant'
              AND period_end IS NOT NULL
              AND value > 0
              AND isfinite(value)
              AND accession_number IS NOT NULL
              AND form IN (
                  '10-Q', '10-Q/A', '10-QT',
                  '10-K', '10-K/A', '10-KT',
                  '20-F', '20-F/A', '40-F', '40-F/A',
                  '6-K', '6-K/A'
              )
            GROUP BY security_id, accession_number, period_end
        ),
        price_dedup AS (
            SELECT
                security_id,
                any_value(symbol) AS symbol,
                trade_date,
                max(available_at) AS price_available_at
            FROM equity_daily_bars
            WHERE close > 0
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
        profit_candidates AS (
            SELECT
                d.*,
                q.* EXCLUDE (security_id, fundamental_symbol),
                row_number() OVER (
                    PARTITION BY d.security_id, d.trade_date
                    ORDER BY q.period_end DESC,
                             q.reporting_available_at DESC,
                             q.accession_number DESC
                ) AS profit_rank
            FROM governed_rebalances d
            JOIN quarterly_gross_profit q
              ON q.security_id = d.security_id
             AND q.period_end <= d.trade_date
             AND q.reporting_available_at <= d.price_available_at
             AND d.trade_date - q.period_end <= ?
            WHERE d.universe_rank = 1
        ),
        current_profit AS (
            SELECT * EXCLUDE (profit_rank)
            FROM profit_candidates
            WHERE profit_rank = 1
        ),
        lagged_asset_candidates AS (
            SELECT
                p.*,
                a.accession_number AS assets_accession_number,
                a.period_end AS assets_period_end,
                a.total_assets,
                a.total_assets_id,
                a.total_assets_available_at,
                row_number() OVER (
                    PARTITION BY p.security_id, p.trade_date
                    ORDER BY abs((p.period_end - a.period_end) - 91),
                             a.period_end DESC,
                             a.total_assets_available_at DESC,
                             a.accession_number DESC
                ) AS assets_rank
            FROM current_profit p
            JOIN quarterly_assets a
              ON a.security_id = p.security_id
             AND a.period_end < p.period_end
             AND p.period_end - a.period_end BETWEEN 60 AND 130
             AND a.total_assets_available_at <= p.price_available_at
        )
        SELECT
            * EXCLUDE (month_rank, universe_rank, assets_rank),
            greatest(
                price_available_at,
                coalesce(universe_available_at, price_available_at)
            ) AS decision_available_at
        FROM lagged_asset_candidates
        WHERE assets_rank = 1
        ORDER BY trade_date, security_id
    """
    return store.con.execute(
        sql,
        [*date_params, options.universe_id, options.maximum_reporting_age_days],
    ).df()


def _lineage(
    row: pd.Series,
    options: QuarterlyGrossProfitabilityOptions,
) -> str:
    return json_dumps(
        {
            "method": "hou_xue_zhang_quarterly_gross_profitability_pit",
            "formula": "(revenue-cogs)/one_quarter_lagged_total_assets",
            "orientation": "higher_is_more_profitable",
            "research_contract": {
                "quarter_duration_days": [70, 115],
                "lagged_assets_gap_days": [60, 130],
                "maximum_reporting_age_days": options.maximum_reporting_age_days,
                "maximum_absolute_raw_value": options.maximum_absolute_raw_value,
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "return_fitted_parameters": False,
            },
            "published_method_adaptations": {
                "timing": (
                    "Actual SEC availability replaces the paper's conservative four-month "
                    "fiscal-quarter delay."
                ),
                "gross_profit_fallback": (
                    "Reported gross profit is an algebraically identical fallback for "
                    "revenue minus COGS."
                ),
                "financial_firm_exclusion": (
                    "Not applied because historical point-in-time industry coverage is absent."
                ),
            },
            "decision": {
                "trade_date": row["trade_date"],
                "available_at": row["decision_available_at"],
                "universe_id": row["universe_id"],
                "universe_valid_from": row["universe_valid_from"],
                "universe_valid_to": row["universe_valid_to"],
                "universe_available_at": row["universe_available_at"],
                "universe_source": row["universe_source"],
            },
            "quarterly_statement": {
                "accession_number": row["accession_number"],
                "period_start": row["period_start"],
                "period_end": row["period_end"],
                "available_at": row["reporting_available_at"],
                "gross_profit_method": row["gross_profit_method"],
                "revenue": {"id": row.get("revenue_id"), "value": row.get("revenue")},
                "cogs": {"id": row.get("cogs_id"), "value": row.get("cogs")},
                "gross_profit": {
                    "id": row.get("gross_profit_id"),
                    "reported_value": row.get("gross_profit"),
                    "resolved_value": row["resolved_gross_profit"],
                },
            },
            "lagged_assets": {
                "statement_point_id": row["total_assets_id"],
                "accession_number": row["assets_accession_number"],
                "period_end": row["assets_period_end"],
                "value": row["total_assets"],
                "available_at": row["total_assets_available_at"],
            },
            "quarterly_gross_profitability": row["raw_value"],
        }
    )


def compute_quarterly_gross_profitability_rows(
    inputs: pd.DataFrame,
    options: QuarterlyGrossProfitabilityOptions | None = None,
) -> pd.DataFrame:
    """Compute, guard, winsorize, and standardize quarterly gross profitability."""

    options = options or QuarterlyGrossProfitabilityOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id",
        "symbol",
        "trade_date",
        "decision_available_at",
        "resolved_gross_profit",
        "total_assets",
        "universe_id",
        "universe_valid_from",
        "universe_valid_to",
        "universe_available_at",
        "universe_source",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"quarterly gross profitability inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["trade_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(
        rows["decision_available_at"], errors="coerce"
    )
    rows["resolved_gross_profit"] = pd.to_numeric(
        rows["resolved_gross_profit"], errors="coerce"
    )
    rows["total_assets"] = pd.to_numeric(rows["total_assets"], errors="coerce")
    rows = rows.dropna(
        subset=[
            "security_id",
            "as_of_date",
            "available_at",
            "resolved_gross_profit",
            "total_assets",
        ]
    )
    rows = rows[
        (rows["total_assets"] > 0)
        & rows["resolved_gross_profit"].map(math.isfinite)
        & rows["total_assets"].map(math.isfinite)
    ].copy()
    rows["quarterly_gross_profitability"] = (
        rows["resolved_gross_profit"] / rows["total_assets"]
    )
    rows = rows[rows["quarterly_gross_profitability"].map(math.isfinite)].copy()
    rows = rows[
        rows["quarterly_gross_profitability"].abs()
        <= options.maximum_absolute_raw_value
    ].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["quarterly_gross_profitability"]
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
    rows["input_ids_json"] = json_dumps(
        [
            "metric:revenue",
            "metric:cogs",
            "metric:gross_profit",
            "metric:total_assets",
            f"universe:{options.universe_id}",
        ]
    )
    rows["input_lineage_json"] = rows.apply(
        lambda row: _lineage(row, options), axis=1
    )
    rows["is_latest_revision"] = True
    rows["run_id"] = options.run_id
    rows["source"] = options.source
    rows["factor_value_id"] = [
        _factor_value_id(options.source, security_id, as_of_date)
        for security_id, as_of_date in zip(
            rows["security_id"], rows["as_of_date"], strict=True
        )
    ]
    return (
        rows[_OUTPUT_COLUMNS]
        .dropna(subset=["value"])
        .sort_values(["as_of_date", "security_id"], kind="stable")
        .reset_index(drop=True)
    )


def refresh_quarterly_gross_profitability_values(
    store: DuckDBStore,
    options: QuarterlyGrossProfitabilityOptions | None = None,
) -> int:
    """Materialize point-in-time quarterly gross profitability."""

    options = options or QuarterlyGrossProfitabilityOptions()
    store.initialize()
    rows = compute_quarterly_gross_profitability_rows(
        load_quarterly_gross_profitability_inputs(store, options), options
    )
    predicates = ["source = ?", "factor_id = ?"]
    params: list[object] = [options.source, FACTOR_ID]
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
                "quarterly_gross_profitability_insert",
            )
    return len(rows)
