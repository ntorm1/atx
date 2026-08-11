"""Point-in-time rolling-WLS q5 expected investment growth."""

from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from functools import partial
from typing import Any

import numpy as np
import pandas as pd

from .asset_growth import FACTOR_ID as ASSET_GROWTH_FACTOR_ID
from .asset_growth import SOURCE_NAME as ASSET_GROWTH_SOURCE_NAME
from .connection import DuckDBStore
from .delta_roe import FACTOR_ID as DELTA_ROE_FACTOR_ID
from .expected_growth import (
    CASH_PROFITABILITY_FACTOR_ID,
    ExpectedGrowthOptions,
    load_expected_growth_inputs,
)
from .factors.cross_section import zscore
from .warehouse import insert_frame, json_dumps

SOURCE_NAME = "atx-db PIT q5 rolling WLS expected growth v1"
FACTOR_ID = "investment_q5_expected_growth_rolling_wls"
FACTOR_NAME = "PIT q5 rolling-WLS expected investment growth"
FACTOR_FAMILY = "fundamental_expected_growth"
MODEL_ID = "q5_expected_growth_wls_1y_v1"
_PREDICTORS = (
    "log_tobins_q",
    "cash_operating_profitability",
    "delta_roe",
)
_SLOPE_COLUMNS = [
    "slope_id",
    "model_id",
    "source",
    "as_of_date",
    "predictor_as_of_date",
    "available_at",
    "intercept",
    "log_tobins_q_slope",
    "cash_operating_profitability_slope",
    "delta_roe_slope",
    "n_obs",
    "condition_number",
    "weighted_r2",
    "training_sample_hash",
    "input_lineage_json",
    "is_latest_revision",
    "run_id",
]
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
class RollingExpectedGrowthOptions:
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    minimum_regression_names: int = 100
    maximum_condition_number: float = 1_000.0
    slope_window_months: int = 120
    minimum_slope_months: int = 30
    winsor_limit: float = 0.01
    source: str = SOURCE_NAME
    model_id: str = MODEL_ID
    run_id: str | None = None


def _stable_id(*parts: Any) -> str:
    return hashlib.sha256("|".join(str(part) for part in parts).encode()).hexdigest()


def load_investment_growth_targets(store: DuckDBStore) -> pd.DataFrame:
    """Build observable one-year changes in investment-to-assets from governed rows."""

    return store.con.execute(
        """
        WITH growth AS (
            SELECT
                factor_value_id,
                security_id,
                symbol,
                as_of_date,
                available_at,
                raw_value,
                CAST(json_extract_string(
                    input_lineage_json, '$.assets.current.period_end'
                ) AS DATE) AS current_period_end
            FROM fundamental_factor_values
            WHERE factor_id = ?
              AND source = ?
              AND is_latest_revision
              AND raw_value IS NOT NULL
              AND isfinite(raw_value)
        ),
        candidates AS (
            SELECT
                current.factor_value_id AS target_factor_value_id,
                current.security_id,
                current.symbol,
                current.as_of_date AS target_as_of_date,
                current.available_at AS target_available_at,
                current.current_period_end,
                -current.raw_value AS current_investment_to_assets,
                prior.factor_value_id AS prior_factor_value_id,
                prior.as_of_date AS prior_decision_date,
                prior.available_at AS prior_available_at,
                prior.current_period_end AS prior_period_end,
                -prior.raw_value AS prior_investment_to_assets,
                row_number() OVER (
                    PARTITION BY current.factor_value_id
                    ORDER BY
                        abs((current.current_period_end - prior.current_period_end) - 365),
                        prior.as_of_date DESC,
                        prior.available_at DESC,
                        prior.factor_value_id DESC
                ) AS prior_rank
            FROM growth current
            JOIN growth prior
              ON prior.security_id = current.security_id
             AND prior.current_period_end < current.current_period_end
             AND current.current_period_end - prior.current_period_end
                 BETWEEN 300 AND 430
             AND prior.as_of_date < current.as_of_date
             AND prior.available_at <= current.available_at
        )
        SELECT
            * EXCLUDE (prior_rank),
            current_investment_to_assets - prior_investment_to_assets
                AS target_delta_investment
        FROM candidates
        WHERE prior_rank = 1
        ORDER BY target_as_of_date, security_id
        """,
        [ASSET_GROWTH_FACTOR_ID, ASSET_GROWTH_SOURCE_NAME],
    ).df()


