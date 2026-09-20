"""Focused catalog coverage for core quarterly growth and efficiency metrics."""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
from atx_db.derived_registry import (
    RECLAIMED_ITEM_CODES,
    default_derived_definitions,
    known_item_codes,
    seed_derived_metric_definitions,
)

_QUARTERS = (
    dt.date(2019, 3, 31), dt.date(2019, 6, 30), dt.date(2019, 9, 30), dt.date(2019, 12, 31),
    dt.date(2020, 3, 31), dt.date(2020, 6, 30), dt.date(2020, 9, 30), dt.date(2020, 12, 31),
    dt.date(2021, 3, 31), dt.date(2021, 6, 30), dt.date(2021, 9, 30), dt.date(2021, 12, 31),
    dt.date(2022, 3, 31), dt.date(2022, 6, 30), dt.date(2022, 9, 30), dt.date(2022, 12, 31),
)


def _available(period_end: dt.date, delay: int = 40) -> dt.datetime:
    return dt.datetime.combine(period_end + dt.timedelta(days=delay), dt.time(21, 0))


def _insert(store, security_id: str, code: str, basis: str, period_end: dt.date, value: float, available_at: dt.datetime) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_standardized (
            standardized_id, source, security_id, item_id, canonical_code, basis,
            period_end, value, as_of_date, available_at, input_codes_json,
            input_item_ids_json, rule_id, combination_rule, revision_sequence,
            is_latest_revision
        ) VALUES (?, 'test', ?, 1, ?, ?, ?, ?, ?, ?, '[]', '[]', 'r', 'direct', 1, true)
        """,
        [f"{security_id}|{code}|{period_end}", security_id, code, basis, period_end, value, available_at.date(), available_at],
    )


def _value(store, code: str, period_end: dt.date):
    return store.con.execute(
        """SELECT value, available_at FROM derived_metric_values
           WHERE security_id = 'S1' AND metric_code = ? AND period_end = ?
           ORDER BY available_at DESC, derived_value_id DESC LIMIT 1""",
        [code, period_end],
    ).fetchone()


def _latest_state(store, code: str, period_end: dt.date, security_id: str = "S1"):
    return store.con.execute(
        """SELECT value, value_status, available_at FROM derived_metric_values
           WHERE security_id = ? AND metric_code = ? AND period_end = ?
           ORDER BY available_at DESC, derived_value_id DESC LIMIT 1""",
        [security_id, code, period_end],
    ).fetchone()


def _seed_core_series(store, security_id: str = "S1", *, negative_bases: bool = False) -> None:
    for index, period_end in enumerate(_QUARTERS):
        revenue = (-100.0 if negative_bases else 100.0) + index * (0.0 if negative_bases else 10.0)
        cogs = -60.0 if negative_bases else 60.0 + index * 2.0
        values = {
            "revenue": revenue,
            "cost_of_revenue_cogs": cogs,
            "gross_profit__1004": revenue - cogs,
            "operating_income": 20.0 + index,
            "net_income_total": 15.0 + index,
            "eps_diluted": 1.0 + index / 10.0,
            "eps_basic__1034": 1.1 + index / 10.0,
            "cash_flow_from_operations": 25.0 + index,
            "capex__1305": -10.0 - index / 10.0,
            "r_and_d_expense": 5.0 + index / 10.0,
            "ebitda_standardised": 30.0 + index,
        }
        for code, value in values.items():
            _insert(store, security_id, code, "quarterly", period_end, value, _available(period_end))
        for code, value in {
            "common_equity": 200.0 + index * 10.0,
            "accounts_receivable": 20.0 + index,
            "inventory": 30.0 + index,
            "accounts_payable": 15.0 + index,
        }.items():
            _insert(store, security_id, code, "instant", period_end, value, _available(period_end, 50))


def test_core_metric_breadth_preserves_the_prior_catalog_and_declares_the_new_families():
    definitions = {definition.metric_code: definition for definition in default_derived_definitions()}
    expected = {
        *(f"{series}_q_growth_yoy" for series in ("revenue", "gross_profit", "operating_income", "net_income", "eps_diluted", "cfo", "fcf", "capex", "rd_expense")),
        *(f"{series}_q_growth_qoq" for series in ("revenue", "gross_profit", "operating_income", "net_income", "eps_diluted", "cfo", "fcf", "capex", "rd_expense")),
        "gross_profit_cagr_3y", "operating_income_cagr_3y", "ebitda_cagr_3y", "fcf_cagr_3y", "common_equity_cagr_3y",
        "eps_basic_ttm", "dso_days", "dio_days", "dpo_days", "cash_conversion_cycle",
    }
    assert len(definitions) == 201
    assert expected <= definitions.keys()
    assert definitions["dso_days"].expression == "safe_div(receivables_avg2 * 365, max(revenue_ttm, 0))"
    reclaimed = {"dso_days", "dio_days", "dpo_days", "cash_conversion_cycle"}
    assert reclaimed <= RECLAIMED_ITEM_CODES
    assert reclaimed <= known_item_codes()


def test_core_quarterly_growth_cagrs_basic_eps_and_cash_cycle_are_pit_derived(tmp_store):
    seed_derived_metric_definitions(tmp_store)
    _seed_core_series(tmp_store)
    requested = (
        "revenue_q_growth_yoy", "revenue_q_growth_qoq", "gross_profit_cagr_3y",
        "operating_income_cagr_3y", "ebitda_cagr_3y", "fcf_cagr_3y", "common_equity_cagr_3y",
        "eps_basic_ttm", "dso_days", "dio_days", "dpo_days", "cash_conversion_cycle",
    )
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions(metric_codes=requested))

    yoy, yoy_available = _value(tmp_store, "revenue_q_growth_yoy", _QUARTERS[4])
    qoq, _ = _value(tmp_store, "revenue_q_growth_qoq", _QUARTERS[4])
    assert yoy == pytest.approx((140.0 - 100.0) / 100.0)
    assert qoq == pytest.approx((140.0 - 130.0) / 130.0)
    assert yoy_available == _available(_QUARTERS[4])

    gross_ttm_now = sum((100.0 + 10.0 * index) - (60.0 + 2.0 * index) for index in range(12, 16))
    gross_ttm_then = sum((100.0 + 10.0 * index) - (60.0 + 2.0 * index) for index in range(0, 4))
    gross_cagr, _ = _value(tmp_store, "gross_profit_cagr_3y", _QUARTERS[15])
    assert gross_cagr == pytest.approx((gross_ttm_now / gross_ttm_then) ** (1.0 / 3.0) - 1.0)
    eps_basic, _ = _value(tmp_store, "eps_basic_ttm", _QUARTERS[3])
    assert eps_basic == pytest.approx(sum(1.1 + index / 10.0 for index in range(4)))

    dso, dso_available = _value(tmp_store, "dso_days", _QUARTERS[4])
    dio, _ = _value(tmp_store, "dio_days", _QUARTERS[4])
    dpo, _ = _value(tmp_store, "dpo_days", _QUARTERS[4])
    cash_cycle, _ = _value(tmp_store, "cash_conversion_cycle", _QUARTERS[4])
    # At _QUARTERS[4], each trailing-flow denominator uses quarters 1..4.
    assert dso == pytest.approx(((20.0 + 24.0) / 2.0) * 365.0 / (110.0 + 120.0 + 130.0 + 140.0))
    assert dio == pytest.approx(((30.0 + 34.0) / 2.0) * 365.0 / (62.0 + 64.0 + 66.0 + 68.0))
    assert dpo == pytest.approx(((15.0 + 19.0) / 2.0) * 365.0 / (62.0 + 64.0 + 66.0 + 68.0))
    assert cash_cycle == pytest.approx(dso + dio - dpo)
    assert dso_available == _available(_QUARTERS[4], 50)


def test_quarterly_growth_and_cash_cycle_have_no_numeric_value_for_missing_or_nonpositive_bases(tmp_store):
    seed_derived_metric_definitions(tmp_store)
    _seed_core_series(tmp_store)
    tmp_store.con.execute(
        "DELETE FROM fundamental_standardized WHERE security_id = 'S1' AND canonical_code = 'revenue' AND period_end = ?",
        [_QUARTERS[0]],
    )
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions(metric_codes=("revenue_q_growth_yoy",)))
    missing_quarter = _latest_state(tmp_store, "revenue_q_growth_yoy", _QUARTERS[4])
    assert missing_quarter == (None, "missing_input_or_domain", _available(_QUARTERS[4]))

    _seed_core_series(tmp_store, "S2", negative_bases=True)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions(metric_codes=("dso_days", "dio_days", "dpo_days", "cash_conversion_cycle")))
    for code in ("dso_days", "dio_days", "dpo_days", "cash_conversion_cycle"):
        value, value_status, available_at = _latest_state(tmp_store, code, _QUARTERS[4], "S2")
        assert value is None
        assert value_status != "valid"
        assert available_at is not None


def test_cagr_has_no_numeric_value_when_an_endpoint_is_nonpositive(tmp_store):
    seed_derived_metric_definitions(tmp_store)
    _seed_core_series(tmp_store)
    tmp_store.con.execute(
        "UPDATE fundamental_standardized SET value = 0 WHERE security_id = 'S1' AND canonical_code = 'common_equity' AND period_end = ?",
        [_QUARTERS[3]],
    )
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions(metric_codes=("common_equity_cagr_3y",)))
    nonpositive_endpoint = _latest_state(tmp_store, "common_equity_cagr_3y", _QUARTERS[15])
    assert nonpositive_endpoint == (
        None,
        "missing_input_or_domain",
        _available(_QUARTERS[15], 50),
    )
