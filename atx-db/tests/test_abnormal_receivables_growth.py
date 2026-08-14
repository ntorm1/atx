from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.abnormal_receivables_growth import (
    FACTOR_ID,
    SOURCE_NAME,
    AbnormalReceivablesGrowthOptions,
    compute_abnormal_receivables_growth_rows,
)


def _row(
    security_id: str,
    *,
    current_receivables: float,
    current_revenue: float = 110.0,
    prior_receivables: float = 100.0,
    prior_revenue: float = 100.0,
) -> dict[str, object]:
    available = dt.datetime(2025, 2, 1, 22)
    return {
        "revenue_growth_factor_value_id": f"{security_id}-revenue-growth",
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 2, 28),
        "decision_available_at": available,
        "current_period_end": dt.date(2024, 12, 31),
        "prior_period_end": dt.date(2023, 12, 31),
        "revenue_growth": (
            current_revenue / prior_revenue - 1.0
            if prior_revenue > 0
            else float("nan")
        ),
        "current_revenue": current_revenue,
        "current_revenue_id": f"{security_id}-revenue-current",
        "current_revenue_available_at": available,
        "prior_revenue": prior_revenue,
        "prior_revenue_id": f"{security_id}-revenue-prior",
        "prior_revenue_available_at": dt.datetime(2024, 2, 1, 22),
        "current_receivables": current_receivables,
        "current_receivables_id": f"{security_id}-receivables-current",
        "current_receivables_available_at": available,
        "prior_receivables": prior_receivables,
        "prior_receivables_id": f"{security_id}-receivables-prior",
        "prior_receivables_available_at": dt.datetime(2024, 2, 1, 22),
    }


def test_abnormal_receivables_growth_formula_orientation_and_lineage() -> None:
    result = compute_abnormal_receivables_growth_rows(
        pd.DataFrame(
            [
                _row("LOW", current_receivables=100.0),
                _row("MATCH", current_receivables=110.0),
                _row("HIGH", current_receivables=130.0),
            ]
        ),
        AbnormalReceivablesGrowthOptions(minimum_names_per_date=2, winsor_limit=0.0),
    ).set_index("security_id")

    assert result.loc["LOW", "raw_value"] == pytest.approx(-0.1)
    assert result.loc["MATCH", "raw_value"] == pytest.approx(0.0)
    assert result.loc["HIGH", "raw_value"] == pytest.approx(0.2)
    assert result.loc["LOW", "value"] > result.loc["MATCH", "value"]
    assert result.loc["MATCH", "value"] > result.loc["HIGH", "value"]
    lineage = json.loads(result.loc["LOW", "input_lineage_json"])
    assert lineage["orientation"] == "lower_abnormal_receivables_growth_is_preferred"
    assert lineage["receivables"]["growth"] == pytest.approx(0.0)
    assert lineage["revenue"]["growth"] == pytest.approx(0.1)
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_abnormal_receivables_growth_requires_positive_complete_pairs() -> None:
    result = compute_abnormal_receivables_growth_rows(
        pd.DataFrame(
            [
                _row("GOOD", current_receivables=105.0),
                _row("GOOD_2", current_receivables=95.0),
                _row("NO_PRIOR_AR", current_receivables=105.0, prior_receivables=0.0),
                _row("NO_REVENUE", current_receivables=105.0, prior_revenue=0.0),
                _row("OUTLIER", current_receivables=1200.0),
            ]
        ),
        AbnormalReceivablesGrowthOptions(minimum_names_per_date=1, winsor_limit=0.0),
    )
    assert result["security_id"].tolist() == ["GOOD", "GOOD_2"]


def test_abnormal_receivables_growth_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT direction,is_point_in_time_safe,source,expression,
                  standardization_spec_json
           FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition[:3] == (-1, True, SOURCE_NAME)
    assert definition[3] == (
        "(receivables_t/receivables_t_4-1)-(revenue_t/revenue_t_4-1)"
    )
    standardization = json.loads(definition[4])
    assert standardization["missing_receivables_imputed"] is False
    assert standardization["return_fitted_parameters"] is False
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name
           FROM factor_dependency_edges WHERE factor_id=? ORDER BY 1,2""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("factor", "growth_quarterly_revenue_yoy"),
        ("metric", "ar"),
    ]