def prepare_training_panel(
    predictors: pd.DataFrame,
    targets: pd.DataFrame,
) -> pd.DataFrame:
    """Align each observable target with predictors from twelve calendar months earlier."""

    if predictors is None or predictors.empty or targets is None or targets.empty:
        return pd.DataFrame()
    required_predictors = {
        "security_id",
        "as_of_date",
        "decision_available_at",
        "market_equity",
        "tobins_q",
        "cash_operating_profitability",
        "delta_roe",
        "asset_growth_factor_value_id",
        "cash_profitability_factor_value_id",
        "delta_roe_factor_value_id",
    }
    required_targets = {
        "target_factor_value_id",
        "prior_factor_value_id",
        "security_id",
        "target_as_of_date",
        "target_available_at",
        "target_delta_investment",
    }
    missing_predictors = sorted(required_predictors.difference(predictors.columns))
    missing_targets = sorted(required_targets.difference(targets.columns))
    if missing_predictors:
        raise ValueError(f"Rolling-WLS predictors missing columns: {missing_predictors}")
    if missing_targets:
        raise ValueError(f"Investment-growth targets missing columns: {missing_targets}")

    inputs = predictors.copy()
    inputs["as_of_date"] = pd.to_datetime(inputs["as_of_date"], errors="coerce")
    inputs["month"] = inputs["as_of_date"].dt.to_period("M")
    inputs["log_tobins_q"] = np.log(
        pd.to_numeric(inputs["tobins_q"], errors="coerce")
    )
    current_weights = inputs[
        ["security_id", "month", "market_equity", "decision_available_at"]
    ].rename(
        columns={
            "market_equity": "training_market_equity",
            "decision_available_at": "weight_available_at",
        }
    )
    lagged = inputs[
        [
            "security_id",
            "month",
            "as_of_date",
            "decision_available_at",
            *_PREDICTORS,
            "asset_growth_factor_value_id",
            "cash_profitability_factor_value_id",
            "delta_roe_factor_value_id",
        ]
    ].copy()
    lagged["month"] = lagged["month"] + 12
    lagged = lagged.rename(
        columns={
            "as_of_date": "predictor_as_of_date",
            "decision_available_at": "predictor_available_at",
        }
    )

    observations = targets.copy()
    observations["target_as_of_date"] = pd.to_datetime(
        observations["target_as_of_date"], errors="coerce"
    )
    observations["month"] = observations["target_as_of_date"].dt.to_period("M")
    training = observations.merge(
        current_weights,
        on=["security_id", "month"],
        how="inner",
        validate="many_to_one",
    ).merge(
        lagged,
        on=["security_id", "month"],
        how="inner",
        validate="many_to_one",
    )
    training["target_available_at"] = pd.to_datetime(
        training["target_available_at"], errors="coerce"
    )
    training["weight_available_at"] = pd.to_datetime(
        training["weight_available_at"], errors="coerce"
    )
    training["predictor_available_at"] = pd.to_datetime(
        training["predictor_available_at"], errors="coerce"
    )
    training["training_available_at"] = training[
        ["target_available_at", "weight_available_at", "predictor_available_at"]
    ].max(axis=1)
    for column in ["target_delta_investment", "training_market_equity", *_PREDICTORS]:
        training[column] = pd.to_numeric(training[column], errors="coerce")
    training = training.dropna(
        subset=[
            "security_id",
            "target_as_of_date",
            "training_available_at",
            "target_delta_investment",
            "training_market_equity",
            *_PREDICTORS,
        ]
    )
    finite = training[["target_delta_investment", *_PREDICTORS]].apply(
        lambda column: column.map(math.isfinite)
    ).all(axis=1)
    training = training[finite & (training["training_market_equity"] > 0)].copy()
    return training.sort_values(
        ["month", "security_id"], kind="stable"
    ).reset_index(drop=True)


def _winsorized_group(
    group: pd.DataFrame,
    columns: tuple[str, ...],
    limit: float,
) -> pd.DataFrame:
    out = group.copy()
    for column in columns:
        values = pd.to_numeric(out[column], errors="coerce")
        out[f"winsorized_{column}"] = values.clip(
            lower=values.quantile(limit),
            upper=values.quantile(1.0 - limit),
        )
    return out


