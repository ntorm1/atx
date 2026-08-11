"""Revenue growth gated by non-declining same-quarter gross margin."""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import zscore
from .quarterly_operating_profitability import FACTOR_ID as QOP_FACTOR_ID
from .quarterly_operating_profitability import SOURCE_NAME as QOP_SOURCE_NAME
from .quarterly_revenue_growth import FACTOR_ID as REVENUE_GROWTH_FACTOR_ID
from .quarterly_revenue_growth import SOURCE_NAME as REVENUE_GROWTH_SOURCE_NAME
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT quarterly revenue margin confirmation v1"
FACTOR_ID = "growth_quarterly_revenue_margin_confirmation"
FACTOR_NAME = "PIT revenue growth with non-declining gross margin"
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
class QuarterlyRevenueMarginConfirmationOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    maximum_absolute_gross_margin: float = 5.0
    minimum_names_per_date: int = 20
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_quarterly_revenue_margin_confirmation_inputs(
    store: DuckDBStore,
    options: QuarterlyRevenueMarginConfirmationOptions | None = None,
) -> pd.DataFrame:
    """Reconstruct gross margins from the exact QOP rows in revenue-growth lineage."""

    options = options or QuarterlyRevenueMarginConfirmationOptions()
    predicates = ["factor_id = ?", "source = ?", "is_latest_revision"]
    params: list[object] = [REVENUE_GROWTH_FACTOR_ID, REVENUE_GROWTH_SOURCE_NAME]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    return store.con.execute(
        f"""
        WITH revenue_growth AS (
            SELECT
                factor_value_id AS revenue_growth_factor_value_id,
                security_id,
                symbol,
                as_of_date,
                available_at AS decision_available_at,
                raw_value AS revenue_growth_raw_value,
                value AS revenue_growth_value,
                json_extract_string(
                    input_lineage_json,'$.current.qop_factor_value_id'
                ) AS current_qop_factor_value_id,
                json_extract_string(
                    input_lineage_json,'$.prior_year.qop_factor_value_id'
                ) AS prior_qop_factor_value_id
            FROM fundamental_factor_values
            WHERE {' AND '.join(predicates)}
        ),
        qop AS (
            SELECT
                factor_value_id AS qop_factor_value_id,
                available_at AS qop_available_at,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.quarterly_statement.period_end'
                ) AS DATE) AS period_end,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.quarterly_statement.revenue.value'
                ) AS DOUBLE) AS revenue,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.quarterly_statement.cogs.value'
                ) AS DOUBLE) AS cogs,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.quarterly_statement.gross_profit.value'
                ) AS DOUBLE) AS reported_gross_profit,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.quarterly_statement.operating_profit'
                ) AS DOUBLE) AS operating_profit,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.quarterly_statement.sga.value'
                ) AS DOUBLE) AS sga,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.quarterly_statement.rd_expense.value'
                ) AS DOUBLE) AS rd_expense
            FROM fundamental_factor_values
            WHERE factor_id = ? AND source = ? AND is_latest_revision
        ),
        margins AS (
            SELECT
                *,
                CASE
                    WHEN isfinite(cogs) THEN revenue-cogs
                    WHEN isfinite(reported_gross_profit) THEN reported_gross_profit
                    ELSE operating_profit+sga-
                         CASE WHEN isfinite(rd_expense) THEN rd_expense ELSE 0 END
                END AS gross_profit
            FROM qop
        )
        SELECT
            r.*,
            cur.qop_available_at AS current_qop_available_at,
            cur.period_end AS current_period_end,
            cur.revenue AS current_revenue,
            cur.gross_profit AS current_gross_profit,
            cur.gross_profit/cur.revenue AS current_gross_margin,
            old.qop_available_at AS prior_qop_available_at,
            old.period_end AS prior_period_end,
            old.revenue AS prior_revenue,
            old.gross_profit AS prior_gross_profit,
            old.gross_profit/old.revenue AS prior_gross_margin
        FROM revenue_growth r
        JOIN margins cur
          ON cur.qop_factor_value_id = r.current_qop_factor_value_id
        JOIN margins old
          ON old.qop_factor_value_id = r.prior_qop_factor_value_id
        ORDER BY r.as_of_date,r.security_id
        """,
        [*params, QOP_FACTOR_ID, QOP_SOURCE_NAME],
    ).df()


