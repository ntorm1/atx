from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.earnings_surprise import FACTOR_ID as SUE_FACTOR_ID
from atx_db.earnings_surprise import SOURCE_NAME as SUE_SOURCE_NAME
from atx_db.fundamental_momentum import (
    FACTOR_ID,
    FundamentalMomentumOptions,
    compute_fundamental_momentum_rows,
    load_fundamental_momentum_inputs,
)


def _input(security_id: str, sue: float, momentum: float) -> dict[str, object]:
    available_at = dt.datetime(2025, 1, 31, 22)
    return {
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "decision_available_at": available_at,
        "sue_factor_value_id": f"{security_id}-sue",
        "sue_value": sue,
        "sue_available_at": available_at,
        "sue_lineage_json": "{}",
        "price_momentum_12_1": momentum,
        "reference_trade_date": dt.date(2025, 1, 31),
        "reference_available_at": available_at,
        "momentum_end_date": dt.date(2024, 12, 31),
        "momentum_end_available_at": dt.datetime(2024, 12, 31, 22),
        "momentum_end_close": 110.0,
        "momentum_start_date": dt.date(2024, 1, 31),
        "momentum_start_available_at": dt.datetime(2024, 1, 31, 22),
        "momentum_start_close": 100.0,
    }


def test_fundamental_momentum_residual_is_orthogonal_to_price_rank() -> None:
    inputs = pd.DataFrame(
        [
            _input("A", -2.0, -0.20),
            _input("B", -1.0, -0.10),
            _input("C", 2.0, 0.00),
            _input("D", 1.0, 0.10),
            _input("E", 0.0, 0.20),
        ]
    )
    rows = compute_fundamental_momentum_rows(
        inputs,
        FundamentalMomentumOptions(minimum_names_per_date=3, winsor_limit=0.0),
    )
    lineage = [json.loads(value) for value in rows["input_lineage_json"]]
    residuals = pd.Series([item["cross_sectional_regression"]["residual"] for item in lineage])
    momentum_ranks = pd.Series([item["price_momentum"]["rank"] for item in lineage])

    assert len(rows) == 5
    assert residuals.mean() == pytest.approx(0.0, abs=1e-12)
    assert ((momentum_ranks - momentum_ranks.mean()) * residuals).sum() == pytest.approx(
        0.0, abs=1e-12
    )
    assert rows.loc[rows["security_id"] == "C", "value"].iloc[0] > 0
    assert lineage[0]["price_momentum"]["end_available_at"] == "2024-12-31 22:00:00"
    assert lineage[0]["price_momentum"]["start_available_at"] == "2024-01-31 22:00:00"


def test_price_momentum_loader_adjusts_splits_and_skips_recent_session(tmp_store) -> None:
    as_of_date = dt.date(2025, 1, 6)
    available_at = dt.datetime(2025, 1, 6, 22)
    for security_id in ("A", "B", "C"):
        tmp_store.con.execute(
            """
            INSERT INTO fundamental_factor_values (
                factor_value_id,factor_id,factor_name,family,security_id,symbol,
                as_of_date,raw_value,value,available_at,input_ids_json,
                input_lineage_json,is_latest_revision,run_id,source
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
            """,
            [
                f"{security_id}-sue",
                SUE_FACTOR_ID,
                "SUE",
                "fundamental_earnings",
                security_id,
                security_id,
                as_of_date,
                1.0,
                1.0,
                available_at,
                "[]",
                "{}",
                True,
                "test",
                SUE_SOURCE_NAME,
            ],
        )
        for trade_date, close, split_factor in (
            [
                (dt.date(2024, 12, 30), 100.0, 1.0),
                (dt.date(2024, 12, 31), 50.0, 0.5),
                (dt.date(2025, 1, 2), 55.0, 1.0),
                (dt.date(2025, 1, 3), 60.0, 1.0),
                (dt.date(2025, 1, 6), 66.0, 1.0),
            ]
        ):
            bar_available_at = dt.datetime.combine(trade_date, dt.time(22))
            tmp_store.con.execute(
                """
                INSERT INTO equity_daily_bars (
                    source,security_id,symbol,trade_date,close,split_factor,
                    is_adjusted,available_at,run_id
                ) VALUES ('test',?,?,?,?,?,false,?,'test')
                """,
                [security_id, security_id, trade_date, close, split_factor, bar_available_at],
            )

    inputs = load_fundamental_momentum_inputs(
        tmp_store,
        FundamentalMomentumOptions(
            skip_sessions=1,
            lookback_sessions=4,
            minimum_names_per_date=3,
        ),
    )

    assert len(inputs) == 3
    assert inputs.iloc[0]["momentum_end_date"].date() == dt.date(2025, 1, 3)
    assert inputs.iloc[0]["momentum_start_date"].date() == dt.date(2024, 12, 30)
    assert inputs.iloc[0]["price_momentum_12_1"] == pytest.approx(0.20)


def test_fundamental_momentum_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """
        SELECT family,is_point_in_time_safe,source
        FROM factor_definition WHERE factor_id=?
        """,
        [FACTOR_ID],
    ).fetchone()
    dependencies = tmp_store.con.execute(
        """
        SELECT dependency_type,dependency_name
        FROM factor_dependency_edges WHERE factor_id=?
        ORDER BY dependency_type,dependency_name
        """,
        [FACTOR_ID],
    ).fetchall()

    assert definition == (
        "fundamental_earnings",
        True,
        "atx-db PIT price-controlled fundamental momentum v1",
    )
    assert dependencies == [
        ("factor", SUE_FACTOR_ID),
        ("market", "equity_daily_bars"),
    ]
