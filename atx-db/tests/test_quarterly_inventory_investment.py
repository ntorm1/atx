from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.quarterly_inventory_investment import (
    INVENTORY_CHANGE_FACTOR_ID,
    INVENTORY_GROWTH_FACTOR_ID,
    SOURCE_NAME,
    QuarterlyInventoryInvestmentOptions,
    compute_quarterly_inventory_investment_rows,
)


def _row(
    security_id: str,
    current_inventory: float,
    prior_inventory: float = 100.0,
    current_assets: float = 110.0,
    prior_assets: float = 90.0,
) -> dict[str, object]:
    return {
        "cash_factor_value_id": f"{security_id}-cash",
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "decision_available_at": dt.datetime(2025, 1, 31, 22),
        "current_period_end": dt.date(2024, 12, 31),
        "prior_period_end": dt.date(2024, 9, 30),
        "current_inventory": current_inventory,
        "current_inventory_id": f"{security_id}-inventory-current",
        "current_inventory_available_at": dt.datetime(2025, 1, 25, 22),
        "prior_inventory": prior_inventory,
        "prior_inventory_id": f"{security_id}-inventory-prior",
        "prior_inventory_available_at": dt.datetime(2024, 10, 25, 22),
        "current_total_assets": current_assets,
        "current_total_assets_id": f"{security_id}-assets-current",
        "current_total_assets_available_at": dt.datetime(2025, 1, 25, 22),
        "prior_total_assets": prior_assets,
    }


def test_quarterly_inventory_formulas_orientation_and_lineage() -> None:
    result = compute_quarterly_inventory_investment_rows(
        pd.DataFrame(
            [
                _row("LOW", 80.0),
                _row("MID", 100.0),
                _row("HIGH", 120.0),
            ]
        ),
        QuarterlyInventoryInvestmentOptions(
            minimum_names_per_date=2,
            winsor_limit=0.0,
        ),
    ).set_index(["factor_id", "security_id"])

    assert result.loc[(INVENTORY_CHANGE_FACTOR_ID, "LOW"), "raw_value"] == pytest.approx(-0.2)
    assert result.loc[(INVENTORY_GROWTH_FACTOR_ID, "LOW"), "raw_value"] == pytest.approx(-0.2)
    for factor_id in (INVENTORY_CHANGE_FACTOR_ID, INVENTORY_GROWTH_FACTOR_ID):
        assert result.loc[(factor_id, "LOW"), "value"] > result.loc[(factor_id, "MID"), "value"]
        assert result.loc[(factor_id, "MID"), "value"] > result.loc[(factor_id, "HIGH"), "value"]
    lineage = json.loads(
        result.loc[(INVENTORY_CHANGE_FACTOR_ID, "LOW"), "input_lineage_json"]
    )
    assert lineage["orientation"] == "lower_inventory_investment_is_preferred"
    assert lineage["assets"]["average_value"] == pytest.approx(100.0)
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_quarterly_inventory_requires_positive_pairs_and_applies_variant_guards() -> None:
    result = compute_quarterly_inventory_investment_rows(
        pd.DataFrame(
            [
                _row("GOOD", 105.0),
                _row("GOOD_2", 95.0),
                _row("NO_PRIOR_INVENTORY", 105.0, prior_inventory=0.0),
                _row("NO_ASSETS", 105.0, current_assets=0.0),
                _row(
                    "GROWTH_OUTLIER",
                    1200.0,
                    prior_inventory=100.0,
                    current_assets=1000.0,
                    prior_assets=1000.0,
                ),
            ]
        ),
        QuarterlyInventoryInvestmentOptions(
            minimum_names_per_date=1,
            winsor_limit=0.0,
        ),
    )
    pairs = set(zip(result["factor_id"], result["security_id"], strict=True))
    assert (INVENTORY_CHANGE_FACTOR_ID, "GOOD") in pairs
    assert (INVENTORY_GROWTH_FACTOR_ID, "GOOD") in pairs
    assert (INVENTORY_CHANGE_FACTOR_ID, "GROWTH_OUTLIER") in pairs
    assert (INVENTORY_GROWTH_FACTOR_ID, "GROWTH_OUTLIER") not in pairs
    assert {security_id for _, security_id in pairs}.isdisjoint(
        {"NO_PRIOR_INVENTORY", "NO_ASSETS"}
    )


def test_quarterly_inventory_definitions_are_governed(tmp_store) -> None:
    definitions = tmp_store.con.execute(
        """SELECT factor_id,direction,is_point_in_time_safe,source,
                  standardization_spec_json
           FROM factor_definition WHERE factor_id IN (?,?) ORDER BY factor_id""",
        [INVENTORY_CHANGE_FACTOR_ID, INVENTORY_GROWTH_FACTOR_ID],
    ).fetchall()
    assert len(definitions) == 2
    assert all(row[1:4] == (-1, True, SOURCE_NAME) for row in definitions)
    assert all(
        json.loads(row[4])["exclude_no_inventory_in_either_period"] is True
        for row in definitions
    )
    dependencies = tmp_store.con.execute(
        """SELECT factor_id,dependency_type,dependency_name
           FROM factor_dependency_edges WHERE factor_id IN (?,?)
           ORDER BY factor_id,dependency_type""",
        [INVENTORY_CHANGE_FACTOR_ID, INVENTORY_GROWTH_FACTOR_ID],
    ).fetchall()
    assert len(dependencies) == 4
    for factor_id in (INVENTORY_CHANGE_FACTOR_ID, INVENTORY_GROWTH_FACTOR_ID):
        assert (
            factor_id,
            "factor",
            "profitability_quarterly_cash_operating_profitability_lagged_assets",
        ) in dependencies
        assert (factor_id, "metric", "total_assets") in dependencies
