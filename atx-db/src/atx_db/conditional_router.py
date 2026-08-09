"""Conditional operating-profitability / net-issuance factor router.

Operating profitability remains the preferred signal. Net share issuance is used only
for a security/date where operating profitability is unavailable, expanding breadth
without averaging away the stronger input on their common cohort.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import zscore
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db conditional OP/issuance router v3"
FACTOR_ID = "composite_operating_profitability_or_net_issuance"
FACTOR_NAME = "Conditional operating profitability or net issuance"
FACTOR_FAMILY = "fundamental_composite"
PRIMARY_FACTOR_ID = "profitability_operating_profitability"
FALLBACK_FACTOR_ID = "financing_low_net_share_issuance"

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
class ConditionalRouterOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_names_per_date: int = 20
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_conditional_router_inputs(
    store: DuckDBStore,
    options: ConditionalRouterOptions | None = None,
) -> pd.DataFrame:
    """Choose the primary factor when present and the fallback otherwise."""

    options = options or ConditionalRouterOptions()
    predicates = ["factor_id IN (?, ?)", "is_latest_revision"]
    params: list[object] = [PRIMARY_FACTOR_ID, FALLBACK_FACTOR_ID]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    return store.con.execute(
        f"""
        WITH ranked AS (
            SELECT
                factor_value_id AS input_factor_value_id,
                factor_id AS input_factor_id,
                factor_name AS input_factor_name,
                security_id,
                symbol,
                as_of_date,
                raw_value AS input_raw_value,
                value AS input_value,
                available_at AS input_available_at,
                source AS input_source,
                source_loaded_at AS input_source_loaded_at,
                row_number() OVER (
                    PARTITION BY factor_id, security_id, as_of_date
                    ORDER BY available_at DESC, source_loaded_at DESC, factor_value_id DESC
                ) AS revision_rank
            FROM fundamental_factor_values
            WHERE {' AND '.join(predicates)}
              AND value IS NOT NULL
              AND isfinite(value)
        ),
        chosen AS (
            SELECT
                *,
                CASE WHEN input_factor_id = ? THEN 'primary' ELSE 'fallback' END AS route,
                row_number() OVER (
                    PARTITION BY security_id, as_of_date
                    ORDER BY CASE WHEN input_factor_id = ? THEN 0 ELSE 1 END,
                             input_available_at DESC,
                             input_factor_value_id DESC
                ) AS route_rank
            FROM ranked
            WHERE revision_rank = 1
        )
        SELECT * EXCLUDE (revision_rank, route_rank)
        FROM chosen
        WHERE route_rank = 1
        ORDER BY as_of_date, security_id
        """,
        [*params, PRIMARY_FACTOR_ID, PRIMARY_FACTOR_ID],
    ).df()


def _lineage(row: pd.Series) -> str:
    return json_dumps(
        {
            "method": "primary_else_fallback",
            "primary_factor_id": PRIMARY_FACTOR_ID,
            "fallback_factor_id": FALLBACK_FACTOR_ID,
            "selected_route": row["route"],
            "selected_factor": {
                "factor_id": row["input_factor_id"],
                "factor_value_id": row["input_factor_value_id"],
                "raw_value": row["input_raw_value"],
                "value": row["input_value"],
                "available_at": row["input_available_at"],
                "source": row["input_source"],
            },
        }
    )


def compute_conditional_router_rows(
    inputs: pd.DataFrame,
    options: ConditionalRouterOptions | None = None,
) -> pd.DataFrame:
    """Standardize the selected upstream score across the expanded monthly cohort."""

    options = options or ConditionalRouterOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "input_factor_value_id",
        "input_factor_id",
        "security_id",
        "symbol",
        "as_of_date",
        "input_value",
        "input_available_at",
        "route",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"conditional router inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["input_available_at"], errors="coerce")
    rows["input_value"] = pd.to_numeric(rows["input_value"], errors="coerce")
    rows = rows.dropna(subset=["security_id", "as_of_date", "available_at", "input_value"])
    rows = rows[rows["input_value"].map(math.isfinite)].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["input_value"].astype(float)
    rows = zscore(
        rows,
        value_column="raw_value",
        output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    rows["input_ids_json"] = json_dumps(
        [
            f"factor:{PRIMARY_FACTOR_ID}",
            f"factor:{FALLBACK_FACTOR_ID}",
        ]
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


def _delete_scope(store: DuckDBStore, options: ConditionalRouterOptions) -> None:
    predicates = ["factor_id = ?"]
    params: list[object] = [FACTOR_ID]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    store.con.execute(
        f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(predicates)}",
        params,
    )


def refresh_conditional_router_values(
    store: DuckDBStore,
    options: ConditionalRouterOptions | None = None,
) -> int:
    """Materialize the monthly point-in-time conditional factor."""

    options = options or ConditionalRouterOptions()
    store.initialize()
    inputs = load_conditional_router_inputs(store, options)
    rows = compute_conditional_router_rows(inputs, options)
    with store.transaction():
        _delete_scope(store, options)
        if not rows.empty:
            insert_frame(
                store,
                rows,
                "fundamental_factor_values",
                "conditional_router_values_insert",
            )
    return len(rows)
