from __future__ import annotations

import datetime as dt
import json

import numpy as np
import pandas as pd
import pytest

from atx_db.expected_growth_rolling import (
    FACTOR_ID,
    MODEL_ID,
    SOURCE_NAME,
    RollingExpectedGrowthOptions,
    compute_rolling_expected_growth_rows,
    fit_monthly_expected_growth_slopes,
)


def _training_rows(n: int = 120) -> pd.DataFrame:
    q = np.linspace(-1.0, 1.0, n)
    cash = np.sin(np.linspace(0.0, 4.0, n))
    droe = np.cos(np.linspace(0.0, 5.0, n))
    target = 1.0 + 2.0 * q + 3.0 * cash + 4.0 * droe
    return pd.DataFrame(
        {
            "month": pd.Period("2025-06", freq="M"),
            "security_id": [f"S{i:03d}" for i in range(n)],
            "target_as_of_date": pd.Timestamp("2025-06-30"),
            "predictor_as_of_date": pd.Timestamp("2024-06-28"),
            "training_available_at": pd.Timestamp("2025-06-30 22:00:00"),
            "target_delta_investment": target,
            "training_market_equity": np.linspace(100.0, 10_000.0, n),
            "log_tobins_q": q,
            "cash_operating_profitability": cash,
            "delta_roe": droe,
            "target_factor_value_id": [f"target-{i}" for i in range(n)],
            "prior_factor_value_id": [f"prior-{i}" for i in range(n)],
            "asset_growth_factor_value_id": [f"growth-{i}" for i in range(n)],
            "cash_profitability_factor_value_id": [f"cash-{i}" for i in range(n)],
            "delta_roe_factor_value_id": [f"droe-{i}" for i in range(n)],
        }
    )


def test_monthly_wls_recovers_accounting_target_coefficients() -> None:
    slopes = fit_monthly_expected_growth_slopes(
        _training_rows(),
        RollingExpectedGrowthOptions(
            minimum_regression_names=100,
            maximum_condition_number=1_000.0,
            winsor_limit=0.0,
        ),
    )
    assert len(slopes) == 1
    row = slopes.iloc[0]
    assert row["intercept"] == pytest.approx(1.0)
    assert row["log_tobins_q_slope"] == pytest.approx(2.0)
    assert row["cash_operating_profitability_slope"] == pytest.approx(3.0)
    assert row["delta_roe_slope"] == pytest.approx(4.0)
    assert row["weighted_r2"] == pytest.approx(1.0)
    lineage = json.loads(row["input_lineage_json"])
    assert lineage["predictor_lag_months"] == 12
    assert lineage["return_fitted_parameters"] is False


def _forecast_predictors() -> pd.DataFrame:
    return pd.DataFrame(
        {
            "security_id": ["HIGH", "MID", "LOW"],
            "symbol": ["HIGH", "MID", "LOW"],
            "as_of_date": [dt.date(2025, 6, 30)] * 3,
            "decision_available_at": [pd.Timestamp("2025-06-30 22:00:00")] * 3,
            "tobins_q": [1.0, 1.0, 1.0],
            "cash_operating_profitability": [0.2, 0.1, 0.0],
            "delta_roe": [0.2, 0.0, -0.2],
            "delta_roe_imputed_zero": [False, True, False],
            "asset_growth_factor_value_id": ["g-high", "g-mid", "g-low"],
            "cash_profitability_factor_value_id": ["c-high", "c-mid", "c-low"],
            "delta_roe_factor_value_id": ["d-high", None, "d-low"],
        }
    )


def _slope_row(date: dt.date, slope_id: str, *, delta_slope: float) -> dict[str, object]:
    return {
        "slope_id": slope_id,
        "model_id": MODEL_ID,
        "source": SOURCE_NAME,
        "as_of_date": date,
        "available_at": pd.Timestamp(date) + pd.Timedelta(hours=22),
        "intercept": 0.0,
        "log_tobins_q_slope": 0.0,
        "cash_operating_profitability_slope": 1.0,
        "delta_roe_slope": delta_slope,
    }


def test_forecast_uses_only_strictly_prior_slopes() -> None:
    slopes = pd.DataFrame(
        [
            _slope_row(dt.date(2025, 4, 30), "prior-a", delta_slope=1.0),
            _slope_row(dt.date(2025, 5, 30), "prior-b", delta_slope=1.0),
            _slope_row(dt.date(2025, 6, 30), "same-date", delta_slope=-100.0),
        ]
    )
    rows = compute_rolling_expected_growth_rows(
        _forecast_predictors(),
        slopes,
        RollingExpectedGrowthOptions(
            minimum_slope_months=2,
            slope_window_months=120,
            winsor_limit=0.0,
        ),
    ).set_index("security_id")
    assert rows.loc["HIGH", "raw_value"] > rows.loc["MID", "raw_value"]
    assert rows.loc["MID", "raw_value"] > rows.loc["LOW", "raw_value"]
    lineage = json.loads(rows.loc["HIGH", "input_lineage_json"])
    assert lineage["model_window"]["slope_count"] == 2
    assert lineage["model_window"]["last_slope_date"] == "2025-05-30"
    assert lineage["model_window"]["strictly_prior_to_forecast"] is True


def test_rolling_expected_growth_definition_and_slope_table_are_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT is_point_in_time_safe,source,standardization_spec_json
           FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition is not None
    assert definition[:2] == (True, SOURCE_NAME)
    standardization = json.loads(definition[2])
    assert standardization["minimum_regression_names"] == 100
    assert standardization["minimum_slope_months"] == 30
    columns = {
        row[0]
        for row in tmp_store.con.execute(
            "DESCRIBE expected_growth_model_slopes"
        ).fetchall()
    }
    assert {
        "slope_id",
        "model_id",
        "as_of_date",
        "available_at",
        "delta_roe_slope",
        "training_sample_hash",
    } <= columns
