"""Standardized earnings surprise gated by same-sign direct revenue growth."""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .earnings_surprise import FACTOR_ID as SUE_FACTOR_ID
from .earnings_surprise import SOURCE_NAME as SUE_SOURCE_NAME
from .factors.cross_section import zscore
from .quarterly_revenue_growth import FACTOR_ID as REVENUE_GROWTH_FACTOR_ID
from .quarterly_revenue_growth import SOURCE_NAME as REVENUE_GROWTH_SOURCE_NAME
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT SUE revenue-growth agreement v1"
FACTOR_ID = "earnings_sue_revenue_growth_agreement"
FACTOR_NAME = "PIT SUE with same-sign revenue-growth confirmation"
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
class EarningsRevenueGrowthAgreementOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_names_per_date: int = 20
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_earnings_revenue_growth_agreement_inputs(
    store: DuckDBStore,
    options: EarningsRevenueGrowthAgreementOptions | None = None,
) -> pd.DataFrame:
    """Load latest governed SUE and direct revenue growth on identical keys."""

    options = options or EarningsRevenueGrowthAgreementOptions()
    predicates = ["factor_id IN (?, ?)", "is_latest_revision"]
    params: list[object] = [SUE_FACTOR_ID, REVENUE_GROWTH_FACTOR_ID]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    return store.con.execute(
        f"""
        WITH ranked AS (
            SELECT *,row_number() OVER (
                PARTITION BY factor_id,security_id,as_of_date
                ORDER BY available_at DESC,source_loaded_at DESC,factor_value_id DESC
            ) AS revision_rank
            FROM fundamental_factor_values
            WHERE {' AND '.join(predicates)}
              AND ((factor_id = ? AND source = ?) OR
                   (factor_id = ? AND source = ?))
              AND value IS NOT NULL AND isfinite(value)
        ),
        paired AS (
            SELECT
                security_id,
                as_of_date,
                any_value(symbol) AS symbol,
                max(value) FILTER (WHERE factor_id = ?) AS sue_value,
                max(factor_value_id) FILTER (WHERE factor_id = ?)
                    AS sue_factor_value_id,
                max(available_at) FILTER (WHERE factor_id = ?) AS sue_available_at,
                max(value) FILTER (WHERE factor_id = ?) AS revenue_growth_value,
                max(factor_value_id) FILTER (WHERE factor_id = ?)
                    AS revenue_growth_factor_value_id,
                max(available_at) FILTER (WHERE factor_id = ?)
                    AS revenue_growth_available_at
            FROM ranked
            WHERE revision_rank = 1
            GROUP BY security_id,as_of_date
        )
        SELECT *,greatest(sue_available_at,revenue_growth_available_at)
            AS decision_available_at
        FROM paired
        WHERE sue_value IS NOT NULL AND revenue_growth_value IS NOT NULL
        ORDER BY as_of_date,security_id
        """,
        [
            *params,
            SUE_FACTOR_ID,
            SUE_SOURCE_NAME,
            REVENUE_GROWTH_FACTOR_ID,
            REVENUE_GROWTH_SOURCE_NAME,
            SUE_FACTOR_ID,
            SUE_FACTOR_ID,
            SUE_FACTOR_ID,
            REVENUE_GROWTH_FACTOR_ID,
            REVENUE_GROWTH_FACTOR_ID,
            REVENUE_GROWTH_FACTOR_ID,
        ],
    ).df()


def _lineage(row: pd.Series) -> str:
    return json_dumps(
        {
            "method": "same_sign_direct_revenue_growth_gate_then_sue",
            "gate": "sue_value * revenue_growth_value > 0",
            "sue": {
                "factor_id": SUE_FACTOR_ID,
                "factor_value_id": row["sue_factor_value_id"],
                "value": row["sue_value"],
                "available_at": row["sue_available_at"],
            },
            "revenue_growth": {
                "factor_id": REVENUE_GROWTH_FACTOR_ID,
                "factor_value_id": row["revenue_growth_factor_value_id"],
                "value": row["revenue_growth_value"],
                "available_at": row["revenue_growth_available_at"],
            },
            "return_fitted_parameters": False,
        }
    )


def compute_earnings_revenue_growth_agreement_rows(
    inputs: pd.DataFrame,
    options: EarningsRevenueGrowthAgreementOptions | None = None,
) -> pd.DataFrame:
    """Keep and restandardize SUE only when direct revenue growth agrees in sign."""

    options = options or EarningsRevenueGrowthAgreementOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id",
        "symbol",
        "as_of_date",
        "decision_available_at",
        "sue_value",
        "revenue_growth_value",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"SUE revenue-growth agreement inputs missing columns: {missing}")
    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["decision_available_at"], errors="coerce")
    for column in ("sue_value", "revenue_growth_value"):
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(
        subset=["security_id", "as_of_date", "available_at", "sue_value", "revenue_growth_value"]
    )
    rows = rows[
        rows["sue_value"].map(math.isfinite)
        & rows["revenue_growth_value"].map(math.isfinite)
        & ((rows["sue_value"] * rows["revenue_growth_value"]) > 0)
    ].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["sue_value"].astype(float)
    rows = zscore(
        rows,
        value_column="raw_value",
        output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    rows["input_ids_json"] = json_dumps(
        [f"factor:{SUE_FACTOR_ID}", f"factor:{REVENUE_GROWTH_FACTOR_ID}"]
    )
    rows["input_lineage_json"] = rows.apply(_lineage, axis=1)
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


def refresh_earnings_revenue_growth_agreement_values(
    store: DuckDBStore,
    options: EarningsRevenueGrowthAgreementOptions | None = None,
) -> int:
    """Materialize the direct-revenue-growth-confirmed SUE sleeve."""

    options = options or EarningsRevenueGrowthAgreementOptions()
    store.initialize()
    rows = compute_earnings_revenue_growth_agreement_rows(
        load_earnings_revenue_growth_agreement_inputs(store, options), options
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
                "earnings_revenue_growth_agreement_insert",
            )
    return len(rows)
