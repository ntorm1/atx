"""Point-in-time low quarterly operating working-capital accruals factor."""

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

SOURCE_NAME = "atx-db PIT low quarterly working-capital accruals v1"
FACTOR_ID = "quality_low_quarterly_operating_working_capital_accruals"
FACTOR_NAME = "PIT low quarterly operating working-capital accruals"
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
class QuarterlyWorkingCapitalAccrualsOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    maximum_absolute_raw_value: float = 5.0
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_quarterly_working_capital_accruals_inputs(
    store: DuckDBStore,
    options: QuarterlyWorkingCapitalAccrualsOptions | None = None,
) -> pd.DataFrame:
    """Read the exact working-capital decomposition persisted in governed Claq lineage."""

    options = options or QuarterlyWorkingCapitalAccrualsOptions()
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
        SELECT
            factor_value_id AS cash_factor_value_id,
            security_id,
            symbol,
            as_of_date,
            available_at AS decision_available_at,
            CAST(json_extract_string(
                input_lineage_json,
                '$.net_operating_working_capital_change'
            ) AS DOUBLE) AS net_operating_working_capital_change,
            CAST(json_extract_string(
                input_lineage_json,
                '$.statements.lagged_total_assets'
            ) AS DOUBLE) AS lagged_total_assets,
            CAST(json_extract_string(
                input_lineage_json,
                '$.working_capital.ar.change'
            ) AS DOUBLE) AS delta_ar,
            CAST(json_extract_string(
                input_lineage_json,
                '$.working_capital.inventory.change'
            ) AS DOUBLE) AS delta_inventory,
            CAST(json_extract_string(
                input_lineage_json,
                '$.working_capital.deferred_revenue.change'
            ) AS DOUBLE) AS delta_deferred_revenue,
            CAST(json_extract_string(
                input_lineage_json,
                '$.working_capital.ap.change'
            ) AS DOUBLE) AS delta_ap,
            CAST(json_extract(
                input_lineage_json,
                '$.working_capital.ar.missing_change_replaced_with_zero'
            ) AS BOOLEAN) AS ar_missing,
            CAST(json_extract(
                input_lineage_json,
                '$.working_capital.inventory.missing_change_replaced_with_zero'
            ) AS BOOLEAN) AS inventory_missing,
            CAST(json_extract(
                input_lineage_json,
                '$.working_capital.deferred_revenue.missing_change_replaced_with_zero'
            ) AS BOOLEAN) AS deferred_revenue_missing,
            CAST(json_extract(
                input_lineage_json,
                '$.working_capital.ap.missing_change_replaced_with_zero'
            ) AS BOOLEAN) AS ap_missing,
            CAST(json_extract_string(
                input_lineage_json,
                '$.statements.current_period_end'
            ) AS DATE) AS current_period_end,
            CAST(json_extract_string(
                input_lineage_json,
                '$.statements.prior_period_end'
            ) AS DATE) AS prior_period_end
        FROM fundamental_factor_values
        WHERE {' AND '.join(predicates)}
        ORDER BY as_of_date, security_id
        """,
        params,
    ).df()


def _lineage(
    row: pd.Series,
    options: QuarterlyWorkingCapitalAccrualsOptions,
) -> str:
    return json_dumps(
        {
            "method": "quarterly_operating_working_capital_accruals_from_claq",
            "formula": "(dAR+dInventory-dDeferredRevenue-dAP)/lagged_total_assets",
            "orientation": "lower_accruals_are_higher_quality",
            "research_contract": {
                "maximum_absolute_raw_value": options.maximum_absolute_raw_value,
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "return_fitted_parameters": False,
            },
            "cash_profitability_input": {
                "factor_id": CASH_PROFITABILITY_FACTOR_ID,
                "factor_value_id": row["cash_factor_value_id"],
                "source": CASH_SOURCE_NAME,
                "as_of_date": row["as_of_date"],
                "available_at": row["decision_available_at"],
            },
            "periods": {
                "current": row["current_period_end"],
                "prior": row["prior_period_end"],
            },
            "changes": {
                "ar": {"value": row["delta_ar"], "missing_pair": row["ar_missing"]},
                "inventory": {
                    "value": row["delta_inventory"],
                    "missing_pair": row["inventory_missing"],
                },
                "deferred_revenue": {
                    "value": row["delta_deferred_revenue"],
                    "missing_pair": row["deferred_revenue_missing"],
                },
                "ap": {"value": row["delta_ap"], "missing_pair": row["ap_missing"]},
            },
            "net_operating_working_capital_change": row[
                "net_operating_working_capital_change"
            ],
            "lagged_total_assets": row["lagged_total_assets"],
            "working_capital_accrual_ratio": row["raw_value"],
        }
    )


def compute_quarterly_working_capital_accruals_rows(
    inputs: pd.DataFrame,
    options: QuarterlyWorkingCapitalAccrualsOptions | None = None,
) -> pd.DataFrame:
    """Compute low-accrual quality scores from the Claq working-capital wedge."""

    options = options or QuarterlyWorkingCapitalAccrualsOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "cash_factor_value_id",
        "security_id",
        "symbol",
        "as_of_date",
        "decision_available_at",
        "net_operating_working_capital_change",
        "lagged_total_assets",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"quarterly accrual inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(
        rows["decision_available_at"], errors="coerce"
    )
    rows["net_operating_working_capital_change"] = pd.to_numeric(
        rows["net_operating_working_capital_change"], errors="coerce"
    )
    rows["lagged_total_assets"] = pd.to_numeric(
        rows["lagged_total_assets"], errors="coerce"
    )
    rows = rows.dropna(
        subset=[
            "security_id",
            "as_of_date",
            "available_at",
            "net_operating_working_capital_change",
            "lagged_total_assets",
        ]
    )
    rows = rows[
        (rows["lagged_total_assets"] > 0)
        & rows["net_operating_working_capital_change"].map(math.isfinite)
        & rows["lagged_total_assets"].map(math.isfinite)
    ].copy()
    rows["working_capital_accrual_ratio"] = (
        rows["net_operating_working_capital_change"] / rows["lagged_total_assets"]
    )
    rows = rows[rows["working_capital_accrual_ratio"].map(math.isfinite)].copy()
    rows = rows[
        rows["working_capital_accrual_ratio"].abs()
        <= options.maximum_absolute_raw_value
    ].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["working_capital_accrual_ratio"]
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
    rows["input_ids_json"] = json_dumps(
        [f"factor:{CASH_PROFITABILITY_FACTOR_ID}"]
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


def refresh_quarterly_working_capital_accruals_values(
    store: DuckDBStore,
    options: QuarterlyWorkingCapitalAccrualsOptions | None = None,
) -> int:
    """Materialize point-in-time low quarterly working-capital accruals."""

    options = options or QuarterlyWorkingCapitalAccrualsOptions()
    store.initialize()
    rows = compute_quarterly_working_capital_accruals_rows(
        load_quarterly_working_capital_accruals_inputs(store, options), options
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
                "quarterly_working_capital_accruals_insert",
            )
    return len(rows)
