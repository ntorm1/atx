"""Point-in-time quarterly inventory-change and inventory-growth factors."""

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
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT quarterly inventory investment v1"
INVENTORY_CHANGE_FACTOR_ID = "investment_low_quarterly_inventory_change"
INVENTORY_GROWTH_FACTOR_ID = "investment_low_quarterly_inventory_growth"
FACTOR_IDS = (INVENTORY_CHANGE_FACTOR_ID, INVENTORY_GROWTH_FACTOR_ID)

_FACTOR_NAMES = {
    INVENTORY_CHANGE_FACTOR_ID: "PIT low quarterly inventory change",
    INVENTORY_GROWTH_FACTOR_ID: "PIT low quarterly inventory growth",
}
_FORMULAS = {
    INVENTORY_CHANGE_FACTOR_ID: (
        "(current_inventory-prior_inventory)"
        "/average(current_total_assets,prior_total_assets)"
    ),
    INVENTORY_GROWTH_FACTOR_ID: "current_inventory/prior_inventory-1",
}
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
class QuarterlyInventoryInvestmentOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    maximum_absolute_inventory_change: float = 5.0
    maximum_absolute_inventory_growth: float = 10.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(
    source: str,
    factor_id: str,
    security_id: str,
    as_of_date: Any,
) -> str:
    payload = "|".join(
        str(part) for part in (source, factor_id, security_id, as_of_date)
    )
    return hashlib.sha256(payload.encode()).hexdigest()


def load_quarterly_inventory_investment_inputs(
    store: DuckDBStore,
    options: QuarterlyInventoryInvestmentOptions | None = None,
) -> pd.DataFrame:
    """Attach the current total-assets fact to the governed Claq inventory pair."""

    options = options or QuarterlyInventoryInvestmentOptions()
    predicates = ["factor_id = ?", "source = ?", "is_latest_revision"]
    params: list[object] = [CASH_PROFITABILITY_FACTOR_ID, CASH_SOURCE_NAME]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    return store.con.execute(
        f"""
        WITH cash_base AS (
            SELECT
                factor_value_id AS cash_factor_value_id,
                security_id,
                symbol,
                as_of_date,
                available_at AS decision_available_at,
                CAST(json_extract_string(
                    input_lineage_json,'$.statements.current_period_end'
                ) AS DATE) AS current_period_end,
                CAST(json_extract_string(
                    input_lineage_json,'$.statements.prior_period_end'
                ) AS DATE) AS prior_period_end,
                json_extract_string(
                    input_lineage_json,'$.statements.current_accession_number'
                ) AS current_accession_number,
                CAST(json_extract_string(
                    input_lineage_json,'$.statements.lagged_total_assets'
                ) AS DOUBLE) AS prior_total_assets,
                CAST(json_extract_string(
                    input_lineage_json,'$.working_capital.inventory.current_value'
                ) AS DOUBLE) AS current_inventory,
                json_extract_string(
                    input_lineage_json,'$.working_capital.inventory.current_id'
                ) AS current_inventory_id,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.working_capital.inventory.current_available_at'
                ) AS TIMESTAMP) AS current_inventory_available_at,
                CAST(json_extract_string(
                    input_lineage_json,'$.working_capital.inventory.prior_value'
                ) AS DOUBLE) AS prior_inventory,
                json_extract_string(
                    input_lineage_json,'$.working_capital.inventory.prior_id'
                ) AS prior_inventory_id,
                TRY_CAST(json_extract_string(
                    input_lineage_json,'$.working_capital.inventory.prior_available_at'
                ) AS TIMESTAMP) AS prior_inventory_available_at
            FROM fundamental_factor_values
            WHERE {' AND '.join(predicates)}
        ),
        candidates AS (
            SELECT
                b.*,
                a.value AS current_total_assets,
                a.statement_point_id AS current_total_assets_id,
                a.available_at AS current_total_assets_available_at,
                row_number() OVER (
                    PARTITION BY b.cash_factor_value_id
                    ORDER BY a.available_at DESC NULLS LAST,
                             a.revision_sequence DESC NULLS LAST,
                             a.statement_point_id DESC NULLS LAST
                ) AS asset_rank
            FROM cash_base b
            LEFT JOIN fundamental_statement_points a
              ON a.security_id = b.security_id
             AND a.accession_number = b.current_accession_number
             AND a.period_end = b.current_period_end
             AND a.canonical_metric = 'total_assets'
             AND a.unit = 'USD'
             AND a.period_type = 'instant'
             AND a.value IS NOT NULL
             AND isfinite(a.value)
             AND a.available_at IS NOT NULL
             AND a.available_at <= b.decision_available_at
        )
        SELECT * EXCLUDE (asset_rank)
        FROM candidates
        WHERE asset_rank = 1
        ORDER BY as_of_date,security_id
        """,
        params,
    ).df()


