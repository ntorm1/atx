"""Continuous rank-standardized financial strength from annual PIT fundamentals."""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import zscore
from .piotroski import (
    BOOK_TO_MARKET_FACTOR_ID,
)
from .piotroski import (
    FACTOR_ID as PIOTROSKI_FACTOR_ID,
)
from .piotroski import (
    SOURCE_NAME as PIOTROSKI_SOURCE_NAME,
)
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT continuous financial strength v1"
FACTOR_ID = "quality_continuous_financial_strength"
HIGH_BOOK_TO_MARKET_FACTOR_ID = (
    "quality_continuous_financial_strength_high_book_to_market"
)
FACTOR_IDS = (FACTOR_ID, HIGH_BOOK_TO_MARKET_FACTOR_ID)
FACTOR_METADATA = {
    FACTOR_ID: {
        "factor_name": "PIT continuous financial strength",
        "family": "fundamental_quality",
    },
    HIGH_BOOK_TO_MARKET_FACTOR_ID: {
        "factor_name": "PIT continuous financial strength within high book-to-market",
        "family": "fundamental_quality",
    },
}

COMPONENT_COLUMNS = (
    "roa",
    "cfo_to_beginning_assets",
    "delta_roa",
    "low_accruals",
    "low_delta_leverage",
    "delta_liquidity",
    "low_net_issuance",
    "delta_gross_margin",
    "delta_asset_turnover",
)
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
class ContinuousFinancialStrengthOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    maximum_fundamental_age_days: int = 550
    high_book_to_market_percentile: float = 0.80
    minimum_names_per_date: int = 20
    minimum_high_book_to_market_names_per_date: int = 10
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(
    source: str,
    factor_id: str,
    security_id: str,
    as_of_date: Any,
) -> str:
    payload = "|".join(str(part) for part in (source, factor_id, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def _rank_zscore(values: pd.Series, dates: pd.Series) -> pd.Series:
    ranks = values.groupby(dates).rank(method="average", na_option="keep")
    means = ranks.groupby(dates).transform("mean")
    standard_deviations = ranks.groupby(dates).transform(lambda group: group.std(ddof=1))
    result = (ranks - means) / standard_deviations
    return result.where(result.map(lambda value: pd.isna(value) or math.isfinite(value)))


def load_continuous_financial_strength_inputs(
    store: DuckDBStore,
    options: ContinuousFinancialStrengthOptions | None = None,
) -> pd.DataFrame:
    """Read compact component lineage from the production Piotroski surface."""

    options = options or ContinuousFinancialStrengthOptions()
    predicates = ["factor_id = ?", "source = ?", "is_latest_revision"]
    params: list[object] = [PIOTROSKI_FACTOR_ID, PIOTROSKI_SOURCE_NAME]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    sql = f"""
        SELECT
            factor_value_id AS upstream_piotroski_factor_value_id,
            security_id,
            symbol,
            as_of_date,
            available_at,
            CAST(json_extract_string(input_lineage_json, '$.ratios.roa') AS DOUBLE)
                AS roa,
            CAST(json_extract_string(input_lineage_json, '$.ratios.prior_roa') AS DOUBLE)
                AS prior_roa,
            CAST(
                json_extract_string(
                    input_lineage_json,
                    '$.ratios.cfo_to_beginning_assets'
                ) AS DOUBLE
            ) AS cfo_to_beginning_assets,
            CAST(json_extract_string(input_lineage_json, '$.ratios.leverage') AS DOUBLE)
                AS leverage,
            CAST(
                json_extract_string(input_lineage_json, '$.ratios.prior_leverage') AS DOUBLE
            ) AS prior_leverage,
            CAST(
                json_extract_string(input_lineage_json, '$.ratios.current_ratio') AS DOUBLE
            ) AS current_ratio,
            CAST(
                json_extract_string(input_lineage_json, '$.ratios.prior_current_ratio')
                AS DOUBLE
            ) AS prior_current_ratio,
            CAST(
                json_extract_string(input_lineage_json, '$.ratios.gross_margin') AS DOUBLE
            ) AS gross_margin,
            CAST(
                json_extract_string(input_lineage_json, '$.ratios.prior_gross_margin')
                AS DOUBLE
            ) AS prior_gross_margin,
            CAST(
                json_extract_string(input_lineage_json, '$.ratios.asset_turnover') AS DOUBLE
            ) AS asset_turnover,
            CAST(
                json_extract_string(input_lineage_json, '$.ratios.prior_asset_turnover')
                AS DOUBLE
            ) AS prior_asset_turnover,
            CAST(
                json_extract_string(input_lineage_json, '$.ratios.low_net_issuance')
                AS DOUBLE
            ) AS low_net_issuance,
            CAST(
                json_extract_string(
                    input_lineage_json,
                    '$.decision.book_to_market_percentile'
                ) AS DOUBLE
            ) AS book_to_market_percentile,
            CAST(
                json_extract_string(
                    input_lineage_json,
                    '$.decision.is_high_book_to_market'
                ) AS BOOLEAN
            ) AS is_high_book_to_market
        FROM fundamental_factor_values
        WHERE {' AND '.join(predicates)}
          AND value IS NOT NULL
          AND isfinite(value)
        ORDER BY as_of_date, security_id
    """
    return store.con.execute(sql, params).df()


def _calculate_continuous_components(
    inputs: pd.DataFrame,
) -> pd.DataFrame:
    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["available_at"], errors="coerce")
    rows = rows.dropna(subset=["security_id", "as_of_date", "available_at"])
    numeric = [
        "roa",
        "prior_roa",
        "cfo_to_beginning_assets",
        "leverage",
        "prior_leverage",
        "current_ratio",
        "prior_current_ratio",
        "gross_margin",
        "prior_gross_margin",
        "asset_turnover",
        "prior_asset_turnover",
        "low_net_issuance",
        "book_to_market_percentile",
    ]
    for column in numeric:
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(subset=numeric).copy()
    rows["delta_roa"] = rows["roa"] - rows["prior_roa"]
    rows["low_accruals"] = rows["cfo_to_beginning_assets"] - rows["roa"]
    rows["low_delta_leverage"] = rows["prior_leverage"] - rows["leverage"]
    rows["delta_liquidity"] = rows["current_ratio"] - rows["prior_current_ratio"]
    rows["delta_gross_margin"] = rows["gross_margin"] - rows["prior_gross_margin"]
    rows["delta_asset_turnover"] = (
        rows["asset_turnover"] - rows["prior_asset_turnover"]
    )
    standardized_columns: list[str] = []
    for component in COMPONENT_COLUMNS:
        standardized = f"{component}_rank_z"
        rows[standardized] = _rank_zscore(rows[component], rows["as_of_date"])
        standardized_columns.append(standardized)
    rows["continuous_rank_score"] = rows[standardized_columns].mean(axis=1)
    finite = rows["continuous_rank_score"].map(math.isfinite)
    return rows[finite].copy()


def _lineage(
    row: pd.Series,
    factor_id: str,
) -> str:
    return json_dumps(
        {
            "method": "equal_weight_rank_standardized_annual_financial_strength",
            "factor_id": factor_id,
            "research_contract": {
                "ranking": "average cross-sectional rank, sample z-score by date",
                "aggregation": "equal-weight mean of nine oriented component z-scores",
                "return_fitted_weights": False,
                "complete_case": True,
            },
            "components": {
                component: {
                    "raw": row[component],
                    "rank_z": row[f"{component}_rank_z"],
                }
                for component in COMPONENT_COLUMNS
            },
            "upstream": {
                "factor_id": PIOTROSKI_FACTOR_ID,
                "factor_value_id": row["upstream_piotroski_factor_value_id"],
                "source": PIOTROSKI_SOURCE_NAME,
                "available_at": row["available_at"],
                "book_to_market_percentile": row["book_to_market_percentile"],
                "is_high_book_to_market": row["is_high_book_to_market"],
            },
        }
    )


def compute_continuous_financial_strength_rows(
    inputs: pd.DataFrame,
    options: ContinuousFinancialStrengthOptions | None = None,
) -> pd.DataFrame:
    """Build standalone and high-book-to-market continuous strength rows."""

    options = options or ContinuousFinancialStrengthOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    rows = _calculate_continuous_components(inputs)
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    parts: list[pd.DataFrame] = []
    for factor_id in FACTOR_IDS:
        part = rows.copy()
        minimum_names = options.minimum_names_per_date
        if factor_id == HIGH_BOOK_TO_MARKET_FACTOR_ID:
            part = part[part["is_high_book_to_market"]].copy()
            minimum_names = options.minimum_high_book_to_market_names_per_date
        counts = part.groupby("as_of_date")["security_id"].transform("nunique")
        part = part[counts >= minimum_names].copy()
        if part.empty:
            continue
        metadata = FACTOR_METADATA[factor_id]
        part["factor_id"] = factor_id
        part["factor_name"] = metadata["factor_name"]
        part["family"] = metadata["family"]
        part["raw_value"] = part["continuous_rank_score"].astype(float)
        part = zscore(
            part,
            value_column="raw_value",
            output_column="value",
            partition_columns=("factor_id", "as_of_date"),
        )
        input_ids = [f"factor:{PIOTROSKI_FACTOR_ID}"]
        if factor_id == HIGH_BOOK_TO_MARKET_FACTOR_ID:
            input_ids.append(f"factor:{BOOK_TO_MARKET_FACTOR_ID}")
        part["input_ids_json"] = json_dumps(input_ids)
        part["input_lineage_json"] = part.apply(
            lambda row, selected_factor_id=factor_id: _lineage(
                row, selected_factor_id
            ),
            axis=1,
        )
        part["is_latest_revision"] = True
        part["run_id"] = options.run_id
        part["source"] = options.source
        part["factor_value_id"] = [
            _factor_value_id(
                options.source,
                factor_id,
                security_id,
                as_of_date,
            )
            for security_id, as_of_date in zip(
                part["security_id"], part["as_of_date"], strict=True
            )
        ]
        parts.append(part[_OUTPUT_COLUMNS])
    if not parts:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    return (
        pd.concat(parts, ignore_index=True)
        .dropna(subset=["value"])
        .sort_values(["factor_id", "as_of_date", "security_id"], kind="stable")
        .reset_index(drop=True)
    )


def refresh_continuous_financial_strength_values(
    store: DuckDBStore,
    options: ContinuousFinancialStrengthOptions | None = None,
) -> int:
    """Materialize both continuous financial-strength features."""

    options = options or ContinuousFinancialStrengthOptions()
    store.initialize()
    inputs = load_continuous_financial_strength_inputs(store, options)
    rows = compute_continuous_financial_strength_rows(inputs, options)
    predicates = ["source = ?", "factor_id IN (?, ?)"]
    params: list[object] = [options.source, *FACTOR_IDS]
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
                "continuous_financial_strength_insert",
            )
    return len(rows)
