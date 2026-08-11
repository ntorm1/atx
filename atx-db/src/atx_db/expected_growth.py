"""Point-in-time q5 expected investment-growth proxy."""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any

import numpy as np
import pandas as pd

from .asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from .asset_growth import SOURCE_NAME as ASSET_GROWTH_SOURCE_NAME
from .cash_profitability import SOURCE_NAME as CASH_PROFITABILITY_SOURCE_NAME
from .connection import DuckDBStore
from .delta_roe import FACTOR_ID as DELTA_ROE_FACTOR_ID
from .delta_roe import SOURCE_NAME as DELTA_ROE_SOURCE_NAME
from .factors.cross_section import winsorize, zscore
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT q5 published-slope expected growth proxy v1"
FACTOR_ID = "investment_q5_expected_growth_proxy"
FACTOR_NAME = "PIT q5 expected investment-growth proxy"
FACTOR_FAMILY = "fundamental_expected_growth"
CASH_PROFITABILITY_FACTOR_ID = "profitability_cash_operating_profitability"
PUBLISHED_SLOPES = {
    "log_tobins_q": -0.029,
    "cash_operating_profitability": 0.516,
    "delta_roe": 0.771,
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
class ExpectedGrowthOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_annual_age_days: int = 120
    maximum_annual_age_days: int = 550
    minimum_names_per_date: int = 20
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    run_id: str | None = None


def _factor_value_id(source: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, FACTOR_ID, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def load_expected_growth_inputs(
    store: DuckDBStore,
    options: ExpectedGrowthOptions | None = None,
) -> pd.DataFrame:
    """Join governed annual predictors, SEC debt facts, and latest delta ROE."""

    options = options or ExpectedGrowthOptions()
    predicates: list[str] = []
    date_params: list[object] = []
    if options.start_date is not None:
        predicates.append("a.as_of_date >= ?")
        date_params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("a.as_of_date <= ?")
        date_params.append(options.end_date)
    date_sql = " AND " + " AND ".join(predicates) if predicates else ""

    sql = f"""
        WITH asset_growth AS (
            SELECT
                factor_value_id AS asset_growth_factor_value_id,
                security_id,
                symbol,
                as_of_date,
                available_at AS asset_growth_available_at,
                json_extract_string(
                    input_lineage_json, '$.assets.current.accession_number'
                ) AS accession_number,
                CAST(json_extract_string(
                    input_lineage_json, '$.assets.current.period_end'
                ) AS DATE) AS annual_period_end,
                CAST(json_extract_string(
                    input_lineage_json, '$.assets.current.value'
                ) AS DOUBLE) AS current_assets,
                json_extract_string(
                    input_lineage_json, '$.assets.current.statement_point_id'
                ) AS current_asset_id
            FROM fundamental_factor_values
            WHERE factor_id = ? AND source = ? AND is_latest_revision
        ),
        cash_profitability AS (
            SELECT
                factor_value_id AS cash_profitability_factor_value_id,
                security_id,
                as_of_date,
                raw_value AS cash_profitability_prior_assets,
                available_at AS cash_profitability_available_at,
                json_extract_string(
                    input_lineage_json, '$.current_statement.accession_number'
                ) AS accession_number,
                CAST(json_extract_string(
                    input_lineage_json, '$.current_statement.period_end'
                ) AS DATE) AS annual_period_end,
                CAST(json_extract_string(
                    input_lineage_json, '$.prior_statement.total_assets'
                ) AS DOUBLE) AS prior_assets,
                json_extract_string(
                    input_lineage_json, '$.prior_statement.total_assets_id'
                ) AS prior_asset_id,
                CAST(json_extract_string(
                    input_lineage_json, '$.decision.market_cap_usd'
                ) AS DOUBLE) AS market_equity
            FROM fundamental_factor_values
            WHERE factor_id = ? AND source = ? AND is_latest_revision
        ),
        delta_roe AS (
            SELECT
                factor_value_id AS delta_roe_factor_value_id,
                security_id,
                as_of_date,
                raw_value AS delta_roe,
                available_at AS delta_roe_available_at
            FROM fundamental_factor_values
            WHERE factor_id = ? AND source = ? AND is_latest_revision
        ),
        debt AS (
            SELECT
                security_id,
                accession_number,
                period_end AS annual_period_end,
                arg_max(value, (available_at, source_loaded_at)) FILTER (
                    WHERE concept = 'LongTermDebtNoncurrent'
                ) AS long_term_debt,
                arg_max(value, (available_at, source_loaded_at)) FILTER (
                    WHERE concept = 'LongTermDebtCurrent'
                ) AS current_long_term_debt,
                arg_max(value, (available_at, source_loaded_at)) FILTER (
                    WHERE concept = 'DebtCurrent'
                ) AS current_debt_fallback,
                max(available_at) AS debt_available_at
            FROM sec_company_facts
            WHERE taxonomy = 'us-gaap'
              AND unit = 'USD'
              AND concept IN (
                  'LongTermDebtNoncurrent', 'LongTermDebtCurrent', 'DebtCurrent'
              )
              AND form IN (
                  '10-K', '10-K/A', '10-KT',
                  '20-F', '20-F/A', '40-F', '40-F/A'
              )
              AND value >= 0
              AND isfinite(value)
              AND accession_number IS NOT NULL
              AND period_end IS NOT NULL
            GROUP BY security_id, accession_number, period_end
        ),
        aligned AS (
            SELECT
                a.*,
                c.cash_profitability_factor_value_id,
                c.cash_profitability_prior_assets,
                c.cash_profitability_available_at,
                c.prior_assets,
                c.prior_asset_id,
                c.market_equity,
                r.delta_roe_factor_value_id,
                coalesce(r.delta_roe, 0.0) AS delta_roe,
                r.delta_roe IS NULL AS delta_roe_imputed_zero,
                r.delta_roe_available_at,
                d.long_term_debt,
                coalesce(d.current_long_term_debt, d.current_debt_fallback)
                    AS short_term_debt,
                d.debt_available_at,
                greatest(
                    a.asset_growth_available_at,
                    c.cash_profitability_available_at,
                    coalesce(r.delta_roe_available_at, a.asset_growth_available_at),
                    coalesce(d.debt_available_at, a.asset_growth_available_at)
                ) AS decision_available_at
            FROM asset_growth a
            JOIN cash_profitability c
              ON c.security_id = a.security_id
             AND c.as_of_date = a.as_of_date
             AND c.accession_number = a.accession_number
             AND c.annual_period_end = a.annual_period_end
            LEFT JOIN delta_roe r
              ON r.security_id = a.security_id
             AND r.as_of_date = a.as_of_date
            LEFT JOIN debt d
              ON d.security_id = a.security_id
             AND d.accession_number = a.accession_number
             AND d.annual_period_end = a.annual_period_end
            WHERE a.as_of_date - a.annual_period_end BETWEEN ? AND ?
              {date_sql}
        )
        SELECT
            *,
            coalesce(long_term_debt, 0.0) + coalesce(short_term_debt, 0.0)
                AS total_debt,
            (market_equity + coalesce(long_term_debt, 0.0)
                + coalesce(short_term_debt, 0.0)) / current_assets AS tobins_q,
            cash_profitability_prior_assets * prior_assets / current_assets
                AS cash_operating_profitability
        FROM aligned
        WHERE current_assets > 0
          AND prior_assets > 0
          AND market_equity > 0
          AND (debt_available_at IS NULL OR debt_available_at <= decision_available_at)
          AND (
              delta_roe_available_at IS NULL
              OR delta_roe_available_at <= decision_available_at
          )
        ORDER BY as_of_date, security_id
    """
    return store.con.execute(
        sql,
        [
            ASSET_GROWTH_FACTOR_ID,
            ASSET_GROWTH_SOURCE_NAME,
            CASH_PROFITABILITY_FACTOR_ID,
            CASH_PROFITABILITY_SOURCE_NAME,
            DELTA_ROE_FACTOR_ID,
            DELTA_ROE_SOURCE_NAME,
            options.minimum_annual_age_days,
            options.maximum_annual_age_days,
            *date_params,
        ],
    ).df()


def _lineage(row: pd.Series, options: ExpectedGrowthOptions) -> str:
    return json_dumps(
        {
            "method": "hou_mo_xue_zhang_q5_published_full_sample_slopes_proxy",
            "formula": "-0.029*ln(q)+0.516*Cop+0.771*dROE",
            "orientation": "higher_is_higher_expected_investment_growth",
            "research_contract": {
                "published_slopes": PUBLISHED_SLOPES,
                "annual_age_days": [
                    options.minimum_annual_age_days,
                    options.maximum_annual_age_days,
                ],
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "return_fitted_parameters": False,
                "intercept_omitted_as_cross_sectionally_rank_invariant": True,
            },
            "published_method_adaptations": {
                "slopes": "Published 1963-2018 average slopes replace local rolling WLS.",
                "market_equity": "PIT close times latest visible reported shares.",
                "short_term_debt": (
                    "LongTermDebtCurrent with DebtCurrent fallback; zero when both are absent."
                ),
                "long_term_debt": "LongTermDebtNoncurrent; zero when absent.",
                "cash_profitability": (
                    "Governed cash-profitability numerator rescaled from prior to current assets."
                ),
            },
            "annual_statement": {
                "accession_number": row["accession_number"],
                "period_end": row["annual_period_end"],
                "current_assets": row["current_assets"],
                "current_asset_id": row["current_asset_id"],
                "prior_assets": row["prior_assets"],
                "prior_asset_id": row["prior_asset_id"],
                "asset_growth_factor_value_id": row["asset_growth_factor_value_id"],
                "cash_profitability_factor_value_id": row[
                    "cash_profitability_factor_value_id"
                ],
            },
            "predictors": {
                "market_equity": row["market_equity"],
                "long_term_debt": row["long_term_debt"],
                "short_term_debt": row["short_term_debt"],
                "missing_debt_components_replaced_with_zero": bool(
                    row["long_term_debt_imputed_zero"]
                    or row["short_term_debt_imputed_zero"]
                ),
                "tobins_q": row["tobins_q"],
                "log_tobins_q": row["log_tobins_q"],
                "cash_operating_profitability": row[
                    "cash_operating_profitability"
                ],
                "delta_roe": row["delta_roe"],
                "delta_roe_imputed_zero": bool(row["delta_roe_imputed_zero"]),
                "delta_roe_factor_value_id": row["delta_roe_factor_value_id"],
            },
            "expected_growth_proxy": row["raw_value"],
        }
    )


def compute_expected_growth_rows(
    inputs: pd.DataFrame,
    options: ExpectedGrowthOptions | None = None,
) -> pd.DataFrame:
    """Apply published q5 slopes to winsorized PIT predictors and standardize."""

    options = options or ExpectedGrowthOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    required = {
        "asset_growth_factor_value_id",
        "cash_profitability_factor_value_id",
        "security_id",
        "symbol",
        "as_of_date",
        "decision_available_at",
        "accession_number",
        "annual_period_end",
        "current_assets",
        "prior_assets",
        "current_asset_id",
        "prior_asset_id",
        "market_equity",
        "long_term_debt",
        "short_term_debt",
        "tobins_q",
        "cash_operating_profitability",
        "delta_roe",
        "delta_roe_imputed_zero",
        "delta_roe_factor_value_id",
    }
    missing = sorted(required.difference(inputs.columns))
    if missing:
        raise ValueError(f"Expected-growth inputs missing columns: {missing}")

    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(
        rows["decision_available_at"], errors="coerce"
    )
    numeric = [
        "current_assets",
        "prior_assets",
        "market_equity",
        "long_term_debt",
        "short_term_debt",
        "tobins_q",
        "cash_operating_profitability",
        "delta_roe",
    ]
    for column in numeric:
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    rows["long_term_debt_imputed_zero"] = rows["long_term_debt"].isna()
    rows["short_term_debt_imputed_zero"] = rows["short_term_debt"].isna()
    rows["long_term_debt"] = rows["long_term_debt"].fillna(0.0)
    rows["short_term_debt"] = rows["short_term_debt"].fillna(0.0)
    rows["delta_roe"] = rows["delta_roe"].fillna(0.0)
    rows = rows.dropna(
        subset=[
            "security_id",
            "as_of_date",
            "available_at",
            "current_assets",
            "prior_assets",
            "market_equity",
            "tobins_q",
            "cash_operating_profitability",
            "delta_roe",
        ]
    )
    rows = rows[
        (rows["current_assets"] > 0)
        & (rows["prior_assets"] > 0)
        & (rows["market_equity"] > 0)
        & (rows["tobins_q"] > 0)
    ].copy()
    finite = rows[
        ["tobins_q", "cash_operating_profitability", "delta_roe"]
    ].apply(lambda column: column.map(math.isfinite)).all(axis=1)
    rows = rows[finite].copy()
    rows["log_tobins_q"] = np.log(rows["tobins_q"])
    rows = rows[rows["log_tobins_q"].map(math.isfinite)].copy()
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= options.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)

    for predictor in (
        "log_tobins_q",
        "cash_operating_profitability",
        "delta_roe",
    ):
        rows = winsorize(
            rows,
            value_column=predictor,
            output_column=f"winsorized_{predictor}",
            partition_columns=("as_of_date",),
            limits=options.winsor_limit,
        )
    rows["raw_value"] = sum(
        slope * rows[f"winsorized_{predictor}"]
        for predictor, slope in PUBLISHED_SLOPES.items()
    )
    rows["factor_id"] = FACTOR_ID
    rows["factor_name"] = FACTOR_NAME
    rows["family"] = FACTOR_FAMILY
    rows = zscore(
        rows,
        value_column="raw_value",
        output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    rows["input_ids_json"] = json_dumps(
        [
            f"factor:{ASSET_GROWTH_FACTOR_ID}",
            f"factor:{CASH_PROFITABILITY_FACTOR_ID}",
            f"factor:{DELTA_ROE_FACTOR_ID}",
            "source:sec_company_facts",
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


def refresh_expected_growth_values(
    store: DuckDBStore,
    options: ExpectedGrowthOptions | None = None,
) -> int:
    """Materialize the point-in-time q5 published-slope expected-growth proxy."""

    options = options or ExpectedGrowthOptions()
    store.initialize()
    rows = compute_expected_growth_rows(load_expected_growth_inputs(store, options), options)
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
                store, rows, "fundamental_factor_values", "expected_growth_insert"
            )
    return len(rows)
