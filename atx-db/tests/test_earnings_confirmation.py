from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.earnings_confirmation import (
    FACTOR_ID,
    EarningsConfirmationOptions,
    compute_earnings_confirmation_rows,
)


def _row(security_id: str, sue: float, reaction: float) -> dict[str, object]:
    return {
        "security_id": security_id, "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "decision_available_at": dt.datetime(2025, 1, 31, 22),
        "sue_value": sue, "reaction_value": reaction,
        "sue_factor_value_id": f"{security_id}-sue",
        "reaction_factor_value_id": f"{security_id}-reaction",
        "sue_available_at": dt.datetime(2025, 1, 31, 22),
        "reaction_available_at": dt.datetime(2025, 1, 16, 22),
        "sue_lineage_json": "{}", "reaction_lineage_json": "{}",
    }


def test_earnings_confirmation_equal_weights_and_lineage() -> None:
    rows = compute_earnings_confirmation_rows(
        pd.DataFrame([_row("A", 2.0, 1.0), _row("B", -1.0, -2.0)]),
        EarningsConfirmationOptions(minimum_names_per_date=2),
    ).set_index("security_id")
    assert rows.loc["A", "raw_value"] == pytest.approx(1.5)
    assert rows.loc["A", "value"] > rows.loc["B", "value"]
    assert json.loads(rows.loc["A", "input_lineage_json"])["method"] == "equal_weight_intersection"


def test_earnings_confirmation_is_governed_by_migration(tmp_store) -> None:
    assert tmp_store.con.execute(
        "SELECT family,is_point_in_time_safe,source FROM factor_definition WHERE factor_id=?",
        [FACTOR_ID],
    ).fetchone() == (
        "fundamental_earnings", True, "atx-db PIT earnings confirmation composite v1"
    )
