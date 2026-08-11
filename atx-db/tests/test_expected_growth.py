from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.expected_growth import (
    FACTOR_ID,
    PUBLISHED_SLOPES,
    SOURCE_NAME,
    ExpectedGrowthOptions,
    compute_expected_growth_rows,
)


def _input_row(
    security_id: str,
    *,
    market_equity: float,
    current_assets: float,
    cash_profitability: float,
    delta_roe: float,
    delta_missing: bool = False,
) -> dict[str, object]:
    return {
        "asset_growth_factor_value_id": f"{security_id}-growth",
        "cash_profitability_factor_value_id": f"{security_id}-cash",
        "delta_roe_factor_value_id": None if delta_missing else f"{security_id}-droe",
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 6, 30),
        "decision_available_at": dt.datetime(2025, 6, 30, 22),
        "accession_number": f"{security_id}-10k",
        "annual_period_end": dt.date(2024, 12, 31),
        "current_assets": current_assets,
        "prior_assets": 100.0,
        "current_asset_id": f"{security_id}-assets-current",
        "prior_asset_id": f"{security_id}-assets-prior",
        "market_equity": market_equity,
        "long_term_debt": 0.0,
        "short_term_debt": 0.0,
        "tobins_q": market_equity / current_assets,
        "cash_operating_profitability": cash_profitability,
        "delta_roe": delta_roe,
        "delta_roe_imputed_zero": delta_missing,
    }


def test_expected_growth_uses_published_slopes_and_lineage() -> None:
    inputs = pd.DataFrame(
        [
            _input_row(
                "HIGH", market_equity=100.0, current_assets=100.0,
                cash_profitability=0.30, delta_roe=0.20,
            ),
            _input_row(
                "MID", market_equity=100.0, current_assets=100.0,
                cash_profitability=0.10, delta_roe=0.00, delta_missing=True,
            ),
            _input_row(
                "LOW", market_equity=200.0, current_assets=100.0,
                cash_profitability=-0.10, delta_roe=-0.20,
            ),
        ]
    )
    rows = compute_expected_growth_rows(
        inputs,
        ExpectedGrowthOptions(minimum_names_per_date=2, winsor_limit=0.0),
    ).set_index("security_id")

    assert rows.loc["HIGH", "raw_value"] > rows.loc["MID", "raw_value"]
    assert rows.loc["MID", "raw_value"] > rows.loc["LOW", "raw_value"]
    expected_mid = PUBLISHED_SLOPES["cash_operating_profitability"] * 0.10
    assert rows.loc["MID", "raw_value"] == pytest.approx(expected_mid)
    lineage = json.loads(rows.loc["MID", "input_lineage_json"])
    assert lineage["predictors"]["delta_roe_imputed_zero"] is True
    assert lineage["research_contract"]["return_fitted_parameters"] is False
    assert lineage["formula"] == "-0.029*ln(q)+0.516*Cop+0.771*dROE"


def test_expected_growth_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,is_point_in_time_safe,source,standardization_spec_json
           FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition is not None
    assert definition[1:3] == (True, SOURCE_NAME)
    assert definition[0].startswith("-0.029*ln(tobins_q)")
    standardization = json.loads(definition[3])
    assert standardization["missing_delta_roe"] == "zero"
    assert standardization["return_fitted_parameters"] is False
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name
           FROM factor_dependency_edges WHERE factor_id=?
           ORDER BY dependency_type,dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("factor", "investment_conservative_asset_growth"),
        ("factor", "profitability_cash_operating_profitability"),
        ("factor", "profitability_q_factor_delta_roe"),
        ("source", "sec_company_facts"),
    ]
