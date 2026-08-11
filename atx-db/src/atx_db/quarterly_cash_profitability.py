"""Point-in-time quarterly cash operating profits-to-lagged-assets factor."""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .factors.cross_section import winsorize, zscore
from .quarterly_operating_profitability import (
    FACTOR_ID as OPERATING_PROFITABILITY_FACTOR_ID,
)
from .quarterly_operating_profitability import SOURCE_NAME as OPERATING_SOURCE_NAME
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT quarterly cash profitability v1"
FACTOR_ID = "profitability_quarterly_cash_operating_profitability_lagged_assets"
FACTOR_NAME = "PIT quarterly cash operating profits-to-lagged assets"
FACTOR_FAMILY = "fundamental_profitability"
_BALANCE_METRICS = ("ar", "inventory", "deferred_revenue", "ap")
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
class QuarterlyCashProfitabilityOptions:
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


def _date_filter(
    options: QuarterlyCashProfitabilityOptions,
) -> tuple[str, list[object]]:
    predicates: list[str] = []
    params: list[object] = []
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        params.append(options.end_date)
    return (" AND " + " AND ".join(predicates) if predicates else "", params)


def load_quarterly_cash_profitability_inputs(
    store: DuckDBStore,
    options: QuarterlyCashProfitabilityOptions | None = None,
) -> pd.DataFrame:
    """Attach latest visible current/prior working-capital facts to governed Olaq."""

    options = options or QuarterlyCashProfitabilityOptions()
    date_sql, date_params = _date_filter(options)
    sql = f"""
        WITH qop_base AS (
            SELECT
                factor_value_id AS qop_factor_value_id,
                security_id,
                symbol,
                as_of_date,
                raw_value AS quarterly_operating_profitability,
                value AS qop_standardized_value,
                available_at AS decision_available_at,
                source AS qop_source,
                CAST(json_extract_string(
                    input_lineage_json,
                    '$.quarterly_statement.operating_profit'
                ) AS DOUBLE) AS quarterly_operating_profit,
                CAST(json_extract_string(
                    input_lineage_json,
                    '$.quarterly_statement.period_end'
                ) AS DATE) AS current_period_end,
                CAST(json_extract_string(
                    input_lineage_json,
                    '$.lagged_assets.period_end'
                ) AS DATE) AS prior_period_end,
                CAST(json_extract_string(
                    input_lineage_json,
                    '$.lagged_assets.value'
                ) AS DOUBLE) AS lagged_total_assets,
                json_extract_string(
                    input_lineage_json,
                    '$.quarterly_statement.accession_number'
                ) AS current_accession_number,
                json_extract_string(
                    input_lineage_json,
                    '$.lagged_assets.accession_number'
                ) AS prior_accession_number
            FROM fundamental_factor_values
            WHERE factor_id = ?
              AND source = ?
              AND is_latest_revision
              AND raw_value IS NOT NULL
              AND isfinite(raw_value)
              {date_sql}
        ),
        balance_statements AS (
            SELECT
                security_id,
                accession_number AS balance_accession_number,
                period_end,
                max(available_at) AS balance_available_at,
                arg_max(value, (available_at, revision_sequence, statement_point_id))
                    FILTER (WHERE canonical_metric = 'ar') AS ar,
                arg_max(statement_point_id,
                        (available_at, revision_sequence, statement_point_id))
                    FILTER (WHERE canonical_metric = 'ar') AS ar_id,
                max(available_at) FILTER (WHERE canonical_metric = 'ar')
                    AS ar_available_at,
                arg_max(value, (available_at, revision_sequence, statement_point_id))
                    FILTER (WHERE canonical_metric = 'inventory') AS inventory,
                arg_max(statement_point_id,
                        (available_at, revision_sequence, statement_point_id))
                    FILTER (WHERE canonical_metric = 'inventory') AS inventory_id,
                max(available_at) FILTER (WHERE canonical_metric = 'inventory')
                    AS inventory_available_at,
                arg_max(value, (available_at, revision_sequence, statement_point_id))
                    FILTER (WHERE canonical_metric = 'deferred_revenue')
                    AS deferred_revenue,
                arg_max(statement_point_id,
                        (available_at, revision_sequence, statement_point_id))
                    FILTER (WHERE canonical_metric = 'deferred_revenue')
                    AS deferred_revenue_id,
                max(available_at) FILTER (WHERE canonical_metric = 'deferred_revenue')
                    AS deferred_revenue_available_at,
                arg_max(value, (available_at, revision_sequence, statement_point_id))
                    FILTER (WHERE canonical_metric = 'ap') AS ap,
                arg_max(statement_point_id,
                        (available_at, revision_sequence, statement_point_id))
                    FILTER (WHERE canonical_metric = 'ap') AS ap_id,
                max(available_at) FILTER (WHERE canonical_metric = 'ap')
                    AS ap_available_at
            FROM fundamental_statement_points
            WHERE canonical_metric IN ('ar', 'inventory', 'deferred_revenue', 'ap')
              AND unit = 'USD'
              AND period_type = 'instant'
              AND period_end IS NOT NULL
              AND value IS NOT NULL
              AND isfinite(value)
              AND available_at IS NOT NULL
              AND accession_number IS NOT NULL
            GROUP BY security_id, accession_number, period_end
        ),
        current_candidates AS (
            SELECT
                b.*,
                c.balance_accession_number AS current_balance_accession_number,
                c.balance_available_at AS current_balance_available_at,
                c.ar AS current_ar,
                c.ar_id AS current_ar_id,
                c.ar_available_at AS current_ar_available_at,
                c.inventory AS current_inventory,
                c.inventory_id AS current_inventory_id,
                c.inventory_available_at AS current_inventory_available_at,
                c.deferred_revenue AS current_deferred_revenue,
                c.deferred_revenue_id AS current_deferred_revenue_id,
                c.deferred_revenue_available_at
                    AS current_deferred_revenue_available_at,
                c.ap AS current_ap,
                c.ap_id AS current_ap_id,
                c.ap_available_at AS current_ap_available_at,
                row_number() OVER (
                    PARTITION BY b.qop_factor_value_id
                    ORDER BY c.balance_available_at DESC NULLS LAST,
                             c.balance_accession_number DESC NULLS LAST
                ) AS current_rank
            FROM qop_base b
            LEFT JOIN balance_statements c
              ON c.security_id = b.security_id
             AND c.period_end = b.current_period_end
             AND c.balance_available_at <= b.decision_available_at
        ),
        current_balance AS (
            SELECT * EXCLUDE (current_rank)
            FROM current_candidates
            WHERE current_rank = 1
        ),
        prior_candidates AS (
            SELECT
                b.*,
                p.balance_accession_number AS prior_balance_accession_number,
                p.balance_available_at AS prior_balance_available_at,
                p.ar AS prior_ar,
                p.ar_id AS prior_ar_id,
                p.ar_available_at AS prior_ar_available_at,
                p.inventory AS prior_inventory,
                p.inventory_id AS prior_inventory_id,
                p.inventory_available_at AS prior_inventory_available_at,
                p.deferred_revenue AS prior_deferred_revenue,
                p.deferred_revenue_id AS prior_deferred_revenue_id,
                p.deferred_revenue_available_at
                    AS prior_deferred_revenue_available_at,
                p.ap AS prior_ap,
                p.ap_id AS prior_ap_id,
                p.ap_available_at AS prior_ap_available_at,
                row_number() OVER (
                    PARTITION BY b.qop_factor_value_id
                    ORDER BY p.balance_available_at DESC NULLS LAST,
                             p.balance_accession_number DESC NULLS LAST
                ) AS prior_rank
            FROM current_balance b
            LEFT JOIN balance_statements p
              ON p.security_id = b.security_id
             AND p.period_end = b.prior_period_end
             AND p.balance_available_at <= b.decision_available_at
        )
        SELECT * EXCLUDE (prior_rank)
        FROM prior_candidates
        WHERE prior_rank = 1
        ORDER BY as_of_date, security_id
    """
    return store.con.execute(
        sql,
        [OPERATING_PROFITABILITY_FACTOR_ID, OPERATING_SOURCE_NAME, *date_params],
    ).df()


