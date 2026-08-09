from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.piotroski import (
    FACTOR_ID,
    HIGH_BOOK_TO_MARKET_FACTOR_ID,
    SOURCE_NAME,
    PiotroskiOptions,
    compute_piotroski_rows,
)

_METRICS = (
    "net_income",
    "operating_cash_flow",
    "revenue",
    "gross_profit",
    "total_assets",
    "current_assets",
    "current_liabilities",
    "lt_debt",
)


def _row(
    security_id: str,
    *,
    strong: bool,
    book_percentile: float,
) -> dict[str, object]:
    if strong:
        current = {
            "net_income": 12.0,
            "operating_cash_flow": 15.0,
            "revenue": 110.0,
            "gross_profit": 60.0,
            "total_assets": 120.0,
            "current_assets": 60.0,
            "current_liabilities": 30.0,
            "lt_debt": 10.0,
        }
        prior = {
            "net_income": 5.0,
            "operating_cash_flow": 6.0,
            "revenue": 80.0,
            "gross_profit": 36.0,
            "total_assets": 100.0,
            "current_assets": 40.0,
            "current_liabilities": 25.0,
            "lt_debt": 12.0,
        }
        prior2_assets = 80.0
        low_net_issuance = 0.05
    else:
        current = {
            "net_income": -10.0,
            "operating_cash_flow": -15.0,
            "revenue": 100.0,
            "gross_profit": 20.0,
            "total_assets": 120.0,
            "current_assets": 20.0,
            "current_liabilities": 40.0,
            "lt_debt": 30.0,
        }
        prior = {
            "net_income": 0.0,
            "operating_cash_flow": 1.0,
            "revenue": 80.0,
            "gross_profit": 40.0,
            "total_assets": 100.0,
            "current_assets": 40.0,
            "current_liabilities": 20.0,
            "lt_debt": 10.0,
        }
        prior2_assets = 80.0
        low_net_issuance = -0.05

    available_at = dt.datetime(2025, 1, 31, 22)
    row: dict[str, object] = {
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "decision_available_at": available_at,
        "book_to_market": book_percentile,
        "book_to_market_percentile": book_percentile,
        "book_to_market_factor_value_id": f"{security_id}-bm",
        "net_issuance_factor_value_id": f"{security_id}-issuance",
        "low_net_issuance": low_net_issuance,
        "current_period_end": dt.date(2024, 12, 31),
        "prior_period_end": dt.date(2023, 12, 31),
        "prior2_period_end": dt.date(2022, 12, 31),
        "current_accession_number": f"{security_id}-current-accession",
        "prior_accession_number": f"{security_id}-prior-accession",
        "prior2_total_assets": prior2_assets,
        "prior2_total_assets_id": f"{security_id}-prior2-assets",
        "prior2_total_assets_available_at": dt.datetime(2023, 3, 1),
    }
    for prefix, values, timestamp in (
        ("current", current, dt.datetime(2025, 1, 15)),
        ("prior", prior, dt.datetime(2024, 1, 15)),
    ):
        for metric in _METRICS:
            row[f"{prefix}_{metric}"] = values[metric]
            row[f"{prefix}_{metric}_id"] = f"{security_id}-{prefix}-{metric}"
            row[f"{prefix}_{metric}_available_at"] = timestamp
    return row


def test_piotroski_exact_signals_and_high_value_condition() -> None:
    inputs = pd.DataFrame(
        [
            _row("STRONG", strong=True, book_percentile=0.95),
            _row("WEAK", strong=False, book_percentile=0.85),
            _row("MID", strong=True, book_percentile=0.50),
        ]
    )
    rows = compute_piotroski_rows(
        inputs,
        PiotroskiOptions(
            minimum_names_per_date=2,
            minimum_high_book_to_market_names_per_date=2,
        ),
    )
    main = rows[rows["factor_id"] == FACTOR_ID].set_index("security_id")
    conditional = rows[
        rows["factor_id"] == HIGH_BOOK_TO_MARKET_FACTOR_ID
    ].set_index("security_id")

    assert main.loc["STRONG", "raw_value"] == pytest.approx(9.0)
    assert main.loc["WEAK", "raw_value"] == pytest.approx(0.0)
    assert main.loc["STRONG", "value"] > main.loc["WEAK", "value"]
    assert set(conditional.index) == {"STRONG", "WEAK"}
    lineage = json.loads(conditional.loc["STRONG", "input_lineage_json"])
    assert all(lineage["signals"].values())
    assert lineage["inputs"]["prior2_total_assets"]["id"] == (
        "STRONG-prior2-assets"
    )
    assert "split-adjusted" in lineage["equity_offering_proxy"]


def test_piotroski_definitions_and_dependencies_are_production_governed(tmp_store) -> None:
    definitions = tmp_store.con.execute(
        """SELECT factor_id,expression,is_point_in_time_safe,source
           FROM factor_definition
           WHERE factor_id IN (?,?) ORDER BY factor_id""",
        [FACTOR_ID, HIGH_BOOK_TO_MARKET_FACTOR_ID],
    ).fetchall()
    assert len(definitions) == 2
    assert all(row[2:] == (True, SOURCE_NAME) for row in definitions)
    assert "F_ROA" in next(row[1] for row in definitions if row[0] == FACTOR_ID)
    edges = {
        (row[0], row[1], row[2])
        for row in tmp_store.con.execute(
            """SELECT factor_id,dependency_type,dependency_name
               FROM factor_dependency_edges
               WHERE factor_id IN (?,?)""",
            [FACTOR_ID, HIGH_BOOK_TO_MARKET_FACTOR_ID],
        ).fetchall()
    }
    assert (FACTOR_ID, "metric", "lt_debt") in edges
    assert (FACTOR_ID, "factor", "financing_low_net_share_issuance") in edges
    assert (
        HIGH_BOOK_TO_MARKET_FACTOR_ID,
        "factor",
        "value_book_to_market",
    ) in edges
