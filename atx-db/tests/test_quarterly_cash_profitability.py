from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.quarterly_cash_profitability import (
    FACTOR_ID,
    SOURCE_NAME,
    QuarterlyCashProfitabilityOptions,
    compute_quarterly_cash_profitability_rows,
)


def _input_row(
    security_id: str,
    *,
    operating_profit: float = 25.0,
    assets: float = 100.0,
    current_ar: float | None = 20.0,
    prior_ar: float | None = 10.0,
    current_inventory: float | None = 15.0,
    prior_inventory: float | None = 10.0,
    current_deferred_revenue: float | None = 8.0,
    prior_deferred_revenue: float | None = 3.0,
    current_ap: float | None = 12.0,
    prior_ap: float | None = 7.0,
) -> dict[str, object]:
    row: dict[str, object] = {
        "qop_factor_value_id": f"{security_id}-qop",
        "qop_source": "test-qop",
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "decision_available_at": dt.datetime(2025, 1, 31, 22),
        "quarterly_operating_profitability": (
            operating_profit / assets if assets != 0 else float("nan")
        ),
        "qop_standardized_value": 0.0,
        "quarterly_operating_profit": operating_profit,
        "lagged_total_assets": assets,
        "current_period_end": dt.date(2024, 12, 31),
        "prior_period_end": dt.date(2024, 9, 30),
        "current_accession_number": f"{security_id}-current",
        "prior_accession_number": f"{security_id}-prior",
    }
    for metric, current, prior in (
        ("ar", current_ar, prior_ar),
        ("inventory", current_inventory, prior_inventory),
        ("deferred_revenue", current_deferred_revenue, prior_deferred_revenue),
        ("ap", current_ap, prior_ap),
    ):
        row[f"current_{metric}"] = current
        row[f"prior_{metric}"] = prior
        row[f"current_{metric}_id"] = (
            f"{security_id}-current-{metric}" if current is not None else None
        )
        row[f"prior_{metric}_id"] = (
            f"{security_id}-prior-{metric}" if prior is not None else None
        )
        row[f"current_{metric}_available_at"] = (
            dt.datetime(2025, 1, 30) if current is not None else None
        )
        row[f"prior_{metric}_available_at"] = (
            dt.datetime(2024, 10, 30) if prior is not None else None
        )
    return row


def test_quarterly_cash_profitability_formula_and_missing_change_rule() -> None:
    rows = compute_quarterly_cash_profitability_rows(
        pd.DataFrame(
            [
                _input_row("CASH"),
                _input_row(
                    "MISSING",
                    operating_profit=15.0,
                    current_ar=None,
                    prior_ar=None,
                    current_inventory=None,
                    prior_inventory=None,
                    current_deferred_revenue=None,
                    prior_deferred_revenue=None,
                    current_ap=None,
                    prior_ap=None,
                ),
                _input_row("WEAK", operating_profit=-10.0),
            ]
        ),
        QuarterlyCashProfitabilityOptions(
            minimum_names_per_date=2,
            winsor_limit=0.0,
        ),
    ).set_index("security_id")

    assert rows.loc["CASH", "raw_value"] == pytest.approx(0.20)
    assert rows.loc["MISSING", "raw_value"] == pytest.approx(0.15)
    assert rows.loc["WEAK", "raw_value"] == pytest.approx(-0.15)
    assert rows.loc["CASH", "value"] > rows.loc["MISSING", "value"]
    lineage = json.loads(rows.loc["MISSING", "input_lineage_json"])
    assert lineage["working_capital"]["ar"][
        "missing_change_replaced_with_zero"
    ]
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_quarterly_cash_profitability_rejects_scale_errors() -> None:
    rows = compute_quarterly_cash_profitability_rows(
        pd.DataFrame(
            [
                _input_row("GOOD"),
                _input_row("ALSO_GOOD", operating_profit=20.0),
                _input_row("ZERO_ASSETS", assets=0.0),
                _input_row("SCALE_ERROR", operating_profit=1000.0, assets=10.0),
            ]
        ),
        QuarterlyCashProfitabilityOptions(
            minimum_names_per_date=1,
            winsor_limit=0.0,
        ),
    )
    assert set(rows["security_id"]) == {"GOOD", "ALSO_GOOD"}


def test_quarterly_cash_profitability_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,is_point_in_time_safe,source,standardization_spec_json
           FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition is not None
    assert definition[1:3] == (True, SOURCE_NAME)
    assert definition[0] == (
        "(quarterly_operating_profit-dAR-dInventory+dDeferredRevenue+dAP)"
        "/one_quarter_lagged_total_assets"
    )
    standardization = json.loads(definition[3])
    assert standardization["missing_balance_changes"] == "zero"
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name
           FROM factor_dependency_edges WHERE factor_id=? ORDER BY dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("metric", "ap"),
        ("metric", "ar"),
        ("metric", "deferred_revenue"),
        ("metric", "inventory"),
        ("factor", "profitability_quarterly_operating_profitability_lagged_assets"),
    ]