def _lineage(
    row: pd.Series,
    options: QuarterlyRevenueMarginConfirmationOptions,
) -> str:
    return json_dumps(
        {
            "method": "nondeclining_same_quarter_gross_margin_gate_then_revenue_growth",
            "gate": "current_gross_margin-prior_year_gross_margin >= 0",
            "revenue_growth": {
                "factor_id": REVENUE_GROWTH_FACTOR_ID,
                "factor_value_id": row["revenue_growth_factor_value_id"],
                "raw_value": row["revenue_growth_raw_value"],
                "value": row["revenue_growth_value"],
                "available_at": row["decision_available_at"],
            },
            "current": {
                "qop_factor_value_id": row["current_qop_factor_value_id"],
                "period_end": row["current_period_end"],
                "revenue": row["current_revenue"],
                "gross_profit": row["current_gross_profit"],
                "gross_margin": row["current_gross_margin"],
                "available_at": row["current_qop_available_at"],
            },
            "prior_year": {
                "qop_factor_value_id": row["prior_qop_factor_value_id"],
                "period_end": row["prior_period_end"],
                "revenue": row["prior_revenue"],
                "gross_profit": row["prior_gross_profit"],
                "gross_margin": row["prior_gross_margin"],
                "available_at": row["prior_qop_available_at"],
            },
            "gross_margin_change": row["gross_margin_change"],
            "research_contract": {
                "maximum_absolute_gross_margin": options.maximum_absolute_gross_margin,
                "return_fitted_parameters": False,
            },
        }
    )


def compute_quarterly_revenue_margin_confirmation_rows(
    inputs: pd.DataFrame,
    options: QuarterlyRevenueMarginConfirmationOptions | None = None,
) -> pd.DataFrame:
    """Keep revenue-growth scores only where same-quarter gross margin does not fall."""

    options = options or QuarterlyRevenueMarginConfirmationOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "revenue_growth_factor_value_id",
        "security_id",
        "symbol",
        "as_of_date",
        "decision_available_at",
        "revenue_growth_raw_value",
        "revenue_growth_value",
        "current_gross_margin",
        "prior_gross_margin",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"revenue margin confirmation inputs missing columns: {missing}")
    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["decision_available_at"], errors="coerce")
    for column in (
        "revenue_growth_raw_value",
        "revenue_growth_value",
        "current_gross_margin",
        "prior_gross_margin",
    ):
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(
        subset=[
            "security_id",
            "as_of_date",
            "available_at",
            "revenue_growth_value",
            "current_gross_margin",
            "prior_gross_margin",
        ]
    )
    rows = rows[
        rows["revenue_growth_value"].map(math.isfinite)
        & rows["current_gross_margin"].map(math.isfinite)
        & rows["prior_gross_margin"].map(math.isfinite)
        & (rows["current_gross_margin"].abs() <= options.maximum_absolute_gross_margin)
        & (rows["prior_gross_margin"].abs() <= options.maximum_absolute_gross_margin)
    ].copy()
    rows["gross_margin_change"] = rows["current_gross_margin"] - rows["prior_gross_margin"]
    rows = rows[rows["gross_margin_change"] >= 0].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["revenue_growth_value"].astype(float)
    rows = zscore(
        rows,
        value_column="raw_value",
        output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    rows["input_ids_json"] = json_dumps(
        [f"factor:{REVENUE_GROWTH_FACTOR_ID}", f"factor:{QOP_FACTOR_ID}"]
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


def refresh_quarterly_revenue_margin_confirmation_values(
    store: DuckDBStore,
    options: QuarterlyRevenueMarginConfirmationOptions | None = None,
) -> int:
    """Materialize the margin-confirmed revenue-growth sleeve."""

    options = options or QuarterlyRevenueMarginConfirmationOptions()
    store.initialize()
    rows = compute_quarterly_revenue_margin_confirmation_rows(
        load_quarterly_revenue_margin_confirmation_inputs(store, options), options
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
                "quarterly_revenue_margin_confirmation_insert",
            )
    return len(rows)