def _balance_lineage(row: pd.Series, metric: str) -> dict[str, object]:
    current = row.get(f"current_{metric}")
    prior = row.get(f"prior_{metric}")
    complete_pair = not pd.isna(current) and not pd.isna(prior)
    return {
        "current_id": row.get(f"current_{metric}_id"),
        "current_value": current,
        "current_available_at": row.get(f"current_{metric}_available_at"),
        "prior_id": row.get(f"prior_{metric}_id"),
        "prior_value": prior,
        "prior_available_at": row.get(f"prior_{metric}_available_at"),
        "change": row[f"delta_{metric}"],
        "missing_change_replaced_with_zero": not complete_pair,
    }


def _lineage(
    row: pd.Series,
    options: QuarterlyCashProfitabilityOptions,
) -> str:
    return json_dumps(
        {
            "method": "hou_xue_zhang_quarterly_cash_profitability_pit",
            "formula": (
                "(operating_profit-dAR-dInventory+dDeferredRevenue+dAP)"
                "/one_quarter_lagged_total_assets"
            ),
            "orientation": "higher_is_more_profitable",
            "research_contract": {
                "maximum_absolute_raw_value": options.maximum_absolute_raw_value,
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "missing_balance_changes": "zero",
                "return_fitted_parameters": False,
            },
            "operating_profitability_input": {
                "factor_id": OPERATING_PROFITABILITY_FACTOR_ID,
                "factor_value_id": row["qop_factor_value_id"],
                "source": row["qop_source"],
                "as_of_date": row["as_of_date"],
                "available_at": row["decision_available_at"],
                "raw_value": row["quarterly_operating_profitability"],
                "operating_profit": row["quarterly_operating_profit"],
            },
            "statements": {
                "current_period_end": row["current_period_end"],
                "current_accession_number": row["current_accession_number"],
                "prior_period_end": row["prior_period_end"],
                "prior_accession_number": row["prior_accession_number"],
                "lagged_total_assets": row["lagged_total_assets"],
            },
            "working_capital": {
                metric: _balance_lineage(row, metric) for metric in _BALANCE_METRICS
            },
            "net_operating_working_capital_change": row[
                "net_operating_working_capital_change"
            ],
            "quarterly_cash_operating_profit": row["quarterly_cash_operating_profit"],
            "quarterly_cash_profitability": row["raw_value"],
        }
    )


