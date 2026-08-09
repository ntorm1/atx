from __future__ import annotations

import json

import pandas as pd
import pytest

from atx_db.fundamental_signals import (
    FACTOR_IDS,
    FundamentalSignalOptions,
    compute_fundamental_signal_rows,
)


def _inputs() -> pd.DataFrame:
    rows = []
    decision = pd.Timestamp("2024-06-28 22:00:00")
    for i in range(5):
        rows.append(
            {
                "security_id": f"SEC-{i}",
                "symbol": f"S{i}",
                "trade_date": pd.Timestamp("2024-06-28"),
                "price_available_at": decision,
                "close": 10.0 + i,
                "adv21_usd": 2_000_000.0,
                "market_cap_usd": 200_000_000.0 + i,
                "gross_profitability": 0.10 + i * 0.05,
                "operating_profitability": 0.08 + i * 0.04,
                "book_to_market": 0.20 + i * 0.10,
                "period_end": pd.Timestamp("2023-12-31"),
                "accession_number": f"ACC-{i}",
                "gross_profit": 100.0 + i,
                "gross_profit_available_at": pd.Timestamp("2024-02-15 21:00:00"),
                "gross_profit_id": f"GP-{i}",
                "total_assets": 500.0,
                "total_assets_available_at": pd.Timestamp("2024-02-15 21:00:00"),
                "total_assets_id": f"TA-{i}",
                "stockholders_equity": 200.0,
                "stockholders_equity_available_at": pd.Timestamp("2024-02-15 21:00:00"),
                "stockholders_equity_id": f"EQ-{i}",
                "revenue": 1_000.0 + i,
                "revenue_available_at": pd.Timestamp("2024-02-15 21:00:00"),
                "revenue_id": f"REV-{i}",
                "cogs": 600.0,
                "cogs_available_at": pd.Timestamp("2024-02-15 21:00:00"),
                "cogs_id": f"COGS-{i}",
                "sga": 200.0,
                "sga_available_at": pd.Timestamp("2024-02-15 21:00:00"),
                "sga_id": f"SGA-{i}",
                "interest_expense": 20.0,
                "interest_expense_available_at": pd.Timestamp("2024-02-15 21:00:00"),
                "interest_expense_id": f"INT-{i}",
                "share_count": 10_000_000.0,
                "share_history_id": f"SH-{i}",
                "share_available_at": pd.Timestamp("2024-02-15 21:00:00"),
            }
        )
    return pd.DataFrame(rows)


def test_compute_fundamental_signal_rows_emits_three_pit_factors() -> None:
    options = FundamentalSignalOptions(
        minimum_names_per_date=3,
        winsor_limit=0.0,
        run_id="loop-1",
    )
    rows = compute_fundamental_signal_rows(_inputs(), options)

    assert set(rows["factor_id"]) == set(FACTOR_IDS)
    assert len(rows) == 20
    assert rows["available_at"].eq(pd.Timestamp("2024-06-28 22:00:00")).all()
    assert rows["factor_value_id"].nunique() == len(rows)
    assert rows.groupby("factor_id")["value"].mean().abs().max() < 1e-12
    assert rows.groupby("factor_id")["value"].std().sub(1.0).abs().max() < 1e-12

    composite = rows[rows["factor_id"] == "quality_value_gross_profitability"]
    assert composite.sort_values("security_id")["raw_value"].tolist() == pytest.approx(
        [0.0, 0.25, 0.5, 0.75, 1.0]
    )


def test_compute_fundamental_signal_rows_preserves_reproducible_lineage() -> None:
    rows = compute_fundamental_signal_rows(
        _inputs(),
        FundamentalSignalOptions(minimum_names_per_date=3, winsor_limit=0.0),
    )
    row = rows[
        (rows["factor_id"] == "profitability_gross_profitability")
        & (rows["security_id"] == "SEC-0")
    ].iloc[0]
    lineage = json.loads(row["input_lineage_json"])

    assert lineage["gross_profit"]["id"] == "GP-0"
    assert lineage["total_assets"]["id"] == "TA-0"
    assert lineage["decision"]["trade_date"] == "2024-06-28"
    assert lineage["gross_profit"]["available_at"] < lineage["decision"]["price_available_at"]


def test_compute_fundamental_signal_rows_drops_thin_cross_sections() -> None:
    rows = compute_fundamental_signal_rows(
        _inputs().head(2),
        FundamentalSignalOptions(minimum_names_per_date=3),
    )
    assert rows.empty


def test_production_factor_definitions_are_migrated(tmp_store) -> None:
    definitions = tmp_store.con.execute(
        """
        SELECT factor_id, family, is_point_in_time_safe
        FROM factor_definition
        WHERE factor_id IN (
            'profitability_gross_profitability',
            'value_book_to_market',
            'quality_value_gross_profitability'
        )
        ORDER BY factor_id
        """
    ).fetchall()
    assert len(definitions) == 3
    assert all(row[2] for row in definitions)
    assert definitions[1][1] == "fundamental_quality_value"

    dependencies = tmp_store.con.execute(
        """
        SELECT dependency_type, dependency_name
        FROM factor_dependency_edges
        WHERE factor_id = 'quality_value_gross_profitability'
        ORDER BY dependency_name
        """
    ).fetchall()
    assert dependencies == [
        ("factor", "profitability_gross_profitability"),
        ("factor", "value_book_to_market"),
    ]
