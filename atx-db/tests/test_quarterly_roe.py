from __future__ import annotations

import datetime as dt
import json

import pandas as pd

from atx_db.quarterly_roe import (
    FACTOR_ID,
    SOURCE_NAME,
    QuarterlyRoeOptions,
    compute_quarterly_roe_rows,
)


def _input_row(
    security_id: str,
    quarterly_net_income: float,
    stockholders_equity: float = 100.0,
) -> dict[str, object]:
    return {
        "security_id": security_id,
        "symbol": security_id,
        "trade_date": dt.date(2025, 1, 31),
        "decision_available_at": dt.datetime(2025, 1, 31, 22),
        "quarterly_net_income": quarterly_net_income,
        "quarterly_net_income_id": f"{security_id}-income",
        "quarterly_net_income_available_at": dt.datetime(2025, 1, 30, 22),
        "earnings_accession_number": f"{security_id}-earnings-filing",
        "earnings_period_start": dt.date(2024, 10, 1),
        "earnings_period_end": dt.date(2024, 12, 31),
        "stockholders_equity": stockholders_equity,
        "stockholders_equity_id": f"{security_id}-equity",
        "stockholders_equity_available_at": dt.datetime(2024, 11, 1, 22),
        "equity_accession_number": f"{security_id}-equity-filing",
        "equity_period_end": dt.date(2024, 9, 30),
        "universe_id": "us_common_equity_liquid_v1",
        "universe_valid_from": dt.date(2025, 1, 1),
        "universe_valid_to": None,
        "universe_available_at": dt.datetime(2025, 1, 1),
        "universe_source": "test",
    }


def test_quarterly_roe_orientation_and_lineage() -> None:
    inputs = pd.DataFrame(
        [
            _input_row("STRONG", 20.0),
            _input_row("MID", 5.0),
            _input_row("WEAK", -10.0),
        ]
    )
    rows = compute_quarterly_roe_rows(
        inputs,
        QuarterlyRoeOptions(minimum_names_per_date=2, winsor_limit=0.0),
    ).set_index("security_id")

    assert rows.loc["STRONG", "raw_value"] > rows.loc["MID", "raw_value"]
    assert rows.loc["MID", "raw_value"] > rows.loc["WEAK", "raw_value"]
    lineage = json.loads(rows.loc["STRONG", "input_lineage_json"])
    assert lineage["quarterly_roe"] == 0.2
    assert lineage["lagged_book_equity"]["period_end"] == "2024-09-30"
    assert lineage["research_contract"]["return_fitted_parameters"] is False
    assert "income before extraordinary" in lineage["published_method_adaptations"][
        "numerator"
    ]


def test_quarterly_roe_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,is_point_in_time_safe,source,standardization_spec_json
           FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition is not None
    assert definition[1:3] == (True, SOURCE_NAME)
    assert definition[0] == (
        "quarterly_net_income/one_quarter_lagged_stockholders_equity"
    )
    standardization = json.loads(definition[3])
    assert standardization["numerator_adaptation"] == "reported_net_income"
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name
           FROM factor_dependency_edges WHERE factor_id=? ORDER BY dependency_name""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("metric", "net_income"),
        ("metric", "stockholders_equity"),
    ]
