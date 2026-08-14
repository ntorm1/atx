from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.earnings_acceleration import (
    FACTOR_ID,
    SOURCE_NAME,
    EarningsAccelerationOptions,
    compute_earnings_acceleration_rows,
)


def _row(security_id: str, acceleration: float) -> dict[str, object]:
    available = dt.datetime(2025, 2, 1, 22)
    return {
        "sue_factor_value_id": f"{security_id}-sue",
        "security_id": security_id,
        "symbol": security_id,
        "as_of_date": dt.date(2025, 2, 28),
        "decision_available_at": available,
        "earnings_acceleration": acceleration,
        "current_earnings_growth": acceleration + 0.02,
        "prior_earnings_growth": 0.02,
        "statement_point_id": f"{security_id}-t",
        "period_end": dt.date(2024, 12, 31),
        "available_at": available,
        "accession_number": f"{security_id}-t-accession",
        "eps_diluted": 2.0,
        "split_index": 1.0,
        "adjusted_eps": 2.0,
        "lag1_statement_point_id": f"{security_id}-t1",
        "lag1_period_end": dt.date(2024, 9, 30),
        "lag1_available_at": dt.datetime(2024, 11, 1),
        "lag1_accession_number": f"{security_id}-t1-accession",
        "lag1_eps_diluted": 1.8,
        "lag1_split_index": 1.0,
        "lag1_adjusted_eps": 1.8,
        "lag4_statement_point_id": f"{security_id}-t4",
        "lag4_period_end": dt.date(2023, 12, 31),
        "lag4_available_at": dt.datetime(2024, 2, 1),
        "lag4_accession_number": f"{security_id}-t4-accession",
        "lag4_eps_diluted": 1.5,
        "lag4_split_index": 1.0,
        "lag4_adjusted_eps": 1.5,
        "lag5_statement_point_id": f"{security_id}-t5",
        "lag5_period_end": dt.date(2023, 9, 30),
        "lag5_available_at": dt.datetime(2023, 11, 1),
        "lag5_accession_number": f"{security_id}-t5-accession",
        "lag5_eps_diluted": 1.4,
        "lag5_split_index": 1.0,
        "lag5_adjusted_eps": 1.4,
        "lag1_price_date": dt.date(2024, 9, 30),
        "lag1_price_available_at": dt.datetime(2024, 10, 1),
        "lag1_period_end_price": 25.0,
        "lag1_adjusted_period_end_price": 25.0,
        "lag2_price_date": dt.date(2024, 6, 28),
        "lag2_price_available_at": dt.datetime(2024, 6, 29),
        "lag2_period_end_price": 20.0,
        "lag2_adjusted_period_end_price": 20.0,
    }


def test_earnings_acceleration_orientation_and_lineage() -> None:
    result = compute_earnings_acceleration_rows(
        pd.DataFrame([_row("LOW", -0.1), _row("MID", 0.0), _row("HIGH", 0.1)]),
        EarningsAccelerationOptions(minimum_names_per_date=2, winsor_limit=0.0),
    ).set_index("security_id")
    assert result.loc["HIGH", "raw_value"] == pytest.approx(0.1)
    assert result.loc["HIGH", "value"] > result.loc["MID", "value"]
    assert result.loc["MID", "value"] > result.loc["LOW", "value"]
    lineage = json.loads(result.loc["HIGH", "input_lineage_json"])
    assert lineage["revision_policy"] == "first_filed_period_value_only"
    assert lineage["quarters"]["lag4"]["eps_diluted"] == pytest.approx(1.5)
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_earnings_acceleration_rejects_nonfinite_and_extreme_values() -> None:
    result = compute_earnings_acceleration_rows(
        pd.DataFrame(
            [
                _row("GOOD", 0.1),
                _row("GOOD_2", -0.1),
                _row("EXTREME", 20.0),
                _row("NONFINITE", float("inf")),
            ]
        ),
        EarningsAccelerationOptions(minimum_names_per_date=1, winsor_limit=0.0),
    )
    assert result["security_id"].tolist() == ["GOOD", "GOOD_2"]


def test_earnings_acceleration_definition_is_governed(tmp_store) -> None:
    definition = tmp_store.con.execute(
        """SELECT expression,direction,is_point_in_time_safe,source,
                  standardization_spec_json FROM factor_definition WHERE factor_id=?""",
        [FACTOR_ID],
    ).fetchone()
    assert definition[:4] == (
        "((eps_t-eps_t_4)/price_t_1)-((eps_t_1-eps_t_5)/price_t_2)",
        1,
        True,
        SOURCE_NAME,
    )
    standardization = json.loads(definition[4])
    assert standardization["revision_policy"] == "first_filed_period_value_only"
    assert standardization["return_fitted_parameters"] is False
    dependencies = tmp_store.con.execute(
        """SELECT dependency_type,dependency_name FROM factor_dependency_edges
           WHERE factor_id=? ORDER BY 1,2""",
        [FACTOR_ID],
    ).fetchall()
    assert dependencies == [
        ("factor", "earnings_standardized_unexpected_eps"),
        ("market", "close"),
        ("metric", "eps_diluted_quarterly"),
    ]
