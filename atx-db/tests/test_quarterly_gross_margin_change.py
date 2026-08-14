from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.quarterly_gross_margin_change import (
    FACTOR_ID,
    SOURCE_NAME,
    QuarterlyGrossMarginChangeOptions,
    compute_quarterly_gross_margin_change_rows,
)


def _row(security_id: str, current_margin: float, prior_margin: float = 0.4) -> dict[str, object]:
    return {
        "revenue_growth_factor_value_id": f"{security_id}-growth",
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "decision_available_at": dt.datetime(2025, 1, 31, 22),
        "current_qop_factor_value_id": f"{security_id}-qop-current",
        "current_qop_available_at": dt.datetime(2025, 1, 30, 22),
        "current_period_end": dt.date(2024, 12, 31),
        "current_revenue": 100.0,
        "current_gross_profit": 100.0 * current_margin,
        "current_gross_margin": current_margin,
        "prior_qop_factor_value_id": f"{security_id}-qop-prior",
        "prior_qop_available_at": dt.datetime(2024, 1, 30, 22),
        "prior_period_end": dt.date(2023, 12, 31),
        "prior_revenue": 100.0,
        "prior_gross_profit": 100.0 * prior_margin,
        "prior_gross_margin": prior_margin,
    }


def test_quarterly_gross_margin_change_formula_orientation_and_lineage() -> None:
    result = compute_quarterly_gross_margin_change_rows(
        pd.DataFrame([_row("DOWN", 0.3), _row("FLAT", 0.4), _row("UP", 0.5)]),
        QuarterlyGrossMarginChangeOptions(minimum_names_per_date=2, winsor_limit=0.0),
    ).set_index("security_id")

    assert result.loc["DOWN", "raw_value"] == pytest.approx(-0.1)
    assert result.loc["FLAT", "raw_value"] == pytest.approx(0.0)
    assert result.loc["UP", "raw_value"] == pytest.approx(0.1)
    assert result.loc["UP", "value"] > result.loc["FLAT", "value"]
    assert result.loc["FLAT", "value"] > result.loc["DOWN", "value"]
    lineage = json.loads(result.loc["UP", "input_lineage_json"])
    assert lineage["formula"] == "gross_margin_t-gross_margin_t_4"
    assert lineage["research_contract"]["missing_components_imputed"] is False
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_quarterly_gross_margin_change_rejects_implausible_values() -> None:
    result = compute_quarterly_gross_margin_change_rows(
        pd.DataFrame([_row("GOOD", 0.5), _row("GOOD_2", 0.6), _row("EXTREME", 6.0)]),
        QuarterlyGrossMarginChangeOptions(minimum_names_per_date=1, winsor_limit=0.0),
    )
    assert result["security_id"].tolist() == ["GOOD", "GOOD_2"]


def test_quarterly_gross_margin_change_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,direction,is_point_in_time_safe,source,
                  standardization_spec_json FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition[:4] == (
        "gross_margin_t-gross_margin_t_4",
        1,
        True,
        SOURCE_NAME,
    )
    standardization = json.loads(definition[4])
    assert standardization["missing_components_imputed"] is False
    assert standardization["return_fitted_parameters"] is False
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name FROM factor_dependency_edges
           WHERE factor_id=? ORDER BY dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("factor", "growth_quarterly_revenue_yoy"),
        ("factor", "profitability_quarterly_operating_profitability_lagged_assets"),
    ]
