from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.altman_distress import (
    FACTOR_ID,
    AltmanDistressOptions,
    compute_altman_distress_rows,
)


def _row(security_id: str, score: float) -> dict[str, object]:
    values = {
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "decision_available_at": dt.datetime(2025, 1, 31, 22),
        "altman_z_score": score,
        "cash_scaffold_factor_value_id": f"{security_id}-cash",
        "book_to_market_factor_value_id": f"{security_id}-bm",
        "ttm_end_date": dt.date(2024, 9, 30),
        "asset_period_end": dt.date(2024, 9, 30),
        "assets": 500.0,
        "market_cap_usd": 600.0,
    }
    for prefix, value in (
        ("current_assets", 200.0),
        ("current_liabilities", 100.0),
        ("retained_earnings", 80.0),
        ("total_liabilities", 250.0),
        ("ebit_ttm", 60.0),
        ("revenue_ttm", 700.0),
    ):
        values[prefix] = value
        values[f"{prefix}_id"] = f"{security_id}-{prefix}"
        values[f"{prefix}_available_at"] = dt.datetime(2024, 11, 1)
    return values


def test_altman_distress_orientation_and_corrected_liability_lineage() -> None:
    rows = compute_altman_distress_rows(
        pd.DataFrame([_row("A", 4.0), _row("B", 1.0)]),
        AltmanDistressOptions(minimum_names_per_date=2, winsor_limit=0.0),
    ).set_index("security_id")
    assert rows.loc["A", "raw_value"] == pytest.approx(4.0)
    assert rows.loc["A", "value"] > rows.loc["B", "value"]
    lineage = json.loads(rows.loc["A", "input_lineage_json"])
    assert "total liabilities" in lineage["correction"]
    assert lineage["inputs"]["total_liabilities"]["id"] == "A-total_liabilities"


def test_altman_distress_is_corrected_and_governed_by_migration(tmp_store) -> None:
    row = tmp_store.con.execute(
        """SELECT expression,input_ids_json,is_point_in_time_safe,source
           FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert "total_liabilities" in row[0]
    assert "total_debt" not in row[0]
    assert "metric:total_liabilities" in row[1]
    assert row[2:] == (True, "atx-db PIT corrected Altman Z-score v1")
