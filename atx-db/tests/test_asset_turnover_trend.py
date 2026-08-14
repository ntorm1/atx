from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.asset_turnover_trend import (
    FACTOR_ID,
    SOURCE_NAME,
    AssetTurnoverTrendOptions,
    compute_asset_turnover_trend_rows,
)


def _row(security_id: str, trend: float) -> dict[str, object]:
    return {
        "trend_grid_factor_value_id": f"{security_id}-grid",
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 2, 28),
        "decision_available_at": dt.datetime(2025, 2, 28, 22),
        "asset_turnover_trend": trend,
        "history_json": json.dumps(
            [
                {
                    "qgp_factor_value_id": f"{security_id}-q{quarter}",
                    "asset_turnover": 1.0 + trend * quarter,
                }
                for quarter in range(8)
            ]
        ),
        "oldest_period_end": dt.date(2023, 3, 31),
        "latest_period_end": dt.date(2024, 12, 31),
        "history_span_days": 641,
        "observation_count": 8,
        "seasonal_quarter_count": 4,
    }


def test_asset_turnover_trend_orientation_and_lineage() -> None:
    result = compute_asset_turnover_trend_rows(
        pd.DataFrame([_row("LOW", -0.02), _row("MID", 0.0), _row("HIGH", 0.02)]),
        AssetTurnoverTrendOptions(minimum_names_per_date=2, winsor_limit=0.0),
    ).set_index("security_id")
    assert result.loc["HIGH", "raw_value"] == pytest.approx(0.02)
    assert result.loc["HIGH", "value"] > result.loc["MID", "value"]
    assert result.loc["MID", "value"] > result.loc["LOW", "value"]
    lineage = json.loads(result.loc["HIGH", "input_lineage_json"])
    assert len(lineage["history"]) == 8
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_asset_turnover_trend_rejects_nonfinite_and_extreme_values() -> None:
    result = compute_asset_turnover_trend_rows(
        pd.DataFrame(
            [
                _row("GOOD", 0.02),
                _row("GOOD_2", -0.02),
                _row("EXTREME", 6.0),
                _row("NONFINITE", float("inf")),
            ]
        ),
        AssetTurnoverTrendOptions(minimum_names_per_date=1, winsor_limit=0.0),
    )
    assert result["security_id"].tolist() == ["GOOD", "GOOD_2"]


def test_asset_turnover_trend_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,direction,is_point_in_time_safe,source,
                  standardization_spec_json FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition[:4] == (
        "ols_slope_8obs(revenue/lagged_assets ~ elapsed_quarters + calendar_quarter)",
        1,
        True,
        SOURCE_NAME,
    )
    standardization = json.loads(definition[4])
    assert standardization["observations"] == 8
    assert standardization["return_fitted_parameters"] is False
    assert tmp_store.con.execute(
        """SELECT dependency_name FROM factor_dependency_edges
           WHERE factor_id=? ORDER BY 1""",
        [FACTOR_ID],
    ).fetchall() == [
        ("profitability_quarterly_gross_profitability_lagged_assets",),
        ("profitability_quarterly_gross_profitability_trend_8q",),
    ]
