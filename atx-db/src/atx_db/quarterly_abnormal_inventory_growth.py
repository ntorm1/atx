"""Point-in-time sales-adjusted quarterly inventory growth factor."""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import winsorize, zscore
from .quarterly_cash_profitability import FACTOR_ID as CASH_PROFITABILITY_FACTOR_ID
from .quarterly_cash_profitability import SOURCE_NAME as CASH_SOURCE_NAME
from .quarterly_operating_profitability import (
    FACTOR_ID as OPERATING_PROFITABILITY_FACTOR_ID,
)
from .quarterly_operating_profitability import SOURCE_NAME as OPERATING_SOURCE_NAME
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT quarterly abnormal inventory growth v1"
FACTOR_ID = "investment_low_quarterly_abnormal_inventory_growth"
FACTOR_NAME = "PIT low quarterly abnormal inventory growth"
FACTOR_FAMILY = "fundamental_investment"
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
class QuarterlyAbnormalInventoryGrowthOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_period_gap_days: int = 330
    maximum_period_gap_days: int = 400
    maximum_absolute_component_growth: float = 10.0
    maximum_absolute_raw_value: float = 10.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_quarterly_abnormal_inventory_growth_inputs(
    store: DuckDBStore,
    options: QuarterlyAbnormalInventoryGrowthOptions | None = None,
) -> pd.DataFrame:
    """Select the closest governed same-quarter-prior-year inventory/revenue pair."""

    options = options or QuarterlyAbnormalInventoryGrowthOptions()
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
        WITH cash_history AS (
            SELECT
                c.factor_value_id AS cash_factor_value_id,
                c.security_id,
                c.symbol,
                c.as_of_date,
                c.available_at AS decision_available_at,
                TRY_CAST(json_extract_string(
                    c.input_lineage_json,'$.statements.current_period_end'
                ) AS DATE) AS period_end,
                TRY_CAST(json_extract_string(
                    c.input_lineage_json,'$.working_capital.inventory.current_value'
                ) AS DOUBLE) AS inventory,
                json_extract_string(
                    c.input_lineage_json,'$.working_capital.inventory.current_id'
                ) AS inventory_id,
                TRY_CAST(json_extract_string(
                    c.input_lineage_json,
                    '$.working_capital.inventory.current_available_at'
                ) AS TIMESTAMP) AS inventory_available_at,
                json_extract_string(
                    c.input_lineage_json,
                    '$.operating_profitability_input.factor_value_id'
                ) AS qop_factor_value_id
            FROM fundamental_factor_values c
            WHERE c.factor_id = ?
              AND c.source = ?
              AND c.is_latest_revision
        ),
        history AS (
            SELECT
                h.*,
                TRY_CAST(json_extract_string(
                    q.input_lineage_json,'$.quarterly_statement.revenue.value'
                ) AS DOUBLE) AS revenue,
                json_extract_string(
                    q.input_lineage_json,'$.quarterly_statement.revenue.id'
                ) AS revenue_id,
                TRY_CAST(json_extract_string(
                    q.input_lineage_json,'$.quarterly_statement.available_at'
                ) AS TIMESTAMP) AS revenue_available_at
            FROM cash_history h
            JOIN fundamental_factor_values q
              ON q.factor_value_id = h.qop_factor_value_id
             AND q.factor_id = ?
             AND q.source = ?
             AND q.is_latest_revision
        ),
        current_rows AS (
            SELECT * FROM history {current_sql}
        ),
        candidates AS (
            SELECT
                cur.cash_factor_value_id,
                cur.qop_factor_value_id,
                cur.security_id,
                cur.symbol,
                cur.as_of_date,
                cur.decision_available_at,
                cur.period_end AS current_period_end,
                cur.inventory AS current_inventory,
                cur.inventory_id AS current_inventory_id,
                cur.inventory_available_at AS current_inventory_available_at,
                cur.revenue AS current_revenue,
                cur.revenue_id AS current_revenue_id,
                cur.revenue_available_at AS current_revenue_available_at,
                old.cash_factor_value_id AS prior_cash_factor_value_id,
                old.qop_factor_value_id AS prior_qop_factor_value_id,
                old.decision_available_at AS prior_decision_available_at,
                old.period_end AS prior_period_end,
                old.inventory AS prior_inventory,
                old.inventory_id AS prior_inventory_id,
                old.inventory_available_at AS prior_inventory_available_at,
                old.revenue AS prior_revenue,
                old.revenue_id AS prior_revenue_id,
                old.revenue_available_at AS prior_revenue_available_at,
                date_diff('day',old.period_end,cur.period_end) AS period_gap_days,
                row_number() OVER (
                    PARTITION BY cur.cash_factor_value_id
                    ORDER BY abs(
                        date_diff('day',old.period_end,cur.period_end)-365
                    ),old.decision_available_at DESC NULLS LAST,
                      old.cash_factor_value_id DESC NULLS LAST
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
            CASH_PROFITABILITY_FACTOR_ID,
            CASH_SOURCE_NAME,
            OPERATING_PROFITABILITY_FACTOR_ID,
            OPERATING_SOURCE_NAME,
            *date_params,
            options.maximum_period_gap_days,
            options.minimum_period_gap_days,
        ],
    ).df()


