from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.earnings_revenue_agreement import (
    FACTOR_ID,
    EarningsRevenueAgreementOptions,
    compute_earnings_revenue_agreement_rows,
)


def _row(security_id: str, sue: float, revenue: float) -> dict[str, object]:
    return {
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "decision_available_at": dt.datetime(2025, 1, 31, 22),
        "sue_value": sue,
        "sue_factor_value_id": f"{security_id}-sue",
        "sue_available_at": dt.datetime(2025, 1, 31, 22),
        "sue_lineage_json": '{"signal":"sue"}',
        "revenue_surprise_value": revenue,
        "revenue_surprise_factor_value_id": f"{security_id}-revenue",
        "revenue_surprise_available_at": dt.datetime(2025, 1, 31, 22),
        "revenue_surprise_lineage_json": '{"signal":"revenue"}',
    }


def test_earnings_revenue_agreement_filters_disagreement_and_keeps_sue() -> None:
    rows = compute_earnings_revenue_agreement_rows(
        pd.DataFrame(
            [
                _row("A", 2.0, 1.0),
                _row("B", -1.0, -2.0),
                _row("C", 3.0, -1.0),
            ]
        ),
        EarningsRevenueAgreementOptions(minimum_names_per_date=2),
    ).set_index("security_id")
    assert set(rows.index) == {"A", "B"}
    assert rows.loc["A", "raw_value"] == pytest.approx(2.0)
    assert rows.loc["A", "value"] > rows.loc["B", "value"]
    lineage = json.loads(rows.loc["A", "input_lineage_json"])
    assert lineage["method"] == "same_sign_revenue_gate_then_sue"
    assert lineage["gate"] == "sue_value * revenue_surprise_value > 0"


def test_earnings_revenue_agreement_is_governed_by_migration(tmp_store) -> None:
    row = tmp_store.con.execute(
        "SELECT family,is_point_in_time_safe,source FROM factor_definition WHERE factor_id=?",
        [FACTOR_ID],
    ).fetchone()
    assert row == (
        "fundamental_earnings",
        True,
        "atx-db PIT SUE revenue agreement v1",
    )
