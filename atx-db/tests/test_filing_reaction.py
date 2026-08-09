from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.filing_reaction import FACTOR_ID, FilingReactionOptions, compute_filing_reaction_rows


def _row(security_id: str, reaction: float) -> dict[str, object]:
    return {
        "security_id": security_id, "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "decision_available_at": dt.datetime(2025, 1, 31, 22),
        "abnormal_return": reaction, "sue_factor_value_id": f"{security_id}-sue",
        "statement_point_id": f"{security_id}-statement", "accession_number": f"{security_id}-10Q",
        "form": "10-Q", "filed_date": dt.date(2025, 1, 15),
        "filing_available_at": dt.datetime(2025, 1, 15, 22),
        "reaction_date": dt.date(2025, 1, 16),
        "reaction_available_at": dt.datetime(2025, 1, 16, 22),
        "prior_close": 100.0, "reaction_close": 102.0,
        "reaction_adjustment_factor": 1.0, "daily_return": 0.02, "market_return": 0.005,
    }


def test_filing_reaction_orientation_and_event_semantics() -> None:
    rows = compute_filing_reaction_rows(
        pd.DataFrame([_row("A", 0.015), _row("B", -0.01)]),
        FilingReactionOptions(minimum_names_per_date=2, winsor_limit=0.0),
    ).set_index("security_id")
    assert rows.loc["A", "raw_value"] == pytest.approx(0.015)
    assert rows.loc["A", "value"] > rows.loc["B", "value"]
    lineage = json.loads(rows.loc["A", "input_lineage_json"])
    assert lineage["event_semantics"].startswith("first_complete_trading_session")
    assert "synthetic 22:00" in lineage["timestamp_limitation"]


def test_filing_reaction_is_governed_by_migration(tmp_store) -> None:
    assert tmp_store.con.execute(
        "SELECT family,is_point_in_time_safe,source FROM factor_definition WHERE factor_id=?",
        [FACTOR_ID],
    ).fetchone() == ("fundamental_earnings", True, "atx-db PIT SEC filing reaction v1")
