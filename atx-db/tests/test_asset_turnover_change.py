from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.asset_turnover_change import (
    AssetTurnoverChangeOptions,
    compute_asset_turnover_change_rows,
)


def _row(
    security_id: str,
    current_revenue: float,
    prior_revenue: float,
    prior_assets: float,
    prior2_assets: float,
) -> dict[str, object]:
    current_period_end = dt.date(2024, 12, 31)
    prior_period_end = dt.date(2023, 12, 31)
    prior2_period_end = dt.date(2022, 12, 31)
    return {
        "security_id": security_id,
        "symbol": security_id,
        "trade_date": dt.date(2025, 3, 31),
        "decision_available_at": dt.datetime(2025, 3, 31, 22),
        "current_revenue": current_revenue,
        "prior_revenue": prior_revenue,
        "current_assets": prior_assets * 1.1,
        "prior_assets": prior_assets,
        "prior2_assets": prior2_assets,
        "current_revenue_id": f"{security_id}-revenue-current",
        "prior_revenue_id": f"{security_id}-revenue-prior",
        "current_assets_id": f"{security_id}-assets-current",
        "prior_assets_id": f"{security_id}-assets-prior",
        "prior2_assets_id": f"{security_id}-assets-prior2",
        "current_accession_number": f"{security_id}-current",
        "prior_accession_number": f"{security_id}-prior",
        "prior2_accession_number": f"{security_id}-prior2",
        "current_period_end": current_period_end,
        "prior_period_end": prior_period_end,
        "prior2_period_end": prior2_period_end,
        "current_revenue_available_at": dt.datetime(2025, 2, 20, 22),
        "prior_revenue_available_at": dt.datetime(2024, 2, 20, 22),
        "current_assets_available_at": dt.datetime(2025, 2, 20, 22),
        "prior_assets_available_at": dt.datetime(2024, 2, 20, 22),
        "prior2_assets_available_at": dt.datetime(2023, 2, 20, 22),
        "universe_id": "us_equity_research_v1",
        "universe_valid_from": dt.date(2025, 1, 1),
        "universe_valid_to": None,
        "universe_available_at": dt.datetime(2025, 1, 1),
        "universe_source": "test",
    }


def test_asset_turnover_change_orientation_availability_and_lineage() -> None:
    result = compute_asset_turnover_change_rows(
        pd.DataFrame(
            [
                _row("LOW", 100.0, 100.0, 100.0, 80.0),
                _row("MID", 120.0, 100.0, 100.0, 100.0),
                _row("HIGH", 150.0, 100.0, 100.0, 100.0),
            ]
        ),
        AssetTurnoverChangeOptions(minimum_names_per_date=2),
    ).set_index("security_id")

    assert result.loc["HIGH", "value"] > result.loc["MID", "value"]
    assert result.loc["MID", "value"] > result.loc["LOW", "value"]
    assert result.loc["HIGH", "available_at"] == pd.Timestamp("2025-03-31 22:00:00")
    lineage = json.loads(result.loc["HIGH", "input_lineage_json"])
    assert lineage["formula"] == "revenue_t/assets_t_1-revenue_t_1/assets_t_2"
    assert lineage["annual"]["change"] == pytest.approx(0.5)
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_asset_turnover_change_rejects_invalid_denominators_and_nonfinite_values() -> None:
    result = compute_asset_turnover_change_rows(
        pd.DataFrame(
            [
                _row("GOOD", 150.0, 100.0, 100.0, 100.0),
                _row("GOOD_2", 120.0, 100.0, 100.0, 100.0),
                _row("ZERO", 150.0, 100.0, 0.0, 100.0),
                _row("NONFINITE", float("inf"), 100.0, 100.0, 100.0),
            ]
        ),
        AssetTurnoverChangeOptions(minimum_names_per_date=1),
    )
    assert set(result["security_id"]) == {"GOOD", "GOOD_2"}


def test_asset_turnover_change_enforces_minimum_monthly_breadth() -> None:
    result = compute_asset_turnover_change_rows(
        pd.DataFrame([_row("ONLY", 150.0, 100.0, 100.0, 100.0)]),
        AssetTurnoverChangeOptions(minimum_names_per_date=2),
    )
    assert result.empty