def _lineage(
    row: pd.Series,
    options: QuarterlyInventoryInvestmentOptions,
) -> str:
    factor_id = str(row["factor_id"])
    return json_dumps(
        {
            "method": "global_q_inventory_measure_quarterly_pit_adaptation",
            "formula": _FORMULAS[factor_id],
            "orientation": "lower_inventory_investment_is_preferred",
            "research_contract": {
                "annual_to_quarterly_adaptation": True,
                "exclude_no_inventory_in_either_period": True,
                "maximum_absolute_inventory_change": (
                    options.maximum_absolute_inventory_change
                ),
                "maximum_absolute_inventory_growth": (
                    options.maximum_absolute_inventory_growth
                ),
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "return_fitted_parameters": False,
            },
            "cash_profitability_input": {
                "factor_id": CASH_PROFITABILITY_FACTOR_ID,
                "factor_value_id": row["cash_factor_value_id"],
                "source": CASH_SOURCE_NAME,
                "available_at": row["decision_available_at"],
            },
            "periods": {
                "current": row.get("current_period_end"),
                "prior": row.get("prior_period_end"),
            },
            "inventory": {
                "current_value": row["current_inventory"],
                "current_id": row.get("current_inventory_id"),
                "current_available_at": row.get("current_inventory_available_at"),
                "prior_value": row["prior_inventory"],
                "prior_id": row.get("prior_inventory_id"),
                "prior_available_at": row.get("prior_inventory_available_at"),
                "change": row["inventory_change"],
                "growth": row["inventory_growth"],
            },
            "assets": {
                "current_value": row["current_total_assets"],
                "current_id": row.get("current_total_assets_id"),
                "current_available_at": row[
                    "current_total_assets_available_at"
                ],
                "prior_value": row["prior_total_assets"],
                "average_value": row["average_total_assets"],
            },
            "raw_value": row["raw_value"],
        }
    )


