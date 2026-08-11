from __future__ import annotations

import datetime as dt
import json

import pandas as pd

from atx_db.quarterly_operating_profitability import (
    FACTOR_ID,
    SOURCE_NAME,
    QuarterlyOperatingProfitabilityOptions,
    compute_quarterly_operating_profitability_rows,
)


def _input_row(
    security_id: str,
    *,
    revenue: float | None = 100.0,
    cogs: float | None = 60.0,
    gross_profit: float | None = None,
    sga: float = 20.0,
    rd_expense: float | None = 5.0,
    total_assets: float = 100.0,
) -> dict[str, object]:
    resolved_gross_profit = (
        revenue - cogs
        if revenue is not None and cogs is not None
        else gross_profit
    )
    return {
        "security_id": security_id,
        "symbol": security_id,
        "trade_date": dt.date(2025, 1, 31),
        "decision_available_at": dt.datetime(2025, 1, 31, 22),
        "accession_number": f"{security_id}-current",
        "period_start": dt.date(2024, 10, 1),
        "period_end": dt.date(2024, 12, 31),
        "reporting_available_at": dt.datetime(2025, 1, 30, 22),
        "revenue": revenue,
        "revenue_id": f"{security_id}-revenue" if revenue is not None else None,
        "cogs": cogs,
        "cogs_id": f"{security_id}-cogs" if cogs is not None else None,
        "gross_profit": gross_profit,
        "gross_profit_id": (
            f"{security_id}-gross-profit" if gross_profit is not None else None
        ),
        "gross_profit_method": (
            "revenue_minus_cogs"
            if revenue is not None and cogs is not None
            else "reported_gross_profit"
        ),
        "resolved_gross_profit": resolved_gross_profit,
        "sga": sga,
        "sga_id": f"{security_id}-sga",
        "rd_expense": rd_expense,
        "rd_expense_id": f"{security_id}-rd" if rd_expense is not None else None,
        "total_assets": total_assets,
        "total_assets_id": f"{security_id}-assets",
        "total_assets_available_at": dt.datetime(2024, 11, 1, 22),
        "assets_accession_number": f"{security_id}-prior",
        "assets_period_end": dt.date(2024, 9, 30),
        "universe_id": "us_common_equity_liquid_v1",
        "universe_valid_from": dt.date(2025, 1, 1),
        "universe_valid_to": None,
        "universe_available_at": dt.datetime(2025, 1, 1),
        "universe_source": "test",
    }


def test_quarterly_operating_profitability_formula_orientation_and_lineage() -> None:
    inputs = pd.DataFrame(
        [
            _input_row("STRONG"),
            _input_row(
                "FALLBACK",
                revenue=None,
                cogs=None,
                gross_profit=30.0,
                sga=15.0,
                rd_expense=None,
            ),
            _input_row("WEAK", revenue=80.0, cogs=70.0, sga=25.0, rd_expense=0.0),
        ]
    )
    rows = compute_quarterly_operating_profitability_rows(
        inputs,
        QuarterlyOperatingProfitabilityOptions(
            minimum_names_per_date=2,
            winsor_limit=0.0,
        ),
    ).set_index("security_id")

    assert rows.loc["STRONG", "raw_value"] == 0.25
    assert rows.loc["FALLBACK", "raw_value"] == 0.15
    assert rows.loc["WEAK", "raw_value"] == -0.15
    assert rows.loc["STRONG", "value"] > rows.loc["FALLBACK", "value"]
    assert rows.loc["FALLBACK", "value"] > rows.loc["WEAK", "value"]
    lineage = json.loads(rows.loc["FALLBACK", "input_lineage_json"])
    assert lineage["quarterly_statement"]["gross_profit_method"] == (
        "reported_gross_profit"
    )
    assert lineage["quarterly_statement"]["rd_expense"][
        "missing_replaced_with_zero"
    ]
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_quarterly_operating_profitability_rejects_nonpositive_assets() -> None:
    rows = compute_quarterly_operating_profitability_rows(
        pd.DataFrame(
            [
                _input_row("GOOD"),
                _input_row("ALSO_GOOD", revenue=90.0),
                _input_row("ZERO", total_assets=0.0),
                _input_row("NEGATIVE", total_assets=-10.0),
                _input_row("SCALE_ERROR", revenue=1000.0, cogs=0.0, total_assets=10.0),
            ]
        ),
        QuarterlyOperatingProfitabilityOptions(
            minimum_names_per_date=1,
            winsor_limit=0.0,
        ),
    )
    assert set(rows["security_id"]) == {"GOOD", "ALSO_GOOD"}


def test_quarterly_operating_profitability_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,is_point_in_time_safe,source,standardization_spec_json
           FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition is not None
    assert definition[1:3] == (True, SOURCE_NAME)
    assert definition[0] == (
        "(coalesce(revenue-cogs,gross_profit)-sga+coalesce(rd_expense,0))"
        "/one_quarter_lagged_total_assets"
    )
    standardization = json.loads(definition[3])
    assert standardization["gross_profit_fallback"] == "reported_gross_profit"
    assert standardization["maximum_absolute_raw_value"] == 5.0
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name
           FROM factor_dependency_edges WHERE factor_id=? ORDER BY dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("metric", "cogs"),
        ("metric", "gross_profit"),
        ("metric", "rd_expense"),
        ("metric", "revenue"),
        ("metric", "sga"),
        ("metric", "total_assets"),
    ]
