from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.quarterly_revenue_growth import (
    FACTOR_ID,
    SOURCE_NAME,
    QuarterlyRevenueGrowthOptions,
    compute_quarterly_revenue_growth_rows,
)


def _row(security_id: str, current: float, prior: float = 100.0) -> dict[str, object]:
    return {
        "qop_factor_value_id": f"{security_id}-current",
        "prior_qop_factor_value_id": f"{security_id}-prior",
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "decision_available_at": dt.datetime(2025, 1, 31, 22),
        "prior_decision_available_at": dt.datetime(2024, 1, 31, 22),
        "current_period_end": dt.date(2024, 12, 31),
        "prior_period_end": dt.date(2023, 12, 31),
        "period_gap_days": 366,
        "current_revenue": current,
        "prior_revenue": prior,
    }


def test_quarterly_revenue_growth_formula_orientation_and_lineage() -> None:
    result = compute_quarterly_revenue_growth_rows(
        pd.DataFrame([_row("LOW", 80.0), _row("MID", 100.0), _row("HIGH", 120.0)]),
        QuarterlyRevenueGrowthOptions(minimum_names_per_date=2, winsor_limit=0.0),
    ).set_index("security_id")

    assert result.loc["LOW", "raw_value"] == pytest.approx(-0.2)
    assert result.loc["MID", "raw_value"] == pytest.approx(0.0)
    assert result.loc["HIGH", "raw_value"] == pytest.approx(0.2)
    assert result.loc["HIGH", "value"] > result.loc["MID", "value"]
    assert result.loc["MID", "value"] > result.loc["LOW", "value"]
    lineage = json.loads(result.loc["HIGH", "input_lineage_json"])
    assert lineage["orientation"] == "higher_revenue_growth_is_preferred"
    assert lineage["period_gap_days"] == 366
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_quarterly_revenue_growth_rejects_bad_denominators_and_scale() -> None:
    result = compute_quarterly_revenue_growth_rows(
        pd.DataFrame(
            [
                _row("GOOD", 105.0),
                _row("GOOD_2", 95.0),
                _row("ZERO", 100.0, prior=0.0),
                _row("OUTLIER", 1200.0),
            ]
        ),
        QuarterlyRevenueGrowthOptions(minimum_names_per_date=1, winsor_limit=0.0),
    )
    assert set(result["security_id"]) == {"GOOD", "GOOD_2"}


def test_quarterly_revenue_growth_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,direction,is_point_in_time_safe,source,
                  standardization_spec_json
           FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition is not None
    assert definition[0] == "revenue_t/revenue_t_4-1"
    assert definition[1:4] == (1, True, SOURCE_NAME)
    standardization = json.loads(definition[4])
    assert standardization["period_gap_days"] == [330, 400]
    assert standardization["return_fitted_parameters"] is False
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name
           FROM factor_dependency_edges WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        (
            "factor",
            "profitability_quarterly_operating_profitability_lagged_assets",
        )
    ]
