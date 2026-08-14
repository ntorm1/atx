from __future__ import annotations

import datetime as dt
import json

import pandas as pd

from atx_db.earnings_seasonality import (
    FACTOR_ID,
    EarningsSeasonalityOptions,
    compute_earnings_seasonality_rows,
)


def _input(security_id: str, earn_rank: float) -> dict[str, object]:
    available_at = dt.datetime(2025, 1, 31, 22)
    history = [
        {
            "statement_point_id": f"{security_id}-{index}",
            "period_end": str(dt.date(2019 + index // 4, 3 * (index % 4 + 1), 28)),
            "available_at": str(available_at),
            "split_adjusted_eps": float(index),
            "earnings_rank": index + 1,
            "is_same_fiscal_quarter": index % 4 == 3,
        }
        for index in range(20)
    ]
    return {
        "anchor_statement_point_id": f"{security_id}-anchor",
        "security_id": security_id,
        "symbol": security_id,
        "formation_trade_date": dt.date(2025, 1, 31),
        "decision_available_at": available_at,
        "formation_price_available_at": available_at,
        "formation_close": 20.0,
        "market_cap_usd": 1_000_000_000.0,
        "adv21_usd": 10_000_000.0,
        "predicted_announcement_month": dt.date(2025, 2, 1),
        "anchor_period_end": dt.date(2023, 12, 31),
        "anchor_available_at": dt.datetime(2024, 2, 15, 22),
        "anchor_accession_number": f"{security_id}-accession",
        "oldest_period_end": dt.date(2019, 3, 31),
        "latest_period_end": dt.date(2023, 12, 31),
        "latest_history_available_at": dt.datetime(2024, 2, 15, 22),
        "history_span_days": 1736,
        "history_quarters": 20,
        "same_quarter_observations": 5,
        "earn_rank": earn_rank,
        "history_json": json.dumps(history),
    }


def test_earnings_seasonality_preserves_earnrank_direction_and_lineage() -> None:
    rows = compute_earnings_seasonality_rows(
        pd.DataFrame([
            _input("A", 3.0),
            _input("B", 8.0),
            _input("C", 11.0),
            _input("D", 15.0),
            _input("E", 19.0),
        ]),
        EarningsSeasonalityOptions(minimum_names_per_date=3, winsor_limit=0.0),
    )
    values = dict(zip(rows["security_id"], rows["value"], strict=True))
    lineage = json.loads(rows.loc[rows["security_id"] == "E", "input_lineage_json"].iloc[0])

    assert values["A"] < values["B"] < values["C"] < values["D"] < values["E"]
    assert lineage["history_quarters"] == 20
    assert lineage["same_quarter_observations"] == 5
    assert lineage["formation"]["predicted_announcement_month"] == "2025-02-01"
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_earnings_seasonality_rejects_incomplete_history() -> None:
    incomplete = _input("A", 19.0)
    incomplete["history_quarters"] = 19
    rows = compute_earnings_seasonality_rows(
        pd.DataFrame([incomplete]),
        EarningsSeasonalityOptions(minimum_names_per_date=3),
    )
    assert rows.empty


def test_earnings_seasonality_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        "SELECT family,is_point_in_time_safe,source FROM factor_definition WHERE factor_id=?",
        [FACTOR_ID],
    ).fetchone()
    dependencies = tmp_store.con.execute(
        """
        SELECT dependency_type,dependency_name FROM factor_dependency_edges
        WHERE factor_id=? ORDER BY dependency_type,dependency_name
        """,
        [FACTOR_ID],
    ).fetchall()

    assert definition == (
        "fundamental_earnings", True, "atx-db PIT five-year earnings seasonality v1"
    )
    assert dependencies == [
        ("market", "equity_daily_bars"),
        ("metric", "eps_diluted_quarterly"),
    ]
