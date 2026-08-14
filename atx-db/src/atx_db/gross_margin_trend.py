"""Point-in-time eight-observation trend in quarterly gross margin."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import winsorize, zscore
from .profitability_trend import FACTOR_ID as TREND_GRID_FACTOR_ID
from .profitability_trend import SOURCE_NAME as TREND_GRID_SOURCE_NAME
from .quarterly_gross_profitability import FACTOR_ID as QGP_FACTOR_ID
from .quarterly_gross_profitability import SOURCE_NAME as QGP_SOURCE_NAME
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT eight-observation quarterly gross-margin trend v1"
FACTOR_ID = "profitability_quarterly_gross_margin_trend_8q"
FACTOR_NAME = "PIT eight-observation quarterly gross-margin trend"
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
class GrossMarginTrendOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_observations: int = 8
    minimum_seasonal_quarters: int = 3
    maximum_absolute_gross_margin: float = 5.0
    maximum_absolute_trend: float = 1.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_gross_margin_trend_inputs(
    store: DuckDBStore,
    options: GrossMarginTrendOptions | None = None,
) -> pd.DataFrame:
    """Estimate an elapsed-time gross-margin slope on the governed trend grid."""

    options = options or GrossMarginTrendOptions()
    if options.minimum_observations != 8:
        raise ValueError("gross-margin trend currently requires exactly eight observations")
    predicates = ["factor_id = ?", "source = ?", "is_latest_revision"]
    params: list[object] = [TREND_GRID_FACTOR_ID, TREND_GRID_SOURCE_NAME]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    return store.con.execute(
        f"""
        WITH decisions AS (
            SELECT
                factor_value_id AS trend_grid_factor_value_id,
                security_id,
                symbol,
                as_of_date,
                available_at AS decision_available_at,
                input_lineage_json AS trend_grid_lineage
            FROM fundamental_factor_values
            WHERE {' AND '.join(predicates)}
        ),
        history_components AS (
            SELECT
                d.trend_grid_factor_value_id,
                d.security_id,
                d.symbol,
                d.as_of_date,
                d.decision_available_at,
                p.factor_value_id AS qgp_factor_value_id,
                p.available_at AS qgp_available_at,
                TRY_CAST(json_extract_string(
                    p.input_lineage_json,'$.quarterly_statement.period_end'
                ) AS DATE) AS period_end,
                TRY_CAST(json_extract_string(
                    p.input_lineage_json,
                    '$.quarterly_statement.gross_profit.resolved_value'
                ) AS DOUBLE) AS gross_profit,
                TRY_CAST(json_extract_string(
                    p.input_lineage_json,'$.quarterly_statement.revenue.value'
                ) AS DOUBLE) AS revenue,
                json_extract_string(
                    p.input_lineage_json,'$.quarterly_statement.accession_number'
                ) AS accession_number,
                json_extract_string(
                    p.input_lineage_json,'$.quarterly_statement.gross_profit_method'
                ) AS gross_profit_method
            FROM decisions d,
                 json_each(d.trend_grid_lineage,'$.history') h
            JOIN fundamental_factor_values p
              ON p.factor_value_id = json_extract_string(h.value,'$.factor_value_id')
             AND p.factor_id = ?
             AND p.source = ?
             AND p.is_latest_revision
            WHERE p.available_at <= d.decision_available_at
              AND p.as_of_date <= d.as_of_date
        ),
        valid_history AS (
            SELECT
                *,
                gross_profit/revenue AS gross_margin,
                date_diff('day',DATE '1970-01-01',period_end)/91.3125 AS trend_t,
                quarter(period_end) AS seasonal_quarter
            FROM history_components
            WHERE period_end IS NOT NULL
              AND gross_profit IS NOT NULL
              AND isfinite(gross_profit)
              AND revenue > 0
              AND isfinite(revenue)
              AND isfinite(gross_profit/revenue)
              AND abs(gross_profit/revenue) <= ?
        ),
        demeaned AS (
            SELECT
                *,
                trend_t-avg(trend_t) OVER (
                    PARTITION BY trend_grid_factor_value_id,seasonal_quarter
                ) AS residual_t,
                gross_margin-avg(gross_margin) OVER (
                    PARTITION BY trend_grid_factor_value_id,seasonal_quarter
                ) AS residual_gross_margin
            FROM valid_history
        ),
        estimates AS (
            SELECT
                trend_grid_factor_value_id,
                any_value(security_id) AS security_id,
                any_value(symbol) AS symbol,
                any_value(as_of_date) AS as_of_date,
                any_value(decision_available_at) AS decision_available_at,
                min(period_end) AS oldest_period_end,
                max(period_end) AS latest_period_end,
                date_diff('day',min(period_end),max(period_end)) AS history_span_days,
                count(*) AS observation_count,
                count(DISTINCT seasonal_quarter) AS seasonal_quarter_count,
                sum(residual_t*residual_gross_margin)
                    / nullif(sum(residual_t*residual_t),0) AS gross_margin_trend,
                to_json(list(struct_pack(
                    qgp_factor_value_id := qgp_factor_value_id,
                    accession_number := accession_number,
                    period_end := period_end,
                    gross_profit_method := gross_profit_method,
                    gross_profit := gross_profit,
                    revenue := revenue,
                    gross_margin := gross_margin,
                    available_at := qgp_available_at
                ) ORDER BY period_end)) AS history_json
            FROM demeaned
            GROUP BY trend_grid_factor_value_id
            HAVING count(*) = ?
               AND count(DISTINCT seasonal_quarter) >= ?
               AND sum(residual_t*residual_t) > 0
        )
        SELECT *
        FROM estimates
        WHERE isfinite(gross_margin_trend)
          AND abs(gross_margin_trend) <= ?
        ORDER BY as_of_date,security_id
        """,
        [
            *params,
            QGP_FACTOR_ID,
            QGP_SOURCE_NAME,
            options.maximum_absolute_gross_margin,
            options.minimum_observations,
            options.minimum_seasonal_quarters,
            options.maximum_absolute_trend,
        ],
    ).df()


def _lineage(row: pd.Series, options: GrossMarginTrendOptions) -> str:
    history = row.get("history_json")
    if isinstance(history, str):
        history = json.loads(history)
    return json_dumps(
        {
            "method": "akbas_jiang_koch_gross_margin_trend_pit",
            "formula": (
                "beta from quarterly gross_profit/revenue = "
                "a + beta*elapsed_quarters + calendar-quarter fixed effects"
            ),
            "orientation": "higher_gross_margin_trend_is_preferred",
            "published_method_adaptations": {
                "filing_gaps": (
                    "Uses the governed eight-observation trend grid with actual elapsed time."
                ),
                "timing": "Uses actual PIT filing availability.",
                "fallback": (
                    "Reported gross profit is allowed only when the same parent also has revenue."
                ),
            },
            "decision": {
                "trend_grid_factor_id": TREND_GRID_FACTOR_ID,
                "trend_grid_factor_value_id": row["trend_grid_factor_value_id"],
                "as_of_date": row["as_of_date"],
                "available_at": row["decision_available_at"],
            },
            "history": history,
            "oldest_period_end": row.get("oldest_period_end"),
            "latest_period_end": row.get("latest_period_end"),
            "history_span_days": row.get("history_span_days"),
            "observation_count": row.get("observation_count"),
            "seasonal_quarter_count": row.get("seasonal_quarter_count"),
            "gross_margin_trend": row["raw_value"],
            "research_contract": {
                "minimum_observations": options.minimum_observations,
                "minimum_seasonal_quarters": options.minimum_seasonal_quarters,
                "maximum_absolute_gross_margin": options.maximum_absolute_gross_margin,
                "maximum_absolute_trend": options.maximum_absolute_trend,
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "return_fitted_parameters": False,
            },
        }
    )


def compute_gross_margin_trend_rows(
    inputs: pd.DataFrame,
    options: GrossMarginTrendOptions | None = None,
) -> pd.DataFrame:
    """Guard, winsorize, and standardize point-in-time gross-margin trends."""

    options = options or GrossMarginTrendOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "trend_grid_factor_value_id",
        "security_id",
        "symbol",
        "as_of_date",
        "decision_available_at",
        "gross_margin_trend",
        "history_json",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"gross-margin trend inputs missing columns: {missing}")
    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(
        rows["decision_available_at"], errors="coerce"
    )
    rows["gross_margin_trend"] = pd.to_numeric(
        rows["gross_margin_trend"], errors="coerce"
    )
    rows = rows.dropna(
        subset=["security_id", "as_of_date", "available_at", "gross_margin_trend"]
    )
    rows = rows[
        rows["gross_margin_trend"].map(math.isfinite)
        & (rows["gross_margin_trend"].abs() <= options.maximum_absolute_trend)
    ].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["gross_margin_trend"].astype(float)
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
        [f"factor:{TREND_GRID_FACTOR_ID}", f"factor:{QGP_FACTOR_ID}"]
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


def refresh_gross_margin_trend_values(
    store: DuckDBStore,
    options: GrossMarginTrendOptions | None = None,
) -> int:
    """Materialize the PIT eight-observation quarterly gross-margin trend."""

    options = options or GrossMarginTrendOptions()
    store.initialize()
    rows = compute_gross_margin_trend_rows(
        load_gross_margin_trend_inputs(store, options), options
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
                "gross_margin_trend_insert",
            )
    return len(rows)