def fit_monthly_expected_growth_slopes(
    training: pd.DataFrame,
    options: RollingExpectedGrowthOptions | None = None,
) -> pd.DataFrame:
    """Estimate reliable monthly market-equity-weighted accounting forecasts."""

    options = options or RollingExpectedGrowthOptions()
    if training is None or training.empty:
        return pd.DataFrame(columns=_SLOPE_COLUMNS)
    rows: list[dict[str, object]] = []
    regression_columns = ("target_delta_investment", *_PREDICTORS)
    for month, raw_group in training.groupby("month", sort=True):
        group = _winsorized_group(raw_group, regression_columns, options.winsor_limit)
        group = group.dropna(
            subset=[
                *(f"winsorized_{column}" for column in regression_columns),
                "training_market_equity",
            ]
        )
        if group["security_id"].nunique() < options.minimum_regression_names:
            continue
        design = np.column_stack(
            [
                np.ones(len(group), dtype=float),
                group[[f"winsorized_{name}" for name in _PREDICTORS]].to_numpy(
                    dtype=float
                ),
            ]
        )
        target = group["winsorized_target_delta_investment"].to_numpy(dtype=float)
        weights = group["training_market_equity"].to_numpy(dtype=float).copy()
        weights = weights / weights.mean()
        root_weights = np.sqrt(weights)
        weighted_design = design * root_weights[:, None]
        if np.linalg.matrix_rank(weighted_design) < design.shape[1]:
            continue
        condition_number = float(np.linalg.cond(weighted_design))
        if not math.isfinite(condition_number):
            continue
        if condition_number > options.maximum_condition_number:
            continue
        coefficients = np.linalg.lstsq(
            weighted_design,
            target * root_weights,
            rcond=None,
        )[0]
        fitted = design @ coefficients
        target_mean = float(np.average(target, weights=weights))
        total_variation = float(np.sum(weights * (target - target_mean) ** 2))
        residual_variation = float(np.sum(weights * (target - fitted) ** 2))
        weighted_r2 = (
            1.0 - residual_variation / total_variation
            if total_variation > 0
            else float("nan")
        )
        sample_parts = (
            group["target_factor_value_id"].astype(str)
            + "|"
            + group["prior_factor_value_id"].astype(str)
            + "|"
            + group["asset_growth_factor_value_id"].astype(str)
            + "|"
            + group["cash_profitability_factor_value_id"].astype(str)
            + "|"
            + group["delta_roe_factor_value_id"].fillna("missing").astype(str)
        )
        sample_hash = hashlib.sha256(
            "\n".join(sorted(sample_parts.tolist())).encode()
        ).hexdigest()
        as_of_date = pd.to_datetime(group["target_as_of_date"]).max().date()
        predictor_as_of_date = pd.to_datetime(
            group["predictor_as_of_date"]
        ).max().date()
        available_at = pd.to_datetime(group["training_available_at"]).max()
        slope_id = _stable_id(options.source, options.model_id, as_of_date)
        lineage = json_dumps(
            {
                "method": "hou_mo_xue_zhang_monthly_market_cap_weighted_wls",
                "target": "investment_to_assets_t - investment_to_assets_t_minus_1",
                "predictors": list(_PREDICTORS),
                "predictor_lag_months": 12,
                "weight": "current_training_month_market_equity",
                "winsor_limits": [options.winsor_limit, options.winsor_limit],
                "minimum_regression_names": options.minimum_regression_names,
                "maximum_condition_number": options.maximum_condition_number,
                "month": str(month),
                "target_date_range": [
                    pd.to_datetime(group["target_as_of_date"]).min().date(),
                    as_of_date,
                ],
                "predictor_date_range": [
                    pd.to_datetime(group["predictor_as_of_date"]).min().date(),
                    predictor_as_of_date,
                ],
                "n_obs": len(group),
                "training_sample_hash": sample_hash,
                "return_fitted_parameters": False,
            }
        )
        rows.append(
            {
                "slope_id": slope_id,
                "model_id": options.model_id,
                "source": options.source,
                "as_of_date": as_of_date,
                "predictor_as_of_date": predictor_as_of_date,
                "available_at": available_at,
                "intercept": float(coefficients[0]),
                "log_tobins_q_slope": float(coefficients[1]),
                "cash_operating_profitability_slope": float(coefficients[2]),
                "delta_roe_slope": float(coefficients[3]),
                "n_obs": len(group),
                "condition_number": condition_number,
                "weighted_r2": weighted_r2,
                "training_sample_hash": sample_hash,
                "input_lineage_json": lineage,
                "is_latest_revision": True,
                "run_id": options.run_id,
            }
        )
    return pd.DataFrame(rows, columns=_SLOPE_COLUMNS).sort_values(
        "as_of_date", kind="stable"
    ).reset_index(drop=True)


