"""Point-in-time four-quarter change in q-factor-style quarterly ROE."""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import winsorize, zscore
from .quarterly_roe import FACTOR_ID as QUARTERLY_ROE_FACTOR_ID
from .quarterly_roe import SOURCE_NAME as QUARTERLY_ROE_SOURCE_NAME
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT four-quarter change in ROE v1"
FACTOR_ID = "profitability_q_factor_delta_roe"
FACTOR_NAME = "PIT four-quarter change in quarterly ROE"
FACTOR_FAMILY = "fundamental_profitability_growth"
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
class DeltaRoeOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_period_gap_days: int = 300
    maximum_period_gap_days: int = 430
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_delta_roe_inputs(
    store: DuckDBStore,
    options: DeltaRoeOptions | None = None,
) -> pd.DataFrame:
    """Match each visible quarterly ROE decision to its four-quarter lag."""

    options = options or DeltaRoeOptions()
    predicates: list[str] = []
    params: list[object] = [
        QUARTERLY_ROE_FACTOR_ID,
        QUARTERLY_ROE_SOURCE_NAME,
    ]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    date_sql = " AND " + " AND ".join(predicates) if predicates else ""
    sql = f"""
        WITH roe AS (
            SELECT
                factor_value_id AS roe_factor_value_id,
                security_id,
                symbol,
                as_of_date,
                raw_value AS quarterly_roe,
                available_at,
                CAST(
                    json_extract_string(input_lineage_json, '$.earnings.period_end')
                    AS DATE
                ) AS earnings_period_end,
                row_number() OVER (
                    PARTITION BY security_id, as_of_date
                    ORDER BY available_at DESC, source_loaded_at DESC,
                             factor_value_id DESC
                ) AS revision_rank
            FROM fundamental_factor_values
            WHERE factor_id = ?
              AND source = ?
              AND is_latest_revision
              AND raw_value IS NOT NULL
              AND isfinite(raw_value)
        ),
        current_roe AS (
            SELECT * EXCLUDE (revision_rank)
            FROM roe
            WHERE revision_rank = 1 {date_sql}
        ),
        prior_candidates AS (
            SELECT
                c.*,
                p.roe_factor_value_id AS prior_roe_factor_value_id,
                p.as_of_date AS prior_roe_as_of_date,
                p.quarterly_roe AS prior_quarterly_roe,
                p.available_at AS prior_roe_available_at,
                p.earnings_period_end AS prior_earnings_period_end,
                row_number() OVER (
                    PARTITION BY c.roe_factor_value_id
                    ORDER BY abs((c.earnings_period_end - p.earnings_period_end) - 365),
                             p.earnings_period_end DESC,
                             p.as_of_date DESC,
                             p.available_at DESC,
                             p.roe_factor_value_id DESC
                ) AS prior_rank
            FROM current_roe c
            JOIN roe p
              ON p.security_id = c.security_id
             AND p.revision_rank = 1
             AND p.earnings_period_end < c.earnings_period_end
             AND c.earnings_period_end - p.earnings_period_end BETWEEN ? AND ?
             AND p.as_of_date < c.as_of_date
             AND p.available_at <= c.available_at
        )
        SELECT * EXCLUDE (prior_rank)
        FROM prior_candidates
        WHERE prior_rank = 1
        ORDER BY as_of_date, security_id
    """
    return store.con.execute(
        sql,
        [
            *params,
            options.minimum_period_gap_days,
            options.maximum_period_gap_days,
        ],
    ).df()


def _lineage(row: pd.Series, options: DeltaRoeOptions) -> str:
    return json_dumps(
        {
            "method": "hou_mo_xue_zhang_four_quarter_delta_roe_pit",
            "formula": "quarterly_roe_t - quarterly_roe_t_minus_4",
            "orientation": "higher_is_improving_profitability",
            "research_contract": {
                "period_gap_days": [
                    options.minimum_period_gap_days,
                    options.maximum_period_gap_days,
                ],
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "complete_case": True,
                "return_fitted_parameters": False,
            },
            "current": {
                "factor_id": QUARTERLY_ROE_FACTOR_ID,
                "factor_value_id": row["roe_factor_value_id"],
                "as_of_date": row["as_of_date"],
                "earnings_period_end": row["earnings_period_end"],
                "quarterly_roe": row["quarterly_roe"],
                "available_at": row["available_at"],
            },
            "prior_four_quarter": {
                "factor_id": QUARTERLY_ROE_FACTOR_ID,
                "factor_value_id": row["prior_roe_factor_value_id"],
                "as_of_date": row["prior_roe_as_of_date"],
                "earnings_period_end": row["prior_earnings_period_end"],
                "quarterly_roe": row["prior_quarterly_roe"],
                "available_at": row["prior_roe_available_at"],
            },
            "delta_roe": row["raw_value"],
        }
    )


def compute_delta_roe_rows(
    inputs: pd.DataFrame,
    options: DeltaRoeOptions | None = None,
) -> pd.DataFrame:
    """Winsorize and standardize four-quarter change in quarterly ROE."""

    options = options or DeltaRoeOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "roe_factor_value_id",
        "prior_roe_factor_value_id",
        "security_id",
        "symbol",
        "as_of_date",
        "quarterly_roe",
        "prior_quarterly_roe",
        "available_at",
        "prior_roe_available_at",
        "earnings_period_end",
        "prior_earnings_period_end",
        "prior_roe_as_of_date",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"Delta-ROE inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["available_at"], errors="coerce")
    rows["quarterly_roe"] = pd.to_numeric(rows["quarterly_roe"], errors="coerce")
    rows["prior_quarterly_roe"] = pd.to_numeric(
        rows["prior_quarterly_roe"], errors="coerce"
    )
    rows = rows.dropna(
        subset=[
            "security_id",
            "as_of_date",
            "available_at",
            "quarterly_roe",
            "prior_quarterly_roe",
        ]
    )
    finite = rows[["quarterly_roe", "prior_quarterly_roe"]].apply(
        lambda column: column.map(math.isfinite)
    ).all(axis=1)
    rows = rows[finite].copy()
    rows["delta_roe"] = rows["quarterly_roe"] - rows["prior_quarterly_roe"]
    rows = rows[rows["delta_roe"].map(math.isfinite)].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["delta_roe"]
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
    rows["input_ids_json"] = json_dumps([f"factor:{QUARTERLY_ROE_FACTOR_ID}"])
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


def refresh_delta_roe_values(
    store: DuckDBStore,
    options: DeltaRoeOptions | None = None,
) -> int:
    """Materialize point-in-time four-quarter change in quarterly ROE."""

    options = options or DeltaRoeOptions()
    store.initialize()
    rows = compute_delta_roe_rows(load_delta_roe_inputs(store, options), options)
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
            insert_frame(store, rows, "fundamental_factor_values", "delta_roe_insert")
    return len(rows)
