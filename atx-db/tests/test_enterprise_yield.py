from __future__ import annotations

import datetime as dt
import math

import pandas as pd

from atx_db.enterprise_yield import (
    VARIANT_FACTOR_IDS,
    EnterpriseYieldOptions,
    compute_enterprise_yield_rows,
)


def _input(variant: str, security_id: str, metric_value: float, enterprise_value: float) -> dict[str, object]:
    trade_date = dt.date(2020, 1, 31)
    available_at = dt.datetime(2020, 1, 31, 22)
    return {
        "variant": variant,
        "security_id": security_id,
        "symbol": security_id,
        "trade_date": trade_date,
        "decision_available_at": available_at,
        "enterprise_value_id": f"ev-{security_id}",
        "enterprise_value": enterprise_value,
        "available_at": available_at,
        "input_lineage_json": "{}",
        "metric_value": metric_value,
        "metric_id": f"metric-{security_id}-{variant}",
        "metric_available_at": available_at,
        "metric_source": "fixture",
        "period_end": dt.date(2019, 12, 31),
        "universe_valid_from": dt.date(2019, 1, 1),
        "universe_valid_to": None,
        "universe_available_at": available_at,
        "universe_source": "fixture",
    }


def test_compute_enterprise_yield_orients_and_standardizes_each_variant() -> None:
    inputs = pd.DataFrame(
        [
            _input("ebit", "A", 20.0, 100.0),
            _input("ebit", "B", 10.0, 100.0),
            _input("gross_profit", "A", 50.0, 100.0),
            _input("gross_profit", "B", 25.0, 100.0),
            _input("operating_cash_flow", "A", 30.0, 100.0),
            _input("operating_cash_flow", "B", 15.0, 100.0),
            _input("sales", "A", 80.0, 100.0),
            _input("sales", "B", 40.0, 100.0),
        ]
    )
    options = EnterpriseYieldOptions(minimum_names_per_date=2, winsor_limit=0.0, run_id="test")

    rows = compute_enterprise_yield_rows(inputs, options)

    assert len(rows) == 8
    assert set(rows["factor_id"]) == set(VARIANT_FACTOR_IDS.values())
    for _, group in rows.groupby("factor_id"):
        assert math.isclose(float(group["value"].mean()), 0.0, abs_tol=1e-12)
        assert group.sort_values("raw_value")["value"].is_monotonic_increasing
    assert rows["factor_value_id"].is_unique
    assert set(rows["run_id"]) == {"test"}


def test_compute_enterprise_yield_rejects_nonpositive_inputs_and_thin_dates() -> None:
    inputs = pd.DataFrame(
        [
            _input("ebit", "A", 20.0, 100.0),
            _input("ebit", "B", -1.0, 100.0),
            _input("sales", "A", 80.0, 0.0),
        ]
    )

    rows = compute_enterprise_yield_rows(
        inputs,
        EnterpriseYieldOptions(minimum_names_per_date=2, winsor_limit=0.0),
    )

    assert rows.empty
