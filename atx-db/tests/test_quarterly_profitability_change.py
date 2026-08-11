from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.quarterly_profitability_change import (
    FACTOR_ID,
    SOURCE_NAME,
    QuarterlyProfitabilityChangeOptions,
    compute_quarterly_profitability_change_rows,
)


def _row(
    security_id: str,
    current_profitability: float,
    prior_profitability: float,
) -> dict[str, object]:
    return {
        "revenue_growth_factor_value_id": f"{security_id}-growth",
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "revenue_growth_available_at": dt.datetime(2025, 1, 31, 22),
        "current_qop_factor_value_id": f"{security_id}-current",
        "current_period_end": dt.date(2024, 12, 31),
        "current_qop_raw_value": current_profitability,
        "current_qop_value": current_profitability,
        "current_qop_available_at": dt.datetime(2025, 1, 30, 22),
        "prior_qop_factor_value_id": f"{security_id}-prior",
        "prior_period_end": dt.date(2023, 12, 31),
        "prior_qop_raw_value": prior_profitability,
        "prior_qop_value": prior_profitability,
        "prior_qop_available_at": dt.datetime(2024, 1, 30, 22),
        "period_gap_days": 366,
    }


def test_profitability_change_orientation_availability_and_lineage() -> None:
    result = compute_quarterly_profitability_change_rows(
        pd.DataFrame(
            [
                _row("LOW", 0.10, 0.20),
                _row("MID", 0.20, 0.20),
                _row("HIGH", 0.35, 0.20),
            ]
        ),
        QuarterlyProfitabilityChangeOptions(minimum_names_per_date=2),
    ).set_index("security_id")
    assert result.loc["HIGH", "value"] > result.loc["MID", "value"]
    assert result.loc["MID", "value"] > result.loc["LOW", "value"]
    assert result.loc["HIGH", "available_at"] == pd.Timestamp("2025-01-31 22:00:00")
    lineage = json.loads(result.loc["HIGH", "input_lineage_json"])
    assert lineage["formula"] == (
        "quarterly_operating_profitability_t-quarterly_operating_profitability_t_4"
    )
    assert lineage["profitability_change"] == pytest.approx(0.15)
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_profitability_change_rejects_nonfinite_and_extreme_changes() -> None:
    result = compute_quarterly_profitability_change_rows(
        pd.DataFrame(
                [
                    _row("GOOD", 0.30, 0.20),
                    _row("GOOD_2", 0.40, 0.20),
                    _row("EXTREME", 20.0, 0.0),
                    _row("NONFINITE", float("inf"), 0.0),
            ]
        ),
        QuarterlyProfitabilityChangeOptions(minimum_names_per_date=1),
    )
    assert set(result["security_id"]) == {"GOOD", "GOOD_2"}


def test_profitability_change_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,direction,is_point_in_time_safe,source,
                  standardization_spec_json FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition is not None
    assert definition[0] == "zscore(winsorize_1pct(qop_t-qop_t_4))"
    assert definition[1:4] == (1, True, SOURCE_NAME)
    spec = json.loads(definition[4])
    assert spec["maximum_absolute_change"] == 10.0
    assert spec["return_fitted_parameters"] is False
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name FROM factor_dependency_edges
           WHERE factor_id=? ORDER BY dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("factor", "growth_quarterly_revenue_yoy"),
        ("factor", "profitability_quarterly_operating_profitability_lagged_assets"),
    ]
