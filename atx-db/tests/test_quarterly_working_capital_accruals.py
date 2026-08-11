from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.quarterly_working_capital_accruals import (
    FACTOR_ID,
    SOURCE_NAME,
    QuarterlyWorkingCapitalAccrualsOptions,
    compute_quarterly_working_capital_accruals_rows,
)


def _row(security_id: str, net_change: float, assets: float = 100.0) -> dict[str, object]:
    return {
        "cash_factor_value_id": f"{security_id}-cash",
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "decision_available_at": dt.datetime(2025, 1, 31, 22),
        "net_operating_working_capital_change": net_change,
        "lagged_total_assets": assets,
        "delta_ar": net_change,
        "delta_inventory": 0.0,
        "delta_deferred_revenue": 0.0,
        "delta_ap": 0.0,
        "ar_missing": False,
        "inventory_missing": True,
        "deferred_revenue_missing": True,
        "ap_missing": True,
        "current_period_end": dt.date(2024, 12, 31),
        "prior_period_end": dt.date(2024, 9, 30),
    }


def test_low_quarterly_accruals_orientation_and_lineage() -> None:
    rows = compute_quarterly_working_capital_accruals_rows(
        pd.DataFrame([_row("LOW", -10.0), _row("MID", 0.0), _row("HIGH", 10.0)]),
        QuarterlyWorkingCapitalAccrualsOptions(
            minimum_names_per_date=2,
            winsor_limit=0.0,
        ),
    ).set_index("security_id")

    assert rows.loc["LOW", "raw_value"] == pytest.approx(-0.1)
    assert rows.loc["HIGH", "raw_value"] == pytest.approx(0.1)
    assert rows.loc["LOW", "value"] > rows.loc["MID", "value"]
    assert rows.loc["MID", "value"] > rows.loc["HIGH", "value"]
    lineage = json.loads(rows.loc["LOW", "input_lineage_json"])
    assert lineage["orientation"] == "lower_accruals_are_higher_quality"
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_low_quarterly_accruals_rejects_bad_denominators_and_scale() -> None:
    rows = compute_quarterly_working_capital_accruals_rows(
        pd.DataFrame(
            [
                _row("GOOD", 5.0),
                _row("ALSO_GOOD", -5.0),
                _row("ZERO", 5.0, assets=0.0),
                _row("SCALE", 1000.0, assets=10.0),
            ]
        ),
        QuarterlyWorkingCapitalAccrualsOptions(
            minimum_names_per_date=1,
            winsor_limit=0.0,
        ),
    )
    assert set(rows["security_id"]) == {"GOOD", "ALSO_GOOD"}


def test_low_quarterly_accruals_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,direction,is_point_in_time_safe,source,
                  standardization_spec_json
           FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition is not None
    assert definition[0] == (
        "-(dAR+dInventory-dDeferredRevenue-dAP)/one_quarter_lagged_total_assets"
    )
    assert definition[1:4] == (-1, True, SOURCE_NAME)
    standardization = json.loads(definition[4])
    assert standardization["missing_balance_changes"] == "inherited_zero_from_claq"
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name
           FROM factor_dependency_edges WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        (
            "factor",
            "profitability_quarterly_cash_operating_profitability_lagged_assets",
        )
    ]