def _forecast_lineage(
    row: pd.Series,
    *,
    options: RollingExpectedGrowthOptions,
    coefficients: dict[str, float],
    slope_count: int,
    slope_first_date: dt.date,
    slope_last_date: dt.date,
    slope_window_hash: str,
) -> str:
    return json_dumps(
        {
            "method": "hou_mo_xue_zhang_prior_rolling_average_wls_slopes",
            "model_id": options.model_id,
            "formula": "intercept+b_q*ln(q)+b_cop*Cop+b_droe*dROE",
            "orientation": "higher_is_higher_expected_investment_growth",
            "model_window": {
                "maximum_months": options.slope_window_months,
                "minimum_slope_months": options.minimum_slope_months,
                "slope_count": slope_count,
                "first_slope_date": slope_first_date,
                "last_slope_date": slope_last_date,
                "strictly_prior_to_forecast": True,
                "slope_window_hash": slope_window_hash,
            },
            "average_coefficients": coefficients,
            "predictors": {
                "log_tobins_q": row["log_tobins_q"],
                "winsorized_log_tobins_q": row["winsorized_log_tobins_q"],
                "cash_operating_profitability": row[
                    "cash_operating_profitability"
                ],
                "winsorized_cash_operating_profitability": row[
                    "winsorized_cash_operating_profitability"
                ],
                "delta_roe": row["delta_roe"],
                "winsorized_delta_roe": row["winsorized_delta_roe"],
                "delta_roe_imputed_zero": bool(row["delta_roe_imputed_zero"]),
            },
            "upstream_factor_value_ids": {
                "asset_growth": row["asset_growth_factor_value_id"],
                "cash_profitability": row["cash_profitability_factor_value_id"],
                "delta_roe": row["delta_roe_factor_value_id"],
            },
            "forecast": row["raw_value"],
            "return_fitted_parameters": False,
        }
    )