def compute_quarterly_cash_profitability_rows(
    inputs: pd.DataFrame,
    options: QuarterlyCashProfitabilityOptions | None = None,
) -> pd.DataFrame:
    """Compute, guard, winsorize, and standardize quarterly cash profitability."""

    options = options or QuarterlyCashProfitabilityOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "qop_factor_value_id",
        "security_id",
        "symbol",
        "as_of_date",
        "decision_available_at",
        "quarterly_operating_profitability",
        "quarterly_operating_profit",
        "lagged_total_assets",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"quarterly cash profitability inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(
        rows["decision_available_at"], errors="coerce"
    )
    for column in ("quarterly_operating_profit", "lagged_total_assets"):
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    for metric in _BALANCE_METRICS:
        current_column = f"current_{metric}"
        prior_column = f"prior_{metric}"
        rows[current_column] = pd.to_numeric(rows[current_column], errors="coerce")
        rows[prior_column] = pd.to_numeric(rows[prior_column], errors="coerce")
        complete = rows[current_column].notna() & rows[prior_column].notna()
        rows[f"delta_{metric}"] = 0.0
        rows.loc[complete, f"delta_{metric}"] = (
            rows.loc[complete, current_column] - rows.loc[complete, prior_column]
        )
    rows = rows.dropna(
        subset=[
            "security_id",
            "as_of_date",
            "available_at",
            "quarterly_operating_profit",
            "lagged_total_assets",
        ]
    )
    rows = rows[
        (rows["lagged_total_assets"] > 0)
        & rows["quarterly_operating_profit"].map(math.isfinite)
        & rows["lagged_total_assets"].map(math.isfinite)
    ].copy()
    rows["net_operating_working_capital_change"] = (
        rows["delta_ar"]
        + rows["delta_inventory"]
        - rows["delta_deferred_revenue"]
        - rows["delta_ap"]
    )
    rows["quarterly_cash_operating_profit"] = (
        rows["quarterly_operating_profit"]
        - rows["net_operating_working_capital_change"]
    )
    rows["quarterly_cash_profitability"] = (
        rows["quarterly_cash_operating_profit"] / rows["lagged_total_assets"]
    )
    rows = rows[rows["quarterly_cash_profitability"].map(math.isfinite)].copy()
    rows = rows[
        rows["quarterly_cash_profitability"].abs()
        <= options.maximum_absolute_raw_value
    ].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows["raw_value"] = rows["quarterly_cash_profitability"]
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
    rows["input_ids_json"] = json_dumps(
        [
            f"factor:{OPERATING_PROFITABILITY_FACTOR_ID}",
            *[f"metric:{metric}" for metric in _BALANCE_METRICS],
        ]
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


def refresh_quarterly_cash_profitability_values(
    store: DuckDBStore,
    options: QuarterlyCashProfitabilityOptions | None = None,
) -> int:
    """Materialize point-in-time quarterly cash profitability."""

    options = options or QuarterlyCashProfitabilityOptions()
    store.initialize()
    rows = compute_quarterly_cash_profitability_rows(
        load_quarterly_cash_profitability_inputs(store, options), options
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
                "quarterly_cash_profitability_insert",
            )
    return len(rows)
