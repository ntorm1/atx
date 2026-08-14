"""Point-in-time abnormal receivables growth relative to revenue growth."""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import winsorize, zscore
from .quarterly_revenue_growth import FACTOR_ID as REVENUE_GROWTH_FACTOR_ID
from .quarterly_revenue_growth import SOURCE_NAME as REVENUE_GROWTH_SOURCE
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT abnormal receivables growth v1"
FACTOR_ID = "quality_low_abnormal_receivables_growth"
FACTOR_NAME = "PIT low abnormal receivables growth"
FACTOR_FAMILY = "fundamental_quality"
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
class AbnormalReceivablesGrowthOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    maximum_absolute_receivables_growth: float = 10.0
    maximum_absolute_abnormal_growth: float = 10.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_abnormal_receivables_growth_inputs(
    store: DuckDBStore,
    options: AbnormalReceivablesGrowthOptions | None = None,
) -> pd.DataFrame:
    """Attach latest visible receivables facts to governed YoY revenue-growth rows."""

    options = options or AbnormalReceivablesGrowthOptions()
    predicates = ["factor_id = ?", "source = ?", "is_latest_revision"]
    params: list[object] = [REVENUE_GROWTH_FACTOR_ID, REVENUE_GROWTH_SOURCE]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    return store.con.execute(
        f"""
        WITH revenue_base AS (
            SELECT
                factor_value_id AS revenue_growth_factor_value_id,
                security_id,
                symbol,
                as_of_date,
                available_at AS decision_available_at,
                raw_value AS revenue_growth,
                CAST(json_extract_string(
                    input_lineage_json,'$.current.period_end'
                ) AS DATE) AS current_period_end,
                CAST(json_extract_string(
                    input_lineage_json,'$.prior_year.period_end'
                ) AS DATE) AS prior_period_end,
                CAST(json_extract_string(
                    input_lineage_json,'$.current.revenue.value'
                ) AS DOUBLE) AS current_revenue,
                json_extract_string(
                    input_lineage_json,'$.current.revenue.id'
                ) AS current_revenue_id,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.current.revenue.available_at'
                ) AS TIMESTAMP) AS current_revenue_available_at,
                CAST(json_extract_string(
                    input_lineage_json,'$.prior_year.revenue.value'
                ) AS DOUBLE) AS prior_revenue,
                json_extract_string(
                    input_lineage_json,'$.prior_year.revenue.id'
                ) AS prior_revenue_id,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.prior_year.revenue.available_at'
                ) AS TIMESTAMP) AS prior_revenue_available_at
            FROM fundamental_factor_values
            WHERE {' AND '.join(predicates)}
              AND raw_value IS NOT NULL
              AND isfinite(raw_value)
        ),
        receivables_facts AS (
            SELECT
                security_id,
                period_end,
                value AS receivables,
                statement_point_id AS receivables_id,
                available_at AS receivables_available_at,
                revision_sequence
            FROM fundamental_statement_points
            WHERE canonical_metric = 'ar'
              AND unit = 'USD'
              AND period_type = 'instant'
              AND period_end IS NOT NULL
              AND value IS NOT NULL
              AND isfinite(value)
              AND available_at IS NOT NULL
        ),
        current_candidates AS (
            SELECT
                b.*,
                r.receivables AS current_receivables,
                r.receivables_id AS current_receivables_id,
                r.receivables_available_at AS current_receivables_available_at,
                row_number() OVER (
                    PARTITION BY b.revenue_growth_factor_value_id
                    ORDER BY r.receivables_available_at DESC NULLS LAST,
                             r.revision_sequence DESC NULLS LAST,
                             r.receivables_id DESC NULLS LAST
                ) AS current_rank
            FROM revenue_base b
            LEFT JOIN receivables_facts r
              ON r.security_id = b.security_id
             AND r.period_end = b.current_period_end
             AND r.receivables_available_at <= b.decision_available_at
        ),
        current_receivables AS (
            SELECT * EXCLUDE (current_rank)
            FROM current_candidates
            WHERE current_rank = 1
        ),
        prior_candidates AS (
            SELECT
                b.*,
                r.receivables AS prior_receivables,
                r.receivables_id AS prior_receivables_id,
                r.receivables_available_at AS prior_receivables_available_at,
                row_number() OVER (
                    PARTITION BY b.revenue_growth_factor_value_id
                    ORDER BY r.receivables_available_at DESC NULLS LAST,
                             r.revision_sequence DESC NULLS LAST,
                             r.receivables_id DESC NULLS LAST
                ) AS prior_rank
            FROM current_receivables b
            LEFT JOIN receivables_facts r
              ON r.security_id = b.security_id
             AND r.period_end = b.prior_period_end
             AND r.receivables_available_at <= b.decision_available_at
        )
        SELECT * EXCLUDE (prior_rank)
        FROM prior_candidates
        WHERE prior_rank = 1
        ORDER BY as_of_date,security_id
        """,
        params,
    ).df()


