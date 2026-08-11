from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.annual_margin_change import (
    VARIANT_FACTOR_IDS,
    AnnualMarginChangeOptions,
    compute_annual_margin_change_rows,
)


def _row(
    security_id: str,
    current_numerator: float,
    prior_numerator: float,
    *,
    variant: str = "net_margin",
) -> dict[str, object]:
    return {
        "variant": variant,
        "security_id": security_id,
        "symbol": security_id,
        "trade_date": dt.date(2025, 3, 31),
        "decision_available_at": dt.datetime(2025, 3, 31, 22),
        "current_revenue": 100.0,
        "current_numerator": current_numerator,
        "prior_revenue": 100.0,
        "prior_numerator": prior_numerator,
        "current_revenue_id": f"{security_id}-revenue-current",
        "current_numerator_id": f"{security_id}-numerator-current",
        "prior_revenue_id": f"{security_id}-revenue-prior",
        "prior_numerator_id": f"{security_id}-numerator-prior",
        "current_accession_number": f"{security_id}-current",
        "prior_accession_number": f"{security_id}-prior",
        "current_period_end": dt.date(2024, 12, 31),
        "prior_period_end": dt.date(2023, 12, 31),
        "current_revenue_available_at": dt.datetime(2025, 2, 20, 22),
        "current_numerator_available_at": dt.datetime(2025, 2, 20, 22),
        "prior_revenue_available_at": dt.datetime(2024, 2, 20, 22),
        "prior_numerator_available_at": dt.datetime(2024, 2, 20, 22),
        "universe_id": "us_equity_research_v1",
        "universe_valid_from": dt.date(2025, 1, 1),
        "universe_valid_to": None,
        "universe_available_at": dt.datetime(2025, 1, 1),
        "universe_source": "test",
    }


def test_annual_margin_change_orientation_and_lineage() -> None:
    result = compute_annual_margin_change_rows(
        pd.DataFrame(
            [
                _row("LOW", 5.0, 10.0),
                _row("MID", 10.0, 10.0),
                _row("HIGH", 20.0, 10.0),
            ]
        ),
        AnnualMarginChangeOptions(minimum_names_per_date=2),
    ).set_index("security_id")
    assert result.loc["HIGH", "value"] > result.loc["MID", "value"]
    assert result.loc["MID", "value"] > result.loc["LOW", "value"]
    assert result.loc["HIGH", "available_at"] == pd.Timestamp("2025-03-31 22:00:00")
    lineage = json.loads(result.loc["HIGH", "input_lineage_json"])
    assert lineage["formula"] == "net_income_t/revenue_t-net_income_t_1/revenue_t_1"
    assert lineage["change"] == pytest.approx(0.1)
    assert lineage["research_contract"]["return_fitted_parameters"] is False


def test_annual_margin_change_keeps_variants_independent() -> None:
    frame = pd.DataFrame(
        [
            _row("A", 20.0, 10.0, variant="net_margin"),
            _row("B", 10.0, 10.0, variant="net_margin"),
            _row("A", 30.0, 10.0, variant="gross_margin"),
            _row("B", 20.0, 10.0, variant="gross_margin"),
        ]
    )
    result = compute_annual_margin_change_rows(
        frame, AnnualMarginChangeOptions(minimum_names_per_date=2)
    )
    assert set(result["factor_id"]) == {
        VARIANT_FACTOR_IDS["net_margin"],
        VARIANT_FACTOR_IDS["gross_margin"],
    }
    assert result.groupby("factor_id")["value"].mean().abs().max() < 1e-12


def test_annual_margin_change_rejects_bad_revenue_and_unknown_variants() -> None:
    bad = _row("BAD", 20.0, 10.0)
    bad["current_revenue"] = 0.0
    good = _row("GOOD", 20.0, 10.0)
    good_2 = _row("GOOD_2", 15.0, 10.0)
    result = compute_annual_margin_change_rows(
        pd.DataFrame([bad, good, good_2]),
        AnnualMarginChangeOptions(minimum_names_per_date=1),
    )
    assert set(result["security_id"]) == {"GOOD", "GOOD_2"}

    unknown = _row("UNKNOWN", 20.0, 10.0, variant="mystery")
    with pytest.raises(ValueError, match="Unknown annual-margin variants"):
        compute_annual_margin_change_rows(
            pd.DataFrame([unknown]),
            AnnualMarginChangeOptions(minimum_names_per_date=1),
        )
