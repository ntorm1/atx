"""Point-in-time same-quarter year-over-year gross-margin change."""

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
from .quarterly_revenue_growth import FACTOR_ID as REVENUE_GROWTH_FACTOR_ID
from .quarterly_revenue_margin_confirmation import (
    QuarterlyRevenueMarginConfirmationOptions,
    load_quarterly_revenue_margin_confirmation_inputs,
)
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT quarterly gross margin change v1"
FACTOR_ID = "profitability_quarterly_gross_margin_change_yoy"
FACTOR_NAME = "PIT same-quarter change in quarterly gross margin"
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
class QuarterlyGrossMarginChangeOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    maximum_absolute_gross_margin: float = 5.0
    maximum_absolute_margin_change: float = 5.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_quarterly_gross_margin_change_inputs(
    store: DuckDBStore,
    options: QuarterlyGrossMarginChangeOptions | None = None,
) -> pd.DataFrame:
    """Load exact current/prior-year gross margins visible at each decision."""

    options = options or QuarterlyGrossMarginChangeOptions()
    return load_quarterly_revenue_margin_confirmation_inputs(
        store,
        QuarterlyRevenueMarginConfirmationOptions(
            start_date=options.start_date,
            end_date=options.end_date,
            maximum_absolute_gross_margin=options.maximum_absolute_gross_margin,
            minimum_names_per_date=options.minimum_names_per_date,
        ),
    )


def _lineage(row: pd.Series, options: QuarterlyGrossMarginChangeOptions) -> str:
    return json_dumps(
        {
            "method": "same_quarter_year_over_year_gross_margin_change_pit",
            "formula": "gross_margin_t-gross_margin_t_4",
            "orientation": "higher_margin_change_is_preferred",
            "revenue_growth_anchor": {
                "factor_id": REVENUE_GROWTH_FACTOR_ID,
                "factor_value_id": row["revenue_growth_factor_value_id"],
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
            "gross_margin_change": row["raw_value"],
            "research_contract": {
                "same_quarter_prior_year": True,
                "maximum_absolute_gross_margin": options.maximum_absolute_gross_margin,
                "maximum_absolute_margin_change": options.maximum_absolute_margin_change,
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "missing_components_imputed": False,
                "return_fitted_parameters": False,
            },
        }
    )


def compute_quarterly_gross_margin_change_rows(
    inputs: pd.DataFrame,
    options: QuarterlyGrossMarginChangeOptions | None = None,
) -> pd.DataFrame:
    """Compute, bound, winsorize, and standardize gross-margin changes."""

    options = options or QuarterlyGrossMarginChangeOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "revenue_growth_factor_value_id",
        "security_id",
        "symbol",
        "as_of_date",
        "decision_available_at",
        "current_qop_factor_value_id",
        "current_qop_available_at",
        "current_gross_margin",
        "prior_qop_factor_value_id",
        "prior_qop_available_at",
        "prior_gross_margin",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"quarterly gross margin change inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    availability_columns = (
        "decision_available_at",
        "current_qop_available_at",
        "prior_qop_available_at",
    )
    for column in availability_columns:
        rows[column] = pd.to_datetime(rows[column], errors="coerce")
    for column in ("current_gross_margin", "prior_gross_margin"):
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(
        subset=[
            "security_id",
            "as_of_date",
            *availability_columns,
            "current_gross_margin",
            "prior_gross_margin",
        ]
    ).copy()
    rows = rows[
        rows["current_gross_margin"].map(math.isfinite)
        & rows["prior_gross_margin"].map(math.isfinite)
        & (rows["current_gross_margin"].abs() <= options.maximum_absolute_gross_margin)
        & (rows["prior_gross_margin"].abs() <= options.maximum_absolute_gross_margin)
    ].copy()
    rows["raw_value"] = rows["current_gross_margin"] - rows["prior_gross_margin"]
    rows = rows[
        rows["raw_value"].map(math.isfinite)
        & (rows["raw_value"].abs() <= options.maximum_absolute_margin_change)
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
    rows["available_at"] = rows[list(availability_columns)].max(axis=1)
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


def refresh_quarterly_gross_margin_change_values(
    store: DuckDBStore,
    options: QuarterlyGrossMarginChangeOptions | None = None,
) -> int:
    """Materialize the point-in-time quarterly gross-margin-change factor."""

    options = options or QuarterlyGrossMarginChangeOptions()
    store.initialize()
    rows = compute_quarterly_gross_margin_change_rows(
        load_quarterly_gross_margin_change_inputs(store, options), options
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
                "quarterly_gross_margin_change_insert",
            )
    return len(rows)
