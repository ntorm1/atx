from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.earnings_persistence import (
    FACTOR_ID,
    EarningsPersistenceOptions,
    compute_earnings_persistence_rows,
)
from atx_db.earnings_surprise import FACTOR_ID as SUE_FACTOR_ID


def _input(security_id: str, sue: float, volatility: float) -> dict[str, object]:
    available_at = dt.datetime(2025, 1, 31, 22)
    return {
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "sue_available_at": available_at,
        "sue_factor_value_id": f"{security_id}-sue",
        "sue_raw_value": sue,
        "sue_value": sue,
        "sue_lineage_json": "{}",
        "historical_std": volatility,
        "history_observations": 8,
    }


def test_low_earnings_volatility_increases_same_direction_sue_conviction() -> None:
    rows = compute_earnings_persistence_rows(
        pd.DataFrame([
            _input("A", -2.0, 0.10),
            _input("B", -1.0, 1.00),
            _input("C", 0.0, 0.50),
            _input("D", 1.0, 1.00),
            _input("E", 2.0, 0.10),
        ]),
        EarningsPersistenceOptions(minimum_names_per_date=3, winsor_limit=0.0),
    )
    raw = dict(zip(rows["security_id"], rows["raw_value"], strict=True))
    lineage = json.loads(rows.loc[rows["security_id"] == "E", "input_lineage_json"].iloc[0])

    assert raw["A"] < raw["B"] < 0 < raw["D"] < raw["E"]
    assert lineage["persistence_proxy"]["low_volatility_percentile"] == pytest.approx(0.9)
    assert json.loads(rows["input_ids_json"].iloc[0]) == [f"factor:{SUE_FACTOR_ID}"]


def test_earnings_persistence_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        "SELECT family,is_point_in_time_safe,source FROM factor_definition WHERE factor_id=?",
        [FACTOR_ID],
    ).fetchone()
    dependency = tmp_store.con.execute(
        "SELECT dependency_type,dependency_name FROM factor_dependency_edges WHERE factor_id=?",
        [FACTOR_ID],
    ).fetchone()

    assert definition == (
        "fundamental_earnings", True, "atx-db PIT earnings-persistence-weighted SUE v1"
    )
    assert dependency == ("factor", SUE_FACTOR_ID)
