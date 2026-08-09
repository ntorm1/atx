from __future__ import annotations

import datetime as dt

import pandas as pd

from atx_db.cash_profitability import (
    FACTOR_IDS,
    CashProfitabilityOptions,
    compute_cash_profitability_rows,
)


def _inputs() -> pd.DataFrame:
    rows = []
    for date_offset in (0, 31):
        trade_date = dt.date(2024, 6, 28) + dt.timedelta(days=date_offset)
        for index in range(30):
            rows.append(
                {
                    "security_id": f"S{index:02d}",
                    "symbol": f"X{index:02d}",
                    "trade_date": trade_date,
                    "price_available_at": pd.Timestamp(trade_date) + pd.Timedelta(hours=22),
                    "ball_operating_profitability": index / 100.0,
                    "cash_operating_profitability": index / 80.0,
                    "operating_working_capital_accruals": index / 200.0,
                    "close": 10.0,
                    "adv21_usd": 2_000_000.0,
                    "market_cap_usd": 200_000_000.0,
                    "period_end": dt.date(2023, 12, 31),
                    "accession_number": "current",
                    "annual_available_at": pd.Timestamp("2024-02-15"),
                    "prior_period_end": dt.date(2022, 12, 31),
                    "prior_accession_number": "prior",
                    "prior_balance_available_at": pd.Timestamp("2023-02-15"),
                    "prior_total_assets": 1_000.0,
                    "prior_total_assets_id": "assets-prior",
                    "revenue_id": "revenue-current",
                    "cogs_id": "cogs-current",
                    "sga_id": "sga-current",
                    "rd_expense_id": None,
                }
            )
    return pd.DataFrame(rows)


def test_cash_profitability_rows_emit_oriented_factor_family() -> None:
    result = compute_cash_profitability_rows(
        _inputs(),
        CashProfitabilityOptions(run_id="cash-test"),
    )

    assert set(result["factor_id"]) == set(FACTOR_IDS)
    assert len(result) == 180
    assert result["factor_value_id"].is_unique
    assert result["input_lineage_json"].str.contains("prior_year_total_assets").all()

    cash = result[result["factor_id"] == "profitability_cash_operating_profitability"]
    accrual = result[
        result["factor_id"] == "quality_low_operating_working_capital_accruals"
    ]
    assert cash["raw_value"].corr(cash["value"]) > 0.99
    assert accrual["raw_value"].corr(accrual["value"]) < -0.99


def test_cash_profitability_empty_input_is_typed() -> None:
    result = compute_cash_profitability_rows(pd.DataFrame())
    assert result.empty
    assert {"factor_value_id", "factor_id", "input_lineage_json"}.issubset(result.columns)


def test_cash_profitability_factor_definitions_are_migrated(tmp_store) -> None:
    rows = tmp_store.con.execute(
        """
        SELECT factor_id, direction, is_point_in_time_safe, declared_in
        FROM factor_definition
        WHERE factor_id IN (?, ?, ?)
        ORDER BY factor_id
        """,
        list(FACTOR_IDS),
    ).fetchall()
    assert {row[0] for row in rows} == set(FACTOR_IDS)
    assert all(bool(row[2]) for row in rows)
    assert all(row[3] == "atx_db.cash_profitability" for row in rows)
    assert tmp_store.con.execute(
        "SELECT count(*) FROM factor_dependency_edges WHERE factor_id IN (?, ?, ?)",
        list(FACTOR_IDS),
    ).fetchone()[0] == len(FACTOR_IDS) * 11