def _lineage(row: pd.Series, options: AbnormalReceivablesGrowthOptions) -> str:
    return json_dumps(
        {
            "method": "receivables_growth_minus_revenue_growth_yoy_pit",
            "formula": (
                "(receivables_t/receivables_t_4-1)"
                "-(revenue_t/revenue_t_4-1)"
            ),
            "orientation": "lower_abnormal_receivables_growth_is_preferred",
            "research_contract": {
                "same_quarter_prior_year": True,
                "maximum_absolute_receivables_growth": (
                    options.maximum_absolute_receivables_growth
                ),
                "maximum_absolute_abnormal_growth": (
                    options.maximum_absolute_abnormal_growth
                ),
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "missing_receivables_imputed": False,
                "return_fitted_parameters": False,
            },
            "revenue_growth_input": {
                "factor_id": REVENUE_GROWTH_FACTOR_ID,
                "factor_value_id": row["revenue_growth_factor_value_id"],
                "source": REVENUE_GROWTH_SOURCE,
                "available_at": row["decision_available_at"],
                "growth": row["revenue_growth"],
            },
            "periods": {
                "current": row.get("current_period_end"),
                "prior_year": row.get("prior_period_end"),
            },
            "receivables": {
                "current_value": row["current_receivables"],
                "current_id": row.get("current_receivables_id"),
                "current_available_at": row.get("current_receivables_available_at"),
                "prior_value": row["prior_receivables"],
                "prior_id": row.get("prior_receivables_id"),
                "prior_available_at": row.get("prior_receivables_available_at"),
                "growth": row["receivables_growth"],
            },
            "revenue": {
                "current_value": row["current_revenue"],
                "current_id": row.get("current_revenue_id"),
                "current_available_at": row.get("current_revenue_available_at"),
                "prior_value": row["prior_revenue"],
                "prior_id": row.get("prior_revenue_id"),
                "prior_available_at": row.get("prior_revenue_available_at"),
                "growth": row["revenue_growth"],
            },
            "abnormal_receivables_growth": row["raw_value"],
        }
    )


def compute_abnormal_receivables_growth_rows(
    inputs: pd.DataFrame,
    options: AbnormalReceivablesGrowthOptions | None = None,
) -> pd.DataFrame:
    """Compute, orient, winsorize, and standardize abnormal receivables growth."""

    options = options or AbnormalReceivablesGrowthOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "revenue_growth_factor_value_id",
        "security_id",
        "symbol",
        "as_of_date",
        "decision_available_at",
        "revenue_growth",
        "current_revenue",
        "prior_revenue",
        "current_receivables",
        "prior_receivables",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"abnormal receivables growth inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    timestamp_columns = (
        "decision_available_at",
        "current_revenue_available_at",
        "prior_revenue_available_at",
        "current_receivables_available_at",
        "prior_receivables_available_at",
    )
    for column in timestamp_columns:
        if column in rows.columns:
            rows[column] = pd.to_datetime(rows[column], errors="coerce")
    numeric_columns = (
        "revenue_growth",
        "current_revenue",
        "prior_revenue",
        "current_receivables",
        "prior_receivables",
    )
    for column in numeric_columns:
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(
        subset=["security_id", "as_of_date", "decision_available_at", *numeric_columns]
    ).copy()
    valid = pd.Series(True, index=rows.index)
    for column in numeric_columns:
        valid &= rows[column].map(math.isfinite)
    rows = rows[
        valid
        & (rows["current_revenue"] > 0)
        & (rows["prior_revenue"] > 0)
        & (rows["current_receivables"] > 0)
        & (rows["prior_receivables"] > 0)
    ].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["receivables_growth"] = (
        rows["current_receivables"] / rows["prior_receivables"] - 1.0
    )
    rows["raw_value"] = rows["receivables_growth"] - rows["revenue_growth"]
    rows = rows[
        rows["receivables_growth"].map(math.isfinite)
        & rows["raw_value"].map(math.isfinite)
        & (
            rows["receivables_growth"].abs()
            <= options.maximum_absolute_receivables_growth
        )
        & (rows["raw_value"].abs() <= options.maximum_absolute_abnormal_growth)
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
    available_columns = [column for column in timestamp_columns if column in rows.columns]
    rows["available_at"] = rows[available_columns].max(axis=1)
    rows["input_ids_json"] = json_dumps(
        [f"factor:{REVENUE_GROWTH_FACTOR_ID}", "metric:ar"]
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


def refresh_abnormal_receivables_growth_values(
    store: DuckDBStore,
    options: AbnormalReceivablesGrowthOptions | None = None,
) -> int:
    """Materialize the point-in-time abnormal receivables-growth factor."""

    options = options or AbnormalReceivablesGrowthOptions()
    store.initialize()
    rows = compute_abnormal_receivables_growth_rows(
        load_abnormal_receivables_growth_inputs(store, options), options
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
                "abnormal_receivables_growth_insert",
            )
    return len(rows)
