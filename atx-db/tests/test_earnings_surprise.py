from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.earnings_surprise import FACTOR_ID, EarningsSurpriseOptions, compute_earnings_surprise_rows


def _row(security_id: str, sue: float) -> dict[str, object]:
    return {
        "security_id": security_id, "symbol": security_id,
        "trade_date": dt.date(2025, 1, 31),
        "price_available_at": dt.datetime(2025, 1, 31, 22), "sue": sue,
        "statement_point_id": f"{security_id}-current", "period_end": dt.date(2024, 9, 30),
        "available_at": dt.datetime(2024, 11, 1), "accession_number": f"{security_id}-current",
        "eps_diluted": 2.0, "prior_statement_point_id": f"{security_id}-prior",
        "prior_period_end": dt.date(2023, 9, 30), "prior_available_at": dt.datetime(2023, 11, 1),
        "prior_accession_number": f"{security_id}-prior", "prior_eps_diluted": 1.0,
        "unexpected_eps": 1.0, "history_observations": 8, "historical_std": 0.5,
        "market_cap_usd": 1_000_000_000.0, "adv21_usd": 10_000_000.0,
    }


def test_earnings_surprise_orientation_and_first_filed_lineage() -> None:
    rows = compute_earnings_surprise_rows(
        pd.DataFrame([_row("A", 2.0), _row("B", -1.0)]),
        EarningsSurpriseOptions(minimum_names_per_date=2, winsor_limit=0.0),
    ).set_index("security_id")
    assert rows.loc["A", "raw_value"] == pytest.approx(2.0)
    assert rows.loc["A", "value"] > rows.loc["B", "value"]
    lineage = json.loads(rows.loc["A", "input_lineage_json"])
    assert lineage["revision_policy"] == "first_filed_period_value_only"
    assert lineage["standardization"]["history_observations"] == 8


def test_earnings_surprise_is_governed_by_migration(tmp_store) -> None:
    assert tmp_store.con.execute(
        "SELECT family,is_point_in_time_safe,source FROM factor_definition WHERE factor_id=?",
        [FACTOR_ID],
    ).fetchone() == ("fundamental_earnings", True, "atx-db PIT standardized unexpected EPS v1")
