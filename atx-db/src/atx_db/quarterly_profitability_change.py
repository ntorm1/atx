"""Point-in-time same-quarter change in quarterly operating profitability."""

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
from .quarterly_revenue_growth import FACTOR_ID as REVENUE_GROWTH_FACTOR_ID
from .quarterly_revenue_growth import SOURCE_NAME as REVENUE_GROWTH_SOURCE_NAME
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT quarterly operating profitability change v1"
FACTOR_ID = "profitability_quarterly_operating_profitability_change_yoy"
FACTOR_NAME = "PIT same-quarter change in quarterly operating profitability"
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
class QuarterlyProfitabilityChangeOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    maximum_absolute_change: float = 10.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_quarterly_profitability_change_inputs(
    store: DuckDBStore,
    options: QuarterlyProfitabilityChangeOptions | None = None,
) -> pd.DataFrame:
    """Load exact current/prior QOP rows from governed revenue-growth pairs."""

    options = options or QuarterlyProfitabilityChangeOptions()
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
        WITH revenue_pairs AS (
            SELECT
                factor_value_id AS revenue_growth_factor_value_id,
                security_id,
                symbol,
                as_of_date,
                available_at AS revenue_growth_available_at,
                json_extract_string(
                    input_lineage_json,'$.current.qop_factor_value_id'
                ) AS current_qop_factor_value_id,
                json_extract_string(
                    input_lineage_json,'$.prior_year.qop_factor_value_id'
                ) AS prior_qop_factor_value_id,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.current.period_end'
                ) AS DATE) AS current_period_end,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.prior_year.period_end'
                ) AS DATE) AS prior_period_end,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.period_gap_days'
                ) AS INTEGER) AS period_gap_days
            FROM fundamental_factor_values
            WHERE {' AND '.join(predicates)}
        ),
        qop AS (
            SELECT
                factor_value_id AS qop_factor_value_id,
                raw_value AS qop_raw_value,
                value AS qop_value,
                available_at AS qop_available_at
            FROM fundamental_factor_values
            WHERE factor_id = ? AND source = ? AND is_latest_revision
        )
        SELECT
            r.*,
            cur.qop_raw_value AS current_qop_raw_value,
            cur.qop_value AS current_qop_value,
            cur.qop_available_at AS current_qop_available_at,
            old.qop_raw_value AS prior_qop_raw_value,
            old.qop_value AS prior_qop_value,
            old.qop_available_at AS prior_qop_available_at
        FROM revenue_pairs r
        JOIN qop cur ON cur.qop_factor_value_id = r.current_qop_factor_value_id
        JOIN qop old ON old.qop_factor_value_id = r.prior_qop_factor_value_id
        ORDER BY r.as_of_date,r.security_id
        """,
        [*params, QOP_FACTOR_ID, QOP_SOURCE_NAME],
    ).df()


def _lineage(
    row: pd.Series,
    options: QuarterlyProfitabilityChangeOptions,
) -> str:
    return json_dumps(
        {
            "method": "same_quarter_operating_profitability_change_pit",
            "formula": "quarterly_operating_profitability_t-quarterly_operating_profitability_t_4",
            "orientation": "higher_profitability_change_is_preferred",
            "pairing": {
                "factor_id": REVENUE_GROWTH_FACTOR_ID,
                "factor_value_id": row["revenue_growth_factor_value_id"],
                "period_gap_days": row.get("period_gap_days"),
                "available_at": row["revenue_growth_available_at"],
            },
            "current": {
                "qop_factor_value_id": row["current_qop_factor_value_id"],
                "period_end": row.get("current_period_end"),
                "raw_value": row["current_qop_raw_value"],
                "value": row["current_qop_value"],
                "available_at": row["current_qop_available_at"],
            },
            "prior_year": {
                "qop_factor_value_id": row["prior_qop_factor_value_id"],
                "period_end": row.get("prior_period_end"),
                "raw_value": row["prior_qop_raw_value"],
                "value": row["prior_qop_value"],
                "available_at": row["prior_qop_available_at"],
            },
            "profitability_change": row["raw_value"],
            "research_contract": {
                "maximum_absolute_change": options.maximum_absolute_change,
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "return_fitted_parameters": False,
            },
        }
    )


def compute_quarterly_profitability_change_rows(
    inputs: pd.DataFrame,
    options: QuarterlyProfitabilityChangeOptions | None = None,
) -> pd.DataFrame:
    """Compute, winsorize, and standardize same-quarter profitability change."""

    options = options or QuarterlyProfitabilityChangeOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "revenue_growth_factor_value_id",
        "current_qop_factor_value_id",
        "prior_qop_factor_value_id",
        "security_id",
        "symbol",
        "as_of_date",
        "revenue_growth_available_at",
        "current_qop_raw_value",
        "current_qop_available_at",
        "prior_qop_raw_value",
        "prior_qop_available_at",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"quarterly profitability change inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    for column in (
        "revenue_growth_available_at",
        "current_qop_available_at",
        "prior_qop_available_at",
    ):
        rows[column] = pd.to_datetime(rows[column], errors="coerce")
    for column in ("current_qop_raw_value", "prior_qop_raw_value"):
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(subset=sorted(required))
    rows = rows[
        rows["current_qop_raw_value"].map(math.isfinite)
        & rows["prior_qop_raw_value"].map(math.isfinite)
    ].copy()
    rows["raw_value"] = rows["current_qop_raw_value"] - rows["prior_qop_raw_value"]
    rows = rows[
        rows["raw_value"].map(math.isfinite)
        & (rows["raw_value"].abs() <= options.maximum_absolute_change)
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
        [
            "revenue_growth_available_at",
            "current_qop_available_at",
            "prior_qop_available_at",
        ]
    ].max(axis=1)
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


def refresh_quarterly_profitability_change_values(
    store: DuckDBStore,
    options: QuarterlyProfitabilityChangeOptions | None = None,
) -> int:
    """Materialize the same-quarter profitability-change feature."""

    options = options or QuarterlyProfitabilityChangeOptions()
    store.initialize()
    rows = compute_quarterly_profitability_change_rows(
        load_quarterly_profitability_change_inputs(store, options), options
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
                "quarterly_profitability_change_insert",
            )
    return len(rows)
