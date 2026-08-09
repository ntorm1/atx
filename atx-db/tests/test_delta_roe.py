from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.delta_roe import (
    FACTOR_ID,
    SOURCE_NAME,
    DeltaRoeOptions,
    compute_delta_roe_rows,
)


def _input_row(security_id: str, current: float, prior: float) -> dict[str, object]:
    return {
        "roe_factor_value_id": f"{security_id}-roe-current",
        "prior_roe_factor_value_id": f"{security_id}-roe-prior",
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 1, 31),
        "quarterly_roe": current,
        "prior_quarterly_roe": prior,
        "available_at": dt.datetime(2025, 1, 31, 22),
        "prior_roe_available_at": dt.datetime(2024, 1, 31, 22),
        "earnings_period_end": dt.date(2024, 12, 31),
        "prior_earnings_period_end": dt.date(2023, 12, 31),
        "prior_roe_as_of_date": dt.date(2024, 1, 31),
    }


def test_delta_roe_orientation_and_lineage() -> None:
    inputs = pd.DataFrame(
        [
            _input_row("IMPROVE", 0.20, 0.05),
            _input_row("FLAT", 0.10, 0.10),
            _input_row("DETERIORATE", -0.05, 0.10),
        ]
    )
    rows = compute_delta_roe_rows(
        inputs,
        DeltaRoeOptions(minimum_names_per_date=2, winsor_limit=0.0),
    ).set_index("security_id")

    assert rows.loc["IMPROVE", "raw_value"] > rows.loc["FLAT", "raw_value"]
    assert rows.loc["FLAT", "raw_value"] > rows.loc["DETERIORATE", "raw_value"]
    lineage = json.loads(rows.loc["IMPROVE", "input_lineage_json"])
    assert lineage["delta_roe"] == pytest.approx(0.15)
    assert lineage["current"]["factor_value_id"] == "IMPROVE-roe-current"
    assert lineage["prior_four_quarter"]["earnings_period_end"] == "2023-12-31"
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_delta_roe_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,is_point_in_time_safe,source,standardization_spec_json
           FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition is not None
    assert definition[1:3] == (True, SOURCE_NAME)
    assert definition[0] == "quarterly_roe_t-quarterly_roe_t_minus_4"
    standardization = json.loads(definition[3])
    assert standardization["period_gap_days"] == [300, 430]
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name
           FROM factor_dependency_edges WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [("factor", "profitability_q_factor_roe")]
