from __future__ import annotations

import datetime as dt
import json

import pandas as pd

from atx_db.qmj_profitability import (
    COMPONENT_COLUMNS,
    FACTOR_ID,
    SOURCE_NAME,
    QmjProfitabilityOptions,
    compute_qmj_profitability_rows,
)


def _input_row(security_id: str, strength: float) -> dict[str, object]:
    return {
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "available_at": dt.datetime(2025, 1, 31, 22),
        "net_income": -5.0 + 25.0 * strength,
        "operating_cash_flow": 5.0 + 30.0 * strength,
        "gross_profit": 20.0 + 50.0 * strength,
        "revenue": 100.0,
        "total_assets": 120.0,
        "book_equity": 50.0,
        "piotroski_factor_value_id": f"{security_id}-piotroski",
        "book_to_market_factor_value_id": f"{security_id}-book-to-market",
        "book_equity_statement_point_id": f"{security_id}-equity",
        "book_equity_period_end": dt.date(2024, 12, 31),
        "book_equity_available_at": dt.datetime(2025, 1, 30, 22),
    }


def test_qmj_profitability_equal_weight_rank_composite() -> None:
    inputs = pd.DataFrame(
        [
            _input_row("STRONG", 1.0),
            _input_row("MID", 0.5),
            _input_row("WEAK", 0.0),
        ]
    )
    rows = compute_qmj_profitability_rows(
        inputs,
        QmjProfitabilityOptions(minimum_names_per_date=2),
    ).set_index("security_id")

    assert rows.loc["STRONG", "raw_value"] > rows.loc["MID", "raw_value"]
    assert rows.loc["MID", "raw_value"] > rows.loc["WEAK", "raw_value"]
    lineage = json.loads(rows.loc["STRONG", "input_lineage_json"])
    assert set(lineage["components"]) == set(COMPONENT_COLUMNS)
    assert lineage["research_contract"]["return_fitted_weights"] is False
    assert lineage["research_contract"]["complete_case"] is True
    assert "reported operating cash flow" in lineage["research_contract"][
        "reported_cash_flow_adaptation"
    ]
    assert lineage["book_equity"]["period_end"] == "2024-12-31"


def test_qmj_profitability_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,is_point_in_time_safe,source,
                  standardization_spec_json
           FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition is not None
    assert definition[1:3] == (True, SOURCE_NAME)
    assert "mean(zrank" in definition[0]
    standardization = json.loads(definition[3])
    assert standardization["component_count"] == 6
    assert standardization["return_fitted_weights"] is False
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name
           FROM factor_dependency_edges WHERE factor_id=?
           ORDER BY dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("factor", "quality_piotroski_f_score"),
        ("factor", "value_book_to_market"),
    ]