def compute_rolling_expected_growth_rows(
    predictors: pd.DataFrame,
    slopes: pd.DataFrame,
    options: RollingExpectedGrowthOptions | None = None,
) -> pd.DataFrame:
    """Forecast with only strictly prior eligible slopes and standardize by date."""

    options = options or RollingExpectedGrowthOptions()
    if predictors is None or predictors.empty or slopes is None or slopes.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    rows = predictors.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce")
    rows["month"] = rows["as_of_date"].dt.to_period("M")
    rows["log_tobins_q"] = np.log(pd.to_numeric(rows["tobins_q"], errors="coerce"))
    for column in _PREDICTORS:
        rows[column] = pd.to_numeric(rows[column], errors="coerce")
    finite = rows[list(_PREDICTORS)].apply(
        lambda column: column.map(math.isfinite)
    ).all(axis=1)
    rows = rows[finite].copy()

    model = slopes.copy()
    model["as_of_date"] = pd.to_datetime(model["as_of_date"], errors="coerce")
    model["month"] = model["as_of_date"].dt.to_period("M")
    model["available_at"] = pd.to_datetime(model["available_at"], errors="coerce")
    parts: list[pd.DataFrame] = []
    slope_columns = {
        "intercept": "intercept",
        "log_tobins_q": "log_tobins_q_slope",
        "cash_operating_profitability": "cash_operating_profitability_slope",
        "delta_roe": "delta_roe_slope",
    }
    for as_of_date, raw_group in rows.groupby("as_of_date", sort=True):
        month = as_of_date.to_period("M")
        history = model[
            (model["month"] < month)
            & (model["month"] >= month - options.slope_window_months)
        ].copy()
        if len(history) < options.minimum_slope_months:
            continue
        group = _winsorized_group(raw_group, _PREDICTORS, options.winsor_limit)
        coefficients = {
            name: float(history[column].mean())
            for name, column in slope_columns.items()
        }
        group["raw_value"] = coefficients["intercept"]
        for predictor in _PREDICTORS:
            group["raw_value"] += (
                coefficients[predictor] * group[f"winsorized_{predictor}"]
            )
        slope_window_hash = hashlib.sha256(
            "\n".join(sorted(history["slope_id"].astype(str).tolist())).encode()
        ).hexdigest()
        slope_first_date = history["as_of_date"].min().date()
        slope_last_date = history["as_of_date"].max().date()
        model_available_at = history["available_at"].max()
        group["available_at"] = pd.to_datetime(
            group["decision_available_at"], errors="coerce"
        ).where(
            pd.to_datetime(group["decision_available_at"], errors="coerce")
            >= model_available_at,
            model_available_at,
        )
        lineage_builder = partial(
            _forecast_lineage,
            options=options,
            coefficients=coefficients,
            slope_count=len(history),
            slope_first_date=slope_first_date,
            slope_last_date=slope_last_date,
            slope_window_hash=slope_window_hash,
        )
        group["input_lineage_json"] = group.apply(lineage_builder, axis=1)
        parts.append(group)
    if not parts:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    out = pd.concat(parts, ignore_index=True)
    out["as_of_date"] = pd.to_datetime(out["as_of_date"]).dt.date
    if options.start_date is not None:
        out = out[out["as_of_date"] >= options.start_date].copy()
    if options.end_date is not None:
        out = out[out["as_of_date"] <= options.end_date].copy()
    if out.empty:
        return pd.DataFrame(columns=_OUTPUT_COLUMNS)
    out["factor_id"] = FACTOR_ID
    out["factor_name"] = FACTOR_NAME
    out["family"] = FACTOR_FAMILY
    out = zscore(
        out,
        value_column="raw_value",
        output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    out["input_ids_json"] = json_dumps(
        [
            f"factor:{ASSET_GROWTH_FACTOR_ID}",
            f"factor:{CASH_PROFITABILITY_FACTOR_ID}",
            f"factor:{DELTA_ROE_FACTOR_ID}",
            "source:sec_company_facts",
            "dataset:expected_growth_model_slopes",
        ]
    )
    out["is_latest_revision"] = True
    out["run_id"] = options.run_id
    out["source"] = options.source
    out["factor_value_id"] = [
        _stable_id(options.source, FACTOR_ID, security_id, as_of_date)
        for security_id, as_of_date in zip(
            out["security_id"], out["as_of_date"], strict=True
        )
    ]
    return (
        out[_OUTPUT_COLUMNS]
        .dropna(subset=["value"])
        .sort_values(["as_of_date", "security_id"], kind="stable")
        .reset_index(drop=True)
    )


def refresh_rolling_expected_growth_values(
    store: DuckDBStore,
    options: RollingExpectedGrowthOptions | None = None,
) -> dict[str, int]:
    """Replace governed monthly slopes and materialize their PIT forecasts."""

    options = options or RollingExpectedGrowthOptions()
    store.initialize()
    predictor_options = ExpectedGrowthOptions()
    predictors = load_expected_growth_inputs(store, predictor_options)
    targets = load_investment_growth_targets(store)
    training = prepare_training_panel(predictors, targets)
    slopes = fit_monthly_expected_growth_slopes(training, options)
    factor_rows = compute_rolling_expected_growth_rows(predictors, slopes, options)
    predicates = ["source = ?", "factor_id = ?"]
    factor_params: list[object] = [options.source, FACTOR_ID]
    if options.start_date is not None:
        predicates.append("as_of_date >= ?")
        factor_params.append(options.start_date)
    if options.end_date is not None:
        predicates.append("as_of_date <= ?")
        factor_params.append(options.end_date)
    with store.transaction():
        store.con.execute(
            "DELETE FROM expected_growth_model_slopes WHERE source=? AND model_id=?",
            [options.source, options.model_id],
        )
        if not slopes.empty:
            insert_frame(
                store,
                slopes,
                "expected_growth_model_slopes",
                "expected_growth_slopes_insert",
            )
        store.con.execute(
            f"DELETE FROM fundamental_factor_values WHERE {' AND '.join(predicates)}",
            factor_params,
        )
        if not factor_rows.empty:
            insert_frame(
                store,
                factor_rows,
                "fundamental_factor_values",
                "rolling_expected_growth_insert",
            )
    return {"slopes": len(slopes), "factor_rows": len(factor_rows)}
