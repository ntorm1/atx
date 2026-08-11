from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.quarterly_abnormal_inventory_growth import (
    FACTOR_ID,
    SOURCE_NAME,
    QuarterlyAbnormalInventoryGrowthOptions,
    compute_quarterly_abnormal_inventory_growth_rows,
)


def _row(
    security_id: str,
    *,
    current_inventory: float,
    current_revenue: float,
    prior_inventory: float = 100.0,
    prior_revenue: float = 100.0,
) -> dict[str, object]:
    return {
        "cash_factor_value_id": f"{security_id}-cash-current",
        "qop_factor_value_id": f"{security_id}-qop-current",
        "prior_cash_factor_value_id": f"{security_id}-cash-prior",
        "prior_qop_factor_value_id": f"{security_id}-qop-prior",
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "decision_available_at": dt.datetime(2025, 1, 31, 22),
        "prior_decision_available_at": dt.datetime(2024, 1, 31, 22),
        "current_period_end": dt.date(2024, 12, 31),
        "prior_period_end": dt.date(2023, 12, 31),
        "period_gap_days": 366,
        "current_inventory": current_inventory,
        "prior_inventory": prior_inventory,
        "current_revenue": current_revenue,
        "prior_revenue": prior_revenue,
    }


def test_abnormal_inventory_growth_formula_orientation_and_lineage() -> None:
    result = compute_quarterly_abnormal_inventory_growth_rows(
        pd.DataFrame(
            [
                _row("LOW", current_inventory=100.0, current_revenue=120.0),
                _row("MID", current_inventory=110.0, current_revenue=110.0),
                _row("HIGH", current_inventory=120.0, current_revenue=100.0),
            ]
        ),
        QuarterlyAbnormalInventoryGrowthOptions(
            minimum_names_per_date=2,
            winsor_limit=0.0,
        ),
    ).set_index("security_id")

    assert result.loc["LOW", "raw_value"] == pytest.approx(-0.2)
    assert result.loc["MID", "raw_value"] == pytest.approx(0.0)
    assert result.loc["HIGH", "raw_value"] == pytest.approx(0.2)
    assert result.loc["LOW", "value"] > result.loc["MID", "value"]
    assert result.loc["MID", "value"] > result.loc["HIGH", "value"]
    lineage = json.loads(result.loc["LOW", "input_lineage_json"])
    assert lineage["inventory_growth"] == pytest.approx(0.0)
    assert lineage["sales_growth"] == pytest.approx(0.2)
    assert lineage["period_gap_days"] == 366
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_abnormal_inventory_growth_rejects_invalid_components_and_scale() -> None:
    result = compute_quarterly_abnormal_inventory_growth_rows(
        pd.DataFrame(
            [
                _row("GOOD", current_inventory=105.0, current_revenue=103.0),
                _row("GOOD_2", current_inventory=95.0, current_revenue=97.0),
                _row(
                    "NO_PRIOR_INVENTORY",
                    current_inventory=100.0,
                    current_revenue=100.0,
                    prior_inventory=0.0,
                ),
                _row("OUTLIER", current_inventory=1200.0, current_revenue=100.0),
            ]
        ),
        QuarterlyAbnormalInventoryGrowthOptions(
            minimum_names_per_date=1,
            winsor_limit=0.0,
        ),
    )
    assert set(result["security_id"]) == {"GOOD", "GOOD_2"}


def test_abnormal_inventory_growth_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,direction,is_point_in_time_safe,source,
                  standardization_spec_json
           FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition is not None
    assert definition[0] == (
        "-((inventory_t/inventory_t_4-1)-(revenue_t/revenue_t_4-1))"
    )
    assert definition[1:4] == (-1, True, SOURCE_NAME)
    standardization = json.loads(definition[4])
    assert standardization["period_gap_days"] == [330, 400]
    assert standardization["return_fitted_parameters"] is False
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name
           FROM factor_dependency_edges WHERE factor_id=? ORDER BY dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        (
            "factor",
            "profitability_quarterly_cash_operating_profitability_lagged_assets",
        ),
        (
            "factor",
            "profitability_quarterly_operating_profitability_lagged_assets",
        ),
    ]
