from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.quarterly_revenue_margin_confirmation import (
    FACTOR_ID,
    SOURCE_NAME,
    QuarterlyRevenueMarginConfirmationOptions,
    compute_quarterly_revenue_margin_confirmation_rows,
)


def _row(security_id: str, score: float, current_margin: float, prior_margin: float) -> dict[str, object]:
    return {
        "revenue_growth_factor_value_id": f"{security_id}-growth",
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "decision_available_at": dt.datetime(2025, 1, 31, 22),
        "revenue_growth_raw_value": score,
        "revenue_growth_value": score,
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


def test_margin_confirmation_gate_orientation_and_lineage() -> None:
    result = compute_quarterly_revenue_margin_confirmation_rows(
        pd.DataFrame(
            [
                _row("LOW", -1.0, 0.42, 0.40),
                _row("MID", 0.0, 0.40, 0.40),
                _row("HIGH", 1.0, 0.45, 0.40),
                _row("DECLINING", 2.0, 0.35, 0.40),
            ]
        ),
        QuarterlyRevenueMarginConfirmationOptions(minimum_names_per_date=2),
    ).set_index("security_id")
    assert set(result.index) == {"LOW", "MID", "HIGH"}
    assert result.loc["HIGH", "value"] > result.loc["MID", "value"]
    assert result.loc["MID", "value"] > result.loc["LOW", "value"]
    lineage = json.loads(result.loc["HIGH", "input_lineage_json"])
    assert lineage["gate"] == "current_gross_margin-prior_year_gross_margin >= 0"
    assert lineage["gross_margin_change"] == pytest.approx(0.05)
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_margin_confirmation_rejects_implausible_margins() -> None:
    result = compute_quarterly_revenue_margin_confirmation_rows(
        pd.DataFrame(
            [
                _row("GOOD", 1.0, 0.5, 0.4),
                _row("GOOD_2", 2.0, 0.6, 0.4),
                _row("EXTREME", 3.0, 6.0, 0.4),
            ]
        ),
        QuarterlyRevenueMarginConfirmationOptions(minimum_names_per_date=1),
    )
    assert set(result["security_id"]) == {"GOOD", "GOOD_2"}


def test_margin_confirmation_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,direction,is_point_in_time_safe,source,
                  standardization_spec_json FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition is not None
    assert definition[0] == "zscore(revenue_growth_score | gross_margin_t-gross_margin_t_4>=0)"
    assert definition[1:4] == (1, True, SOURCE_NAME)
    assert json.loads(definition[4])["maximum_absolute_gross_margin"] == 5.0
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name FROM factor_dependency_edges
           WHERE factor_id=? ORDER BY dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("factor", "growth_quarterly_revenue_yoy"),
        ("factor", "profitability_quarterly_operating_profitability_lagged_assets"),
    ]