def _lineage(
    row: pd.Series,
    options: QuarterlyAbnormalInventoryGrowthOptions,
) -> str:
    return json_dumps(
        {
            "method": "sales_adjusted_inventory_growth_same_quarter_prior_year_pit",
            "formula": (
                "(inventory_t/inventory_t_4-1)-(revenue_t/revenue_t_4-1)"
            ),
            "orientation": "lower_abnormal_inventory_growth_is_preferred",
            "research_contract": {
                "period_gap_days": [
                    options.minimum_period_gap_days,
                    options.maximum_period_gap_days,
                ],
                "maximum_absolute_component_growth": (
                    options.maximum_absolute_component_growth
                ),
                "maximum_absolute_raw_value": options.maximum_absolute_raw_value,
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "return_fitted_parameters": False,
            },
            "current": {
                "cash_factor_value_id": row["cash_factor_value_id"],
                "qop_factor_value_id": row["qop_factor_value_id"],
                "period_end": row.get("current_period_end"),
                "decision_available_at": row["decision_available_at"],
                "inventory": {
                    "value": row["current_inventory"],
                    "id": row.get("current_inventory_id"),
                    "available_at": row.get("current_inventory_available_at"),
                },
                "revenue": {
                    "value": row["current_revenue"],
                    "id": row.get("current_revenue_id"),
                    "available_at": row.get("current_revenue_available_at"),
                },
            },
            "prior_year": {
                "cash_factor_value_id": row["prior_cash_factor_value_id"],
                "qop_factor_value_id": row["prior_qop_factor_value_id"],
                "period_end": row.get("prior_period_end"),
                "decision_available_at": row["prior_decision_available_at"],
                "inventory": {
                    "value": row["prior_inventory"],
                    "id": row.get("prior_inventory_id"),
                    "available_at": row.get("prior_inventory_available_at"),
                },
                "revenue": {
                    "value": row["prior_revenue"],
                    "id": row.get("prior_revenue_id"),
                    "available_at": row.get("prior_revenue_available_at"),
                },
            },
            "period_gap_days": row.get("period_gap_days"),
            "inventory_growth": row["inventory_growth"],
            "sales_growth": row["sales_growth"],
            "abnormal_inventory_growth": row["raw_value"],
        }
    )


def compute_quarterly_abnormal_inventory_growth_rows(
    inputs: pd.DataFrame,
    options: QuarterlyAbnormalInventoryGrowthOptions | None = None,
) -> pd.DataFrame:
    """Compute low year-over-year inventory growth in excess of sales growth."""

    options = options or QuarterlyAbnormalInventoryGrowthOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "cash_factor_value_id",
        "qop_factor_value_id",
        "prior_cash_factor_value_id",
        "prior_qop_factor_value_id",
        "security_id",
        "symbol",
        "as_of_date",
        "decision_available_at",
        "prior_decision_available_at",
        "current_inventory",
        "prior_inventory",
        "current_revenue",
        "prior_revenue",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"quarterly abnormal inventory inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    for column in ("decision_available_at", "prior_decision_available_at"):
        rows[column] = pd.to_datetime(rows[column], errors="coerce")
    for column in (
        "current_inventory",
        "prior_inventory",
        "current_revenue",
        "prior_revenue",
    ):
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(subset=sorted(required))
    valid = pd.Series(True, index=rows.index)
    for column in (
        "current_inventory",
        "prior_inventory",
        "current_revenue",
        "prior_revenue",
    ):
        valid &= rows[column].map(math.isfinite) & (rows[column] > 0)
    rows = rows[valid].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["inventory_growth"] = rows["current_inventory"] / rows["prior_inventory"] - 1.0
    rows["sales_growth"] = rows["current_revenue"] / rows["prior_revenue"] - 1.0
    rows = rows[
        rows["inventory_growth"].map(math.isfinite)
        & rows["sales_growth"].map(math.isfinite)
        & (rows["inventory_growth"].abs() <= options.maximum_absolute_component_growth)
        & (rows["sales_growth"].abs() <= options.maximum_absolute_component_growth)
    ].copy()
    rows["raw_value"] = rows["inventory_growth"] - rows["sales_growth"]
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
    rows["oriented_value"] = -rows["raw_value"]
    rows = winsorize(
        rows,
        value_column="oriented_value",
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
    rows["input_ids_json"] = json_dumps(
        [
            f"factor:{CASH_PROFITABILITY_FACTOR_ID}",
            f"factor:{OPERATING_PROFITABILITY_FACTOR_ID}",
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


def refresh_quarterly_abnormal_inventory_growth_values(
    store: DuckDBStore,
    options: QuarterlyAbnormalInventoryGrowthOptions | None = None,
) -> int:
    """Materialize point-in-time sales-adjusted quarterly inventory growth."""

    options = options or QuarterlyAbnormalInventoryGrowthOptions()
    store.initialize()
    rows = compute_quarterly_abnormal_inventory_growth_rows(
        load_quarterly_abnormal_inventory_growth_inputs(store, options), options
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
                "quarterly_abnormal_inventory_growth_insert",
            )
    return len(rows)
