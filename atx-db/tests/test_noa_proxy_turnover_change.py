from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.noa_proxy_turnover_change import (
    FACTOR_ID,
    SOURCE_NAME,
    NoaProxyTurnoverChangeOptions,
    compute_noa_proxy_turnover_change_rows,
)


def _row(
    security_id: str,
    current_revenue: float,
    prior_revenue: float,
    prior_noa_proxy: float,
    prior2_noa_proxy: float,
) -> dict[str, object]:
    row: dict[str, object] = {
        "security_id": security_id,
        "symbol": security_id,
        "trade_date": dt.date(2025, 3, 31),
        "decision_available_at": dt.datetime(2025, 3, 31, 22),
        "current_revenue": current_revenue,
        "prior_revenue": prior_revenue,
        "prior_noa_proxy": prior_noa_proxy,
        "prior2_noa_proxy": prior2_noa_proxy,
        "current_revenue_id": f"{security_id}-revenue-current",
        "prior_revenue_id": f"{security_id}-revenue-prior",
        "current_accession_number": f"{security_id}-current",
        "prior_accession_number": f"{security_id}-prior",
        "prior2_accession_number": f"{security_id}-prior2",
        "current_period_end": dt.date(2024, 12, 31),
        "prior_period_end": dt.date(2023, 12, 31),
        "prior2_period_end": dt.date(2022, 12, 31),
        "current_revenue_available_at": dt.datetime(2025, 2, 20, 22),
        "prior_revenue_available_at": dt.datetime(2024, 2, 20, 22),
        "universe_id": "us_equity_research_v1",
        "universe_valid_from": dt.date(2025, 1, 1),
        "universe_valid_to": None,
        "universe_available_at": dt.datetime(2025, 1, 1),
        "universe_source": "test",
    }
    for prefix, noa in (("prior", prior_noa_proxy), ("prior2", prior2_noa_proxy)):
        values = {"equity": noa + 20.0, "lt_debt": 10.0, "cash_st_inv": 30.0}
        for component, value in values.items():
            row[f"{prefix}_{component}"] = value
            row[f"{prefix}_{component}_id"] = f"{security_id}-{prefix}-{component}"
            row[f"{prefix}_{component}_available_at"] = dt.datetime(2024, 2, 20, 22)
    return row


def test_noa_proxy_turnover_orientation_availability_and_lineage() -> None:
    result = compute_noa_proxy_turnover_change_rows(
        pd.DataFrame(
            [
                _row("LOW", 100.0, 100.0, 100.0, 80.0),
                _row("MID", 120.0, 100.0, 100.0, 100.0),
                _row("HIGH", 150.0, 100.0, 100.0, 100.0),
            ]
        ),
        NoaProxyTurnoverChangeOptions(minimum_names_per_date=2),
    ).set_index("security_id")

    assert result.loc["HIGH", "value"] > result.loc["MID", "value"]
    assert result.loc["MID", "value"] > result.loc["LOW", "value"]
    assert result.loc["HIGH", "available_at"] == pd.Timestamp("2025-03-31 22:00:00")
    lineage = json.loads(result.loc["HIGH", "input_lineage_json"])
    assert lineage["change"] == pytest.approx(0.5)
    assert lineage["research_contract"]["missing_components_imputed"] is False
    assert lineage["research_contract"]["return_fitted_parameters"] is False
    assert "Not exact net operating assets" in lineage["proxy_warning"]


def test_noa_proxy_turnover_rejects_invalid_denominators_and_nonfinite_values() -> None:
    result = compute_noa_proxy_turnover_change_rows(
        pd.DataFrame(
            [
                _row("GOOD", 150.0, 100.0, 100.0, 100.0),
                _row("GOOD_2", 120.0, 100.0, 100.0, 100.0),
                _row("ZERO", 150.0, 100.0, 0.0, 100.0),
                _row("NONFINITE", float("inf"), 100.0, 100.0, 100.0),
            ]
        ),
        NoaProxyTurnoverChangeOptions(minimum_names_per_date=1),
    )
    assert set(result["security_id"]) == {"GOOD", "GOOD_2"}


def test_noa_proxy_turnover_enforces_minimum_monthly_breadth() -> None:
    result = compute_noa_proxy_turnover_change_rows(
        pd.DataFrame([_row("ONLY", 150.0, 100.0, 100.0, 100.0)]),
        NoaProxyTurnoverChangeOptions(minimum_names_per_date=2),
    )
    assert result.empty


def test_noa_proxy_turnover_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,direction,is_point_in_time_safe,source,
                  standardization_spec_json FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition is not None
    assert definition[0].startswith("zscore(winsorize_1pct(revenue_t/")
    assert definition[1:4] == (1, True, SOURCE_NAME)
    spec = json.loads(definition[4])
    assert spec["is_exact_noa"] is False
    assert spec["missing_components_imputed"] is False
    assert spec["return_fitted_parameters"] is False
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name FROM factor_dependency_edges
           WHERE factor_id=? ORDER BY dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("metric", "cash_st_inv"),
        ("metric", "lt_debt"),
        ("metric", "revenue"),
        ("metric", "stockholders_equity"),
    ]
