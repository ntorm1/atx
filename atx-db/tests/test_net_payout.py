from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.net_payout import (
    FACTOR_ID,
    NetPayoutOptions,
    compute_net_payout_rows,
)


def _input(
    security_id: str,
    *,
    dividends: float,
    repurchases: float,
    issuance: float,
    market_cap: float,
) -> dict[str, object]:
    return {
        "security_id": security_id,
        "symbol": security_id,
        "trade_date": dt.date(2025, 1, 31),
        "price_available_at": dt.datetime(2025, 1, 31, 22),
        "net_payout_yield": (-dividends - repurchases - issuance) / market_cap,
        "accession_number": f"{security_id}-10K",
        "ttm_end_date": dt.date(2024, 12, 31),
        "common_div_paid_id": f"{security_id}-div",
        "common_div_paid": dividends,
        "common_div_paid_available_at": dt.datetime(2025, 1, 30),
        "share_repurchases_id": f"{security_id}-repurchase",
        "share_repurchases": repurchases,
        "share_repurchases_available_at": dt.datetime(2025, 1, 30),
        "stock_issuance_id": f"{security_id}-issuance",
        "stock_issuance": issuance,
        "stock_issuance_available_at": dt.datetime(2025, 1, 30),
        "share_history_id": f"{security_id}-shares",
        "share_effective_date": dt.date(2024, 12, 31),
        "share_available_at": dt.datetime(2025, 1, 30),
        "share_split_index": 1.0,
        "split_index": 1.0,
        "decision_share_count": 100.0,
        "close": market_cap / 100.0,
        "market_cap_usd": market_cap,
    }


def test_net_payout_formula_orientation_and_lineage() -> None:
    inputs = [
        _input("A", dividends=-10.0, repurchases=-20.0, issuance=5.0, market_cap=100.0),
        _input("B", dividends=-5.0, repurchases=-5.0, issuance=20.0, market_cap=100.0),
    ]
    rows = compute_net_payout_rows(
        pd.DataFrame(inputs),
        NetPayoutOptions(minimum_names_per_date=2, winsor_limit=0.0, run_id="test"),
    ).set_index("security_id")

    assert rows.loc["A", "raw_value"] == pytest.approx(0.25)
    assert rows.loc["B", "raw_value"] == pytest.approx(-0.10)
    assert rows.loc["A", "value"] > rows.loc["B", "value"]
    lineage = json.loads(rows.loc["A", "input_lineage_json"])
    assert lineage["missing_component_policy"] == "require_same_filing_complete_case"
    assert lineage["payout_statement"]["stock_issuance"]["value"] == 5.0


def test_net_payout_factor_is_governed_by_migration(tmp_store) -> None:
    row = tmp_store.con.execute(
        """
        SELECT family, direction, is_point_in_time_safe, source
        FROM factor_definition WHERE factor_id = ?
        """,
        [FACTOR_ID],
    ).fetchone()
    assert row == (
        "fundamental_financing",
        1,
        True,
        "atx-db PIT cash-flow net payout yield v1",
    )
