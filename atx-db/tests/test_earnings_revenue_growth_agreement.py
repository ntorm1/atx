from __future__ import annotations

import datetime as dt
import json

import pandas as pd

from atx_db.earnings_revenue_growth_agreement import (
    FACTOR_ID,
    SOURCE_NAME,
    EarningsRevenueGrowthAgreementOptions,
    compute_earnings_revenue_growth_agreement_rows,
)


def _row(security_id: str, sue: float, revenue_growth: float) -> dict[str, object]:
    return {
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "decision_available_at": dt.datetime(2025, 1, 31, 22),
        "sue_value": sue,
        "sue_factor_value_id": f"{security_id}-sue",
        "sue_available_at": dt.datetime(2025, 1, 30, 22),
        "revenue_growth_value": revenue_growth,
        "revenue_growth_factor_value_id": f"{security_id}-revenue",
        "revenue_growth_available_at": dt.datetime(2025, 1, 29, 22),
    }


def test_revenue_growth_agreement_keeps_same_sign_sue_and_lineage() -> None:
    result = compute_earnings_revenue_growth_agreement_rows(
        pd.DataFrame(
            [
                _row("POS_LOW", 1.0, 0.1),
                _row("POS_HIGH", 2.0, 0.2),
                _row("NEG", -1.0, -0.1),
                _row("DISAGREE", 3.0, -0.2),
            ]
        ),
        EarningsRevenueGrowthAgreementOptions(minimum_names_per_date=2),
    ).set_index("security_id")

    assert set(result.index) == {"POS_LOW", "POS_HIGH", "NEG"}
    assert result.loc["POS_HIGH", "raw_value"] == 2.0
    assert result.loc["POS_HIGH", "value"] > result.loc["POS_LOW", "value"]
    assert result.loc["POS_LOW", "value"] > result.loc["NEG", "value"]
    lineage = json.loads(result.loc["POS_HIGH", "input_lineage_json"])
    assert lineage["gate"] == "sue_value * revenue_growth_value > 0"
    assert lineage["return_fitted_parameters"] is False


def test_revenue_growth_agreement_rejects_missing_and_nonfinite_inputs() -> None:
    frame = pd.DataFrame(
        [
            _row("GOOD", 1.0, 0.1),
            _row("GOOD_2", 2.0, 0.2),
            _row("MISSING", 1.0, float("nan")),
            _row("INFINITE", float("inf"), 0.1),
        ]
    )
    result = compute_earnings_revenue_growth_agreement_rows(
        frame,
        EarningsRevenueGrowthAgreementOptions(minimum_names_per_date=1),
    )
    assert set(result["security_id"]) == {"GOOD", "GOOD_2"}


def test_revenue_growth_agreement_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,direction,is_point_in_time_safe,source,
                  standardization_spec_json
           FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition is not None
    assert definition[0] == "zscore(sue | sue*quarterly_revenue_growth>0)"
    assert definition[1:4] == (1, True, SOURCE_NAME)
    assert json.loads(definition[4])["return_fitted_parameters"] is False
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name
           FROM factor_dependency_edges WHERE factor_id=? ORDER BY dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("factor", "earnings_standardized_unexpected_eps"),
        ("factor", "growth_quarterly_revenue_yoy"),
    ]
