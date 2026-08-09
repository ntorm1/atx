from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.cash_flow_profitability import (
    FACTOR_IDS,
    CashFlowProfitabilityOptions,
    compute_cash_flow_profitability_rows,
)


def _row(security_id: str, cash_profit: float, low_accruals: float) -> dict[str, object]:
    return {
        "security_id": security_id,
        "symbol": security_id,
        "trade_date": dt.date(2025, 1, 31),
        "price_available_at": dt.datetime(2025, 1, 31, 22),
        "cash_flow_profitability": cash_profit,
        "low_total_accruals": low_accruals,
        "universe_id": "us_common_equity_liquid_v1",
        "universe_valid_from": dt.date(2025, 1, 1),
        "universe_valid_to": None,
        "universe_available_at": dt.datetime(2025, 1, 1, 22),
        "universe_source": "fixture",
        "ttm_end_date": dt.date(2024, 9, 30),
        "net_income_ttm_id": f"{security_id}-ni",
        "net_income_ttm": 80.0,
        "net_income_available_at": dt.datetime(2024, 11, 1),
        "operating_cash_flow_ttm_id": f"{security_id}-ocf",
        "operating_cash_flow_ttm": 100.0,
        "operating_cash_flow_available_at": dt.datetime(2024, 11, 1),
        "asset_id": f"{security_id}-assets",
        "asset_period_end": dt.date(2024, 9, 30),
        "assets": 500.0,
        "asset_available_at": dt.datetime(2024, 11, 1),
        "prior_asset_id": f"{security_id}-assets-prior",
        "prior_asset_period_end": dt.date(2023, 9, 30),
        "prior_assets": 400.0,
        "prior_asset_available_at": dt.datetime(2023, 11, 1),
        "average_total_assets": 450.0,
    }


def test_cash_flow_profitability_orients_both_outputs_and_records_lineage() -> None:
    rows = compute_cash_flow_profitability_rows(
        pd.DataFrame([_row("A", 0.3, 0.1), _row("B", -0.1, -0.2)]),
        CashFlowProfitabilityOptions(minimum_names_per_date=2, winsor_limit=0.0),
    )
    assert set(rows["factor_id"]) == set(FACTOR_IDS)
    lookup = rows.set_index(["factor_id", "security_id"])
    for factor_id in FACTOR_IDS:
        assert lookup.loc[(factor_id, "A"), "value"] > lookup.loc[(factor_id, "B"), "value"]
    assert lookup.loc[(FACTOR_IDS[0], "A"), "raw_value"] == pytest.approx(0.3)
    lineage = json.loads(lookup.loc[(FACTOR_IDS[1], "A"), "input_lineage_json"])
    assert lineage["ttm"]["net_income_ttm_id"] == "A-ni"
    assert lineage["assets"]["average_total_assets"] == pytest.approx(450.0)


def test_cash_flow_profitability_factors_are_governed_by_migration(tmp_store) -> None:
    rows = tmp_store.con.execute(
        """SELECT factor_id,is_point_in_time_safe,source
           FROM factor_definition WHERE factor_id IN (?,?) ORDER BY factor_id""",
        list(FACTOR_IDS),
    ).fetchall()
    assert rows == [(factor_id, True, "atx-db PIT cash-flow profitability v1") for factor_id in sorted(FACTOR_IDS)]
