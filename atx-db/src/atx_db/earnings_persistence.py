"""Point-in-time earnings surprise weighted by ex-ante earnings stability."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .earnings_surprise import FACTOR_ID as SUE_FACTOR_ID
from .earnings_surprise import SOURCE_NAME as SUE_SOURCE_NAME
from .factors.cross_section import winsorize, zscore
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT earnings-persistence-weighted SUE v1"
FACTOR_ID = "earnings_sue_low_volatility_persistence"
FACTOR_NAME = "PIT earnings-persistence-weighted SUE"
FACTOR_FAMILY = "fundamental_earnings"

_OUTPUT_COLUMNS = [
    "factor_value_id", "factor_id", "factor_name", "family", "security_id",
    "symbol", "as_of_date", "raw_value", "value", "available_at",
    "input_ids_json", "input_lineage_json", "is_latest_revision", "run_id", "source",
]


@dataclass(frozen=True)
class EarningsPersistenceOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None

    def __post_init__(self) -> None:
        if self.minimum_names_per_date < 3:
            raise ValueError("minimum_names_per_date must be at least 3")
        if not 0 <= self.winsor_limit < 0.5:
            raise ValueError("winsor_limit must be in [0, 0.5)")


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_earnings_persistence_inputs(
    store: DuckDBStore,
    options: EarningsPersistenceOptions | None = None,
) -> pd.DataFrame:
    """Load governed SUE and its strictly prior seasonal-change volatility."""

    options = options or EarningsPersistenceOptions()
    predicates = [
        "factor_id = ?", "source = ?", "is_latest_revision",
        "value IS NOT NULL", "isfinite(value)", "input_lineage_json IS NOT NULL",
    ]
    params: list[object] = [SUE_FACTOR_ID, SUE_SOURCE_NAME]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    return store.con.execute(
        f"""
        WITH revisions AS (
            SELECT
                factor_value_id AS sue_factor_value_id,
                security_id,
                symbol,
                as_of_date,
                raw_value AS sue_raw_value,
                value AS sue_value,
                available_at AS sue_available_at,
                input_lineage_json AS sue_lineage_json,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.standardization.historical_std'
                ) AS DOUBLE) AS historical_std,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.standardization.history_observations'
                ) AS INTEGER) AS history_observations,
                row_number() OVER (
                    PARTITION BY security_id,as_of_date
                    ORDER BY available_at DESC,source_loaded_at DESC,factor_value_id DESC
                ) AS revision_rank
            FROM fundamental_factor_values
            WHERE {' AND '.join(predicates)}
        )
        SELECT * EXCLUDE (revision_rank)
        FROM revisions
        WHERE revision_rank=1
          AND historical_std IS NOT NULL
          AND isfinite(historical_std)
          AND historical_std>0
          AND history_observations>=2
        ORDER BY as_of_date,security_id
        """,
        params,
    ).df()


def _decode_json(value: object) -> object:
    if not isinstance(value, str):
        return value
    try:
        return json.loads(value)
    except json.JSONDecodeError:
        return value


def _lineage(row: pd.Series) -> str:
    return json_dumps(
        {
            "method": "sue_direction_scaled_by_low_earnings_volatility_percentile",
            "formula": "(2*rank_cs(SUE)-1) * rank_cs(-prior_seasonal_change_std)",
            "orientation": "higher_is_positive_persistent_surprise",
            "sue": {
                "factor_value_id": row["sue_factor_value_id"],
                "raw_value": row["sue_raw_value"],
                "value": row["sue_value"],
                "rank": row["sue_rank"],
                "centered_rank": row["sue_score"],
                "available_at": row["sue_available_at"],
                "lineage": _decode_json(row.get("sue_lineage_json")),
            },
            "persistence_proxy": {
                "historical_seasonal_change_std": row["historical_std"],
                "history_observations": row["history_observations"],
                "low_volatility_percentile": row["stability_rank"],
            },
            "cross_section_names": row["cross_section_names"],
            "raw_persistence_weighted_sue": row["raw_value"],
            "research_contract": {
                "hard_volatility_filter": False,
                "return_fitted_parameters": False,
                "missing_volatility_policy": "drop",
            },
        }
    )


def compute_earnings_persistence_rows(
    inputs: pd.DataFrame,
    options: EarningsPersistenceOptions | None = None,
) -> pd.DataFrame:
    """Scale SUE rank continuously by the percentile of low ex-ante volatility."""

    options = options or EarningsPersistenceOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "security_id", "symbol", "as_of_date", "sue_available_at",
        "sue_value", "historical_std", "history_observations",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"earnings persistence inputs missing columns: {missing}")
    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["sue_available_at"], errors="coerce")
    for column in ("sue_value", "historical_std"):
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows["history_observations"] = pd.to_numeric(rows["history_observations"], errors="coerce")
    rows = rows.dropna(
        subset=["security_id", "as_of_date", "available_at", "sue_value", "historical_std"]
    )
    rows = rows[
        rows["sue_value"].map(math.isfinite)
        & rows["historical_std"].map(math.isfinite)
        & (rows["historical_std"] > 0)
        & (rows["history_observations"] >= 2)
    ].copy()
    rows["cross_section_names"] = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[rows["cross_section_names"] >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    grouped = rows.groupby("as_of_date", sort=False)
    rows["sue_rank"] = grouped["sue_value"].rank(method="average", pct=True)
    rows["stability_rank"] = grouped["historical_std"].rank(
        method="average", pct=True, ascending=False
    )
    rows["sue_score"] = 2.0 * rows["sue_rank"] - 1.0
    rows["raw_value"] = rows["sue_score"] * rows["stability_rank"]
    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows = winsorize(
        rows, value_column="raw_value", output_column="raw_value",
        partition_columns=("factor_id", "as_of_date"), limits=options.winsor_limit,
    )
    rows = zscore(
        rows, value_column="raw_value", output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    rows["input_ids_json"] = json_dumps([f"factor:{SUE_FACTOR_ID}"])
    rows["input_lineage_json"] = rows.apply(_lineage, axis=1)
    rows["is_latest_revision"] = True
    rows["run_id"] = options.run_id
    rows["source"] = options.source
    rows["factor_value_id"] = [
        _factor_value_id(options.source, security_id, as_of_date)
        for security_id, as_of_date in zip(rows["security_id"], rows["as_of_date"], strict=True)
    ]
    return (
        rows[_OUTPUT_COLUMNS]
        .dropna(subset=["value"])
        .sort_values(["as_of_date", "security_id"], kind="stable")
        .reset_index(drop=True)
    )


def refresh_earnings_persistence_values(
    store: DuckDBStore,
    options: EarningsPersistenceOptions | None = None,
) -> int:
    """Materialize earnings-persistence-weighted SUE."""

    options = options or EarningsPersistenceOptions()
    store.initialize()
    rows = compute_earnings_persistence_rows(load_earnings_persistence_inputs(store, options), options)
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
            f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(predicates)}", params
        )
        if not rows.empty:
            insert_frame(store, rows, "fundamental_factor_values", "earnings_persistence_insert")
    return len(rows)
