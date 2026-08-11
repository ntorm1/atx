"""Point-in-time same-quarter year-over-year revenue growth factor."""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import winsorize, zscore
from .quarterly_operating_profitability import FACTOR_ID as QOP_FACTOR_ID
from .quarterly_operating_profitability import SOURCE_NAME as QOP_SOURCE_NAME
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT quarterly revenue growth v1"
FACTOR_ID = "growth_quarterly_revenue_yoy"
FACTOR_NAME = "PIT same-quarter year-over-year revenue growth"
FACTOR_FAMILY = "fundamental_growth"
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
class QuarterlyRevenueGrowthOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_period_gap_days: int = 330
    maximum_period_gap_days: int = 400
    maximum_absolute_raw_value: float = 10.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_quarterly_revenue_growth_inputs(
    store: DuckDBStore,
    options: QuarterlyRevenueGrowthOptions | None = None,
) -> pd.DataFrame:
    """Select the closest visible same-quarter-prior-year governed revenue row."""

    options = options or QuarterlyRevenueGrowthOptions()
    current_predicates: list[str] = []
    date_params: list[object] = []
    if options.start_date is not None:
        current_predicates.append("as_of_date >= ?")
        date_params.append(options.start_date)
    if options.end_date is not None:
        current_predicates.append("as_of_date <= ?")
        date_params.append(options.end_date)
    current_sql = (
        "WHERE " + " AND ".join(current_predicates)
        if current_predicates
        else ""
    )
    return store.con.execute(
        f"""
        WITH history AS (
            SELECT
                factor_value_id AS qop_factor_value_id,
                security_id,
                symbol,
                as_of_date,
                available_at AS decision_available_at,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.quarterly_statement.period_end'
                ) AS DATE) AS period_end,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.quarterly_statement.revenue.value'
                ) AS DOUBLE) AS revenue,
                json_extract_string(
                    input_lineage_json,'$.quarterly_statement.revenue.id'
                ) AS revenue_id,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.quarterly_statement.available_at'
                ) AS TIMESTAMP) AS revenue_available_at
            FROM fundamental_factor_values
            WHERE factor_id = ?
              AND source = ?
              AND is_latest_revision
        ),
        current_rows AS (
            SELECT * FROM history {current_sql}
        ),
        candidates AS (
            SELECT
                cur.qop_factor_value_id,
                cur.security_id,
                cur.symbol,
                cur.as_of_date,
                cur.decision_available_at,
                cur.period_end AS current_period_end,
                cur.revenue AS current_revenue,
                cur.revenue_id AS current_revenue_id,
                cur.revenue_available_at AS current_revenue_available_at,
                old.qop_factor_value_id AS prior_qop_factor_value_id,
                old.decision_available_at AS prior_decision_available_at,
                old.period_end AS prior_period_end,
                old.revenue AS prior_revenue,
                old.revenue_id AS prior_revenue_id,
                old.revenue_available_at AS prior_revenue_available_at,
                date_diff('day',old.period_end,cur.period_end) AS period_gap_days,
                row_number() OVER (
                    PARTITION BY cur.qop_factor_value_id
                    ORDER BY abs(
                        date_diff('day',old.period_end,cur.period_end)-365
                    ),old.decision_available_at DESC NULLS LAST,
                      old.qop_factor_value_id DESC NULLS LAST
                ) AS prior_rank
            FROM current_rows cur
            LEFT JOIN history old
              ON old.security_id = cur.security_id
             AND old.period_end BETWEEN
                    cur.period_end - (? * INTERVAL 1 DAY)
                    AND cur.period_end - (? * INTERVAL 1 DAY)
             AND old.decision_available_at <= cur.decision_available_at
        )
        SELECT * EXCLUDE (prior_rank)
        FROM candidates
        WHERE prior_rank = 1
        ORDER BY as_of_date,security_id
        """,
        [
            QOP_FACTOR_ID,
            QOP_SOURCE_NAME,
            *date_params,
            options.maximum_period_gap_days,
            options.minimum_period_gap_days,
        ],
    ).df()


def _lineage(row: pd.Series, options: QuarterlyRevenueGrowthOptions) -> str:
    return json_dumps(
        {
            "method": "same_quarter_prior_year_revenue_growth_pit",
            "formula": "revenue_t/revenue_t_4-1",
            "orientation": "higher_revenue_growth_is_preferred",
            "research_contract": {
                "period_gap_days": [
                    options.minimum_period_gap_days,
                    options.maximum_period_gap_days,
                ],
                "maximum_absolute_raw_value": options.maximum_absolute_raw_value,
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "return_fitted_parameters": False,
            },
            "current": {
                "qop_factor_value_id": row["qop_factor_value_id"],
                "period_end": row.get("current_period_end"),
                "decision_available_at": row["decision_available_at"],
                "revenue": {
                    "value": row["current_revenue"],
                    "id": row.get("current_revenue_id"),
                    "available_at": row.get("current_revenue_available_at"),
                },
            },
            "prior_year": {
                "qop_factor_value_id": row["prior_qop_factor_value_id"],
                "period_end": row.get("prior_period_end"),
                "decision_available_at": row["prior_decision_available_at"],
                "revenue": {
                    "value": row["prior_revenue"],
                    "id": row.get("prior_revenue_id"),
                    "available_at": row.get("prior_revenue_available_at"),
                },
            },
            "period_gap_days": row.get("period_gap_days"),
            "revenue_growth": row["raw_value"],
        }
    )


def compute_quarterly_revenue_growth_rows(
    inputs: pd.DataFrame,
    options: QuarterlyRevenueGrowthOptions | None = None,
) -> pd.DataFrame:
    """Compute guarded and standardized same-quarter revenue growth."""

    options = options or QuarterlyRevenueGrowthOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "qop_factor_value_id",
        "prior_qop_factor_value_id",
        "security_id",
        "symbol",
        "as_of_date",
        "decision_available_at",
        "prior_decision_available_at",
        "current_revenue",
        "prior_revenue",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"quarterly revenue growth inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    for column in ("decision_available_at", "prior_decision_available_at"):
        rows[column] = pd.to_datetime(rows[column], errors="coerce")
    for column in ("current_revenue", "prior_revenue"):
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(subset=sorted(required))
    rows = rows[
        rows["current_revenue"].map(math.isfinite)
        & rows["prior_revenue"].map(math.isfinite)
        & (rows["current_revenue"] > 0)
        & (rows["prior_revenue"] > 0)
    ].copy()
    rows["raw_value"] = rows["current_revenue"] / rows["prior_revenue"] - 1.0
    rows = rows[
        rows["raw_value"].map(math.isfinite)
        & (rows["raw_value"].abs() <= options.maximum_absolute_raw_value)
    ].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
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
    rows["available_at"] = rows[
        ["decision_available_at", "prior_decision_available_at"]
    ].max(axis=1)
    rows["input_ids_json"] = json_dumps([f"factor:{QOP_FACTOR_ID}"])
    rows["input_lineage_json"] = rows.apply(lambda row: _lineage(row, options), axis=1)
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


def refresh_quarterly_revenue_growth_values(
    store: DuckDBStore,
    options: QuarterlyRevenueGrowthOptions | None = None,
) -> int:
    """Materialize point-in-time same-quarter revenue growth."""

    options = options or QuarterlyRevenueGrowthOptions()
    store.initialize()
    rows = compute_quarterly_revenue_growth_rows(
        load_quarterly_revenue_growth_inputs(store, options), options
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
                "quarterly_revenue_growth_insert",
            )
    return len(rows)