def compute_quarterly_inventory_investment_rows(
    inputs: pd.DataFrame,
    options: QuarterlyInventoryInvestmentOptions | None = None,
) -> pd.DataFrame:
    """Compute literature-defined low inventory-change and inventory-growth scores."""

    options = options or QuarterlyInventoryInvestmentOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "cash_factor_value_id",
        "security_id",
        "symbol",
        "as_of_date",
        "decision_available_at",
        "current_total_assets_available_at",
        "current_inventory",
        "prior_inventory",
        "current_total_assets",
        "prior_total_assets",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"quarterly inventory inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(
        rows["as_of_date"], errors="coerce"
    ).dt.date
    for column in (
        "decision_available_at",
        "current_total_assets_available_at",
    ):
        rows[column] = pd.to_datetime(rows[column], errors="coerce")
    for column in (
        "current_inventory",
        "prior_inventory",
        "current_total_assets",
        "prior_total_assets",
    ):
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows = rows.dropna(
        subset=[
            "security_id",
            "as_of_date",
            "decision_available_at",
            "current_inventory",
            "prior_inventory",
            "current_total_assets",
            "prior_total_assets",
        ]
    )
    valid = pd.Series(True, index=rows.index)
    for column in (
        "current_inventory",
        "prior_inventory",
        "current_total_assets",
        "prior_total_assets",
    ):
        valid &= rows[column].map(math.isfinite)
    rows = rows[
        valid
        & (rows["current_inventory"] > 0)
        & (rows["prior_inventory"] > 0)
        & (rows["current_total_assets"] > 0)
        & (rows["prior_total_assets"] > 0)
    ].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["inventory_change"] = rows["current_inventory"] - rows["prior_inventory"]
    rows["average_total_assets"] = (
        rows["current_total_assets"] + rows["prior_total_assets"]
    ) / 2.0
    rows["inventory_change_ratio"] = (
        rows["inventory_change"] / rows["average_total_assets"]
    )
    rows["inventory_growth"] = (
        rows["current_inventory"] / rows["prior_inventory"] - 1.0
    )

    parts: list[pd.DataFrame] = []
    for factor_id, raw_column, maximum in (
        (
            INVENTORY_CHANGE_FACTOR_ID,
            "inventory_change_ratio",
            options.maximum_absolute_inventory_change,
        ),
        (
            INVENTORY_GROWTH_FACTOR_ID,
            "inventory_growth",
            options.maximum_absolute_inventory_growth,
        ),
    ):
        part = rows[rows[raw_column].map(math.isfinite)].copy()
        part["raw_value"] = part[raw_column]
        part = part[part["raw_value"].abs() <= maximum].copy()
        part["factor_id"] = factor_id
        parts.append(part)
    result = pd.concat(parts, ignore_index=True) if parts else pd.DataFrame()
    if result.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    counts = result.groupby(["factor_id", "as_of_date"])["security_id"].transform(
        "nunique"
    )
    result = result[counts >= options.minimum_names_per_date].copy()
    result["factor_name"] = result["factor_id"].map(_FACTOR_NAMES)
    result["family"] = "fundamental_investment"
    result["oriented_value"] = -result["raw_value"]
    result = winsorize(
        result,
        value_column="oriented_value",
        output_column="winsorized_value",
        partition_columns=("factor_id", "as_of_date"),
        limits=options.winsor_limit,
    )
    result = zscore(
        result,
        value_column="winsorized_value",
        output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    result["available_at"] = result[
        ["decision_available_at", "current_total_assets_available_at"]
    ].max(axis=1)
    result["input_ids_json"] = json_dumps(
        [f"factor:{CASH_PROFITABILITY_FACTOR_ID}", "metric:total_assets"]
    )
    result["input_lineage_json"] = result.apply(
        lambda row: _lineage(row, options), axis=1
    )
    result["is_latest_revision"] = True
    result["run_id"] = options.run_id
    result["source"] = options.source
    result["factor_value_id"] = [
        _factor_value_id(options.source, factor_id, security_id, as_of_date)
        for factor_id, security_id, as_of_date in zip(
            result["factor_id"],
            result["security_id"],
            result["as_of_date"],
            strict=True,
        )
    ]
    return (
        result[_OUTPUT_COLUMNS]
        .dropna(subset=["value"])
        .sort_values(["factor_id", "as_of_date", "security_id"], kind="stable")
        .reset_index(drop=True)
    )


def refresh_quarterly_inventory_investment_values(
    store: DuckDBStore,
    options: QuarterlyInventoryInvestmentOptions | None = None,
) -> int:
    """Materialize both point-in-time quarterly inventory investment factors."""

    options = options or QuarterlyInventoryInvestmentOptions()
    store.initialize()
    rows = compute_quarterly_inventory_investment_rows(
        load_quarterly_inventory_investment_inputs(store, options), options
    )
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
                "quarterly_inventory_investment_insert",
            )
    return len(rows)
