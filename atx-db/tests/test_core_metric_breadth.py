"""Focused catalog coverage for core quarterly growth and efficiency metrics."""

from __future__ import annotations

import datetime as dt

import duckdb
import pytest

from atx_db import _derived_annual as annual
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


_QUARTERLY_MARGIN_AND_ACCELERATION = (
    "gross_margin_q", "operating_margin_q", "net_margin_q",
    "gross_margin_q_change_yoy", "operating_margin_q_change_yoy", "net_margin_q_change_yoy",
    "eps_basic_q_growth_yoy", "eps_basic_q_growth_qoq",
    "eps_diluted_q_growth_yoy_accel", "revenue_q_growth_yoy_accel",
    "gross_margin_q_change_yoy_accel", "operating_margin_q_change_yoy_accel",
)

# A 52/53-week retailer-style calendar: fiscal quarters end on Saturdays, and
# the 53-week fiscal 2020 closes with a 14-week quarter on 2021-01-02.
_RETAIL_QUARTERS = (
    dt.date(2019, 3, 30), dt.date(2019, 6, 29), dt.date(2019, 9, 28), dt.date(2019, 12, 28),
    dt.date(2020, 3, 28), dt.date(2020, 6, 27), dt.date(2020, 9, 26), dt.date(2021, 1, 2),
    dt.date(2021, 4, 3), dt.date(2021, 7, 3), dt.date(2021, 10, 2), dt.date(2022, 1, 1),
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
        *_QUARTERLY_MARGIN_AND_ACCELERATION,
    }
    assert len(definitions) == 256
    assert expected <= definitions.keys()
    # Single-quarter margin levels, changes, basic-EPS growth and accelerations
    # live on the quarter grid; the existing *_margin_change_yoy stay trailing.
    assert {definitions[code].window for code in _QUARTERLY_MARGIN_AND_ACCELERATION} == {"q"}
    for margin in ("gross", "operating", "net"):
        assert definitions[f"{margin}_margin_change_yoy"].window == "ttm"
        assert definitions[f"{margin}_margin_q_change_yoy"].expression == (
            f"{margin}_margin_q - lag({margin}_margin_q, 4)"
        )
    assert definitions["eps_basic_q_growth_yoy"].item_inputs == ("eps_basic__1034",)
    assert definitions["revenue_q_growth_yoy_accel"].expression == (
        "revenue_q_growth_yoy - lag(revenue_q_growth_yoy, 1)"
    )
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


# Seasonal single-quarter statements (Q4 spike) so single-quarter and trailing
# margin changes diverge. Index i is _QUARTERS[i] or _RETAIL_QUARTERS[i].
_STATEMENT = {
    "revenue": (100.0, 110.0, 120.0, 200.0, 120.0, 130.0, 140.0, 260.0, 150.0, 160.0, 170.0, 300.0),
    "gross_profit__1004": (40.0, 45.0, 50.0, 100.0, 50.0, 52.0, 60.0, 150.0, 63.0, 64.0, 75.0, 180.0),
    "operating_income": (10.0, 12.0, 14.0, 40.0, 12.0, 13.0, 16.0, 70.0, 18.0, 17.0, 20.0, 90.0),
    "net_income_total": (7.0, 8.0, 10.0, 30.0, 8.0, 9.0, 11.0, 50.0, 12.0, 11.0, 14.0, 66.0),
    "eps_diluted": (0.5, 0.6, 0.7, 1.5, 0.4, 0.7, 0.9, 2.5, 0.6, 0.6, 1.0, 3.2),
    # Loss-to-profit (-1 -> 2) at index 4; a zero base at index 1 feeds index 5.
    "eps_basic__1034": (-1.0, 0.0, 0.8, 1.6, 2.0, 0.9, 1.0, 2.6, 2.5, 0.7, 1.1, 3.3),
}


def _seed_statement(store, security_id: str, quarters: tuple[dt.date, ...],
                    starts: dict[int, dt.date] | None = None) -> None:
    """Quarterly duration facts with contiguous fiscal spans, as SEC quarters carry.

    ``starts`` overrides individual period starts to model gaps between quarters.
    """
    for index, period_end in enumerate(quarters):
        period_start = quarters[index - 1] + dt.timedelta(days=1) if index else period_end - dt.timedelta(days=90)
        period_start = (starts or {}).get(index, period_start)
        available_at = _available(period_end)
        for code, series in _STATEMENT.items():
            store.con.execute(
                """
                INSERT INTO fundamental_standardized (
                    standardized_id, source, security_id, item_id, canonical_code, basis,
                    period_start, period_end, value, as_of_date, available_at, input_codes_json,
                    input_item_ids_json, rule_id, combination_rule, revision_sequence,
                    is_latest_revision
                ) VALUES (?, 'test', ?, 1, ?, 'quarterly', ?, ?, ?, ?, ?, '[]', '[]', 'r', 'direct', 1, true)
                """,
                [f"{security_id}|{code}|{period_end}", security_id, code, period_start, period_end,
                 series[index], available_at.date(), available_at],
            )


def _ratio(numerator: str, index: int) -> float:
    return _STATEMENT[numerator][index] / _STATEMENT["revenue"][index]


def _growth(code: str, index: int, periods: int = 4) -> float:
    base = _STATEMENT[code][index - periods]
    return (_STATEMENT[code][index] - base) / abs(base)


def _ttm_margin(numerator: str, index: int) -> float:
    window = range(index - 3, index + 1)
    return sum(_STATEMENT[numerator][i] for i in window) / sum(_STATEMENT["revenue"][i] for i in window)


def _refresh_quarterly_family(store) -> None:
    seed_derived_metric_definitions(store)
    refresh_derived_metrics(store, DerivedMetricsOptions(
        metric_codes=(*_QUARTERLY_MARGIN_AND_ACCELERATION, "gross_margin_change_yoy",
                      "revenue_q_growth_qoq", "revenue_growth_qoq", "eps_diluted_q_growth_qoq"),
    ))


def _origin(store, code: str, period_end: dt.date, security_id: str = "S1") -> str:
    return store.con.execute(
        """SELECT value_origin FROM derived_metric_values
           WHERE security_id = ? AND metric_code = ? AND period_end = ?
           ORDER BY available_at DESC, derived_value_id DESC LIMIT 1""",
        [security_id, code, period_end],
    ).fetchone()[0]


def test_single_quarter_margins_and_their_yoy_changes_differ_from_trailing_margin_changes(tmp_store):
    _seed_statement(tmp_store, "S1", _QUARTERS[:12])
    _refresh_quarterly_family(tmp_store)

    for level, numerator in (("gross_margin_q", "gross_profit__1004"), ("operating_margin_q", "operating_income"),
                             ("net_margin_q", "net_income_total")):
        assert _latest_state(tmp_store, level, _QUARTERS[8]) == (
            pytest.approx(_ratio(numerator, 8)), "valid", _available(_QUARTERS[8]))
        change, status, _ = _latest_state(tmp_store, f"{level}_change_yoy", _QUARTERS[8])
        # Fraction points against the same fiscal quarter one year earlier.
        assert (change, status) == (pytest.approx(_ratio(numerator, 8) - _ratio(numerator, 4)), "valid")

    quarterly_change, _, _ = _latest_state(tmp_store, "gross_margin_q_change_yoy", _QUARTERS[11])
    trailing_change, _, _ = _latest_state(tmp_store, "gross_margin_change_yoy", _QUARTERS[11])
    assert quarterly_change == pytest.approx(_ratio("gross_profit__1004", 11) - _ratio("gross_profit__1004", 7))
    assert trailing_change == pytest.approx(
        _ttm_margin("gross_profit__1004", 11) - _ttm_margin("gross_profit__1004", 7))
    # 180/300 - 150/260 = +0.0231 versus a trailing 382/780 - 312/650 = +0.0097: not interchangeable.
    assert abs(quarterly_change - trailing_change) > 0.01


def test_basic_eps_growth_uses_absolute_base_and_has_no_value_for_zero_or_missing_bases(tmp_store):
    _seed_statement(tmp_store, "S1", _QUARTERS[:12])
    _seed_statement(tmp_store, "S2", _QUARTERS[:12])
    tmp_store.con.execute(
        "DELETE FROM fundamental_standardized WHERE security_id = 'S2' AND canonical_code = 'eps_basic__1034' "
        "AND period_end = ?", [_QUARTERS[6]],
    )
    _refresh_quarterly_family(tmp_store)

    # -1 -> 2 is (2 - (-1)) / |-1| = +3.0, from basic (not diluted) EPS.
    assert _latest_state(tmp_store, "eps_basic_q_growth_yoy", _QUARTERS[4]) == (
        pytest.approx(3.0), "valid", _available(_QUARTERS[4]))
    qoq, status, _ = _latest_state(tmp_store, "eps_basic_q_growth_qoq", _QUARTERS[4])
    assert (qoq, status) == (pytest.approx((2.0 - 1.6) / 1.6), "valid")
    assert _latest_state(tmp_store, "eps_basic_q_growth_yoy", _QUARTERS[5]) == (
        None, "missing_input_or_domain", _available(_QUARTERS[5]))
    # S2's basic-EPS bucket for _QUARTERS[6] is absent: growth four buckets later
    # has no value rather than a row-lag comparison with _QUARTERS[5].
    assert _latest_state(tmp_store, "eps_basic_q_growth_yoy", _QUARTERS[10], "S2")[:2] == (
        None, "missing_input_or_domain")
    assert _latest_state(tmp_store, "eps_basic_q_growth_yoy", _QUARTERS[10])[:2] == (
        pytest.approx(_growth("eps_basic__1034", 10)), "valid")


def test_acceleration_is_exactly_the_bucket_growth_less_the_prior_bucket_growth(tmp_store):
    _seed_statement(tmp_store, "S1", _QUARTERS[:12])
    _seed_statement(tmp_store, "S2", _QUARTERS[:12])
    tmp_store.con.execute(
        "DELETE FROM fundamental_standardized WHERE security_id = 'S2' AND canonical_code = 'revenue' "
        "AND period_end = ?", [_QUARTERS[6]],
    )
    tmp_store.con.execute(
        "UPDATE fundamental_standardized SET value = 0 WHERE security_id = 'S2' AND canonical_code = 'revenue' "
        "AND period_end = ?", [_QUARTERS[3]],
    )
    _refresh_quarterly_family(tmp_store)

    pairs = (
        ("revenue_q_growth_yoy_accel", "revenue_q_growth_yoy"),
        ("eps_diluted_q_growth_yoy_accel", "eps_diluted_q_growth_yoy"),
        ("gross_margin_q_change_yoy_accel", "gross_margin_q_change_yoy"),
        ("operating_margin_q_change_yoy_accel", "operating_margin_q_change_yoy"),
    )
    for index in (9, 10, 11):
        for accel_code, growth_code in pairs:
            current, current_status, _ = _latest_state(tmp_store, growth_code, _QUARTERS[index])
            prior, prior_status, _ = _latest_state(tmp_store, growth_code, _QUARTERS[index - 1])
            accel, accel_status, accel_available = _latest_state(tmp_store, accel_code, _QUARTERS[index])
            assert (current_status, prior_status, accel_status) == ("valid", "valid", "valid")
            assert accel == current - prior
            assert accel_available == _available(_QUARTERS[index])
    assert _latest_state(tmp_store, "revenue_q_growth_yoy_accel", _QUARTERS[11])[0] == pytest.approx(
        _growth("revenue", 11) - _growth("revenue", 10))

    # S2 lacks revenue at _QUARTERS[6], so growth at _QUARTERS[10] has no base and
    # acceleration at _QUARTERS[11] has no value: it never reaches back to _QUARTERS[9].
    assert _latest_state(tmp_store, "revenue_q_growth_yoy", _QUARTERS[11], "S2")[:2] == (
        pytest.approx(_growth("revenue", 11)), "valid")
    assert _latest_state(tmp_store, "revenue_q_growth_yoy", _QUARTERS[10], "S2")[:2] == (
        None, "missing_input_or_domain")
    assert _latest_state(tmp_store, "revenue_q_growth_yoy_accel", _QUARTERS[11], "S2")[:2] == (
        None, "missing_input_or_domain")
    assert _origin(tmp_store, "revenue_q_growth_yoy_accel", _QUARTERS[11], "S2") == "unavailable"
    assert _latest_state(tmp_store, "gross_margin_q_change_yoy", _QUARTERS[10], "S2")[:2] == (
        None, "missing_input_or_domain")
    # Zero quarterly revenue has no margin, so the next year's margin change has no value either.
    assert _latest_state(tmp_store, "gross_margin_q", _QUARTERS[3], "S2")[:2] == (None, "zero_denominator")
    assert _latest_state(tmp_store, "gross_margin_q", _QUARTERS[7], "S2")[1] == "valid"
    assert _latest_state(tmp_store, "gross_margin_q_change_yoy", _QUARTERS[7], "S2")[:2] == (
        None, "missing_input_or_domain")


def test_fifty_three_week_year_compares_the_same_fiscal_quarter_bucket(tmp_store):
    _seed_statement(tmp_store, "S3", _RETAIL_QUARTERS)
    _refresh_quarterly_family(tmp_store)

    # The 14-week quarter ending 2021-01-02 compares with the quarter ending
    # 2019-12-28, and the next fiscal Q1 (2021-04-03) with 2020-03-28.
    for index in (7, 8):
        value, status, _ = _latest_state(tmp_store, "eps_basic_q_growth_yoy", _RETAIL_QUARTERS[index], "S3")
        assert (value, status) == (pytest.approx(_growth("eps_basic__1034", index)), "valid")
        accel, status, _ = _latest_state(tmp_store, "revenue_q_growth_yoy_accel", _RETAIL_QUARTERS[index], "S3")
        assert (accel, status) == (
            pytest.approx(_growth("revenue", index) - _growth("revenue", index - 1)), "valid")
        # The contiguous 98-day quarter is a proven adjacent fiscal quarter.
        assert _origin(tmp_store, "revenue_q_growth_yoy_accel", _RETAIL_QUARTERS[index], "S3") == "quarterly"
    change, status, _ = _latest_state(tmp_store, "gross_margin_q_change_yoy", _RETAIL_QUARTERS[11], "S3")
    assert (change, status) == (
        pytest.approx(_ratio("gross_profit__1004", 11) - _ratio("gross_profit__1004", 7)), "valid")


_ONE_QUARTER_APART = (
    "revenue_q_growth_qoq", "revenue_q_growth_yoy_accel",
    "eps_diluted_q_growth_yoy_accel", "gross_margin_q_change_yoy_accel", "operating_margin_q_change_yoy_accel",
)
# Share-count-denominated one-quarter comparisons: a 10-Q never restates the
# preceding quarter, so span adjacency cannot prove a common share basis.
_PER_SHARE_QOQ = ("eps_basic_q_growth_qoq", "eps_diluted_q_growth_qoq")


def test_one_quarter_comparisons_are_quarterly_only_when_fiscal_spans_prove_adjacency(tmp_store):
    _seed_statement(tmp_store, "S1", _QUARTERS[:12])
    # _QUARTERS[9] starts after a 7-day gap (tolerated) in S4 and an 8-day gap in S5.
    _seed_statement(tmp_store, "S4", _QUARTERS[:12], starts={9: _QUARTERS[8] + dt.timedelta(days=8)})
    _seed_statement(tmp_store, "S5", _QUARTERS[:12], starts={9: _QUARTERS[8] + dt.timedelta(days=9)})
    # Fiscal-year change: contiguous quarters with a 66-day stub from 2021-04-11 to 2021-06-15.
    stub_calendar = (*_QUARTERS[:8], dt.date(2021, 4, 10), dt.date(2021, 6, 15), *_QUARTERS[10:12])
    _seed_statement(tmp_store, "S6", stub_calendar)
    _refresh_quarterly_family(tmp_store)

    for index in (9, 10, 11):
        for code in _ONE_QUARTER_APART:
            assert _latest_state(tmp_store, code, _QUARTERS[index])[1] == "valid", code
            assert _origin(tmp_store, code, _QUARTERS[index]) == "quarterly", code
            assert _origin(tmp_store, code, _QUARTERS[index], "S4") == "quarterly", code
        for code in _PER_SHARE_QOQ:
            assert _latest_state(tmp_store, code, _QUARTERS[index])[1] == "valid", code
            assert _origin(tmp_store, code, _QUARTERS[index]) == "incomparable", code
    for code in ("eps_basic_q_growth_yoy", "gross_margin_q_change_yoy", "revenue_q_growth_yoy"):
        assert _origin(tmp_store, code, _QUARTERS[9], "S5") == "quarterly", code
    # Overlapping trailing windows are never adjacent quarters.
    assert _latest_state(tmp_store, "revenue_growth_qoq", _QUARTERS[11])[1] == "valid"
    assert _origin(tmp_store, "revenue_growth_qoq", _QUARTERS[11]) == "incomparable"

    for code in _ONE_QUARTER_APART:
        # Unproven adjacency keeps the arithmetic but labels it incomparable.
        assert _latest_state(tmp_store, code, _QUARTERS[9], "S5")[1] == "valid", code
        assert _origin(tmp_store, code, _QUARTERS[9], "S5") == "incomparable", code
        assert _origin(tmp_store, code, _QUARTERS[10], "S5") == "quarterly", code
        for period_end in stub_calendar[9:11]:
            assert _origin(tmp_store, code, period_end, "S6") == "incomparable", (code, period_end)
        assert _origin(tmp_store, code, stub_calendar[11], "S6") == "quarterly", code


def test_per_share_qoq_across_a_reverse_split_is_never_labeled_comparable(tmp_store):
    # 1:10 reverse split between the second and third quarters: the third 10-Q
    # does not restate the second quarter, so its unadjusted EPS base is pre-split.
    quarters = _QUARTERS[:3]
    for index, (eps, revenue) in enumerate(((-0.04, 100.0), (-0.05, 110.0), (-0.50, 121.0))):
        period_start = quarters[index - 1] + dt.timedelta(days=1) if index else dt.date(2019, 1, 1)
        available_at = _available(quarters[index])
        for code, value in (("eps_diluted", eps), ("eps_basic__1034", eps), ("revenue", revenue)):
            tmp_store.con.execute(
                """
                INSERT INTO fundamental_standardized (
                    standardized_id, source, security_id, item_id, canonical_code, basis,
                    period_start, period_end, value, as_of_date, available_at, input_codes_json,
                    input_item_ids_json, rule_id, combination_rule, revision_sequence,
                    is_latest_revision
                ) VALUES (?, 'test', 'S7', 1, ?, 'quarterly', ?, ?, ?, ?, ?, '[]', '[]', 'r', 'direct', 1, true)
                """,
                [f"S7|{code}|{quarters[index]}", code, period_start, quarters[index], value,
                 available_at.date(), available_at],
            )
    seed_derived_metric_definitions(tmp_store)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions(
        metric_codes=(*_PER_SHARE_QOQ, "revenue_q_growth_qoq")))

    for code in _PER_SHARE_QOQ:
        # The arithmetic is published (-9.0), but never as a comparable quarterly pair.
        assert _latest_state(tmp_store, code, quarters[2], "S7")[:2] == (pytest.approx(-9.0), "valid")
        assert _origin(tmp_store, code, quarters[2], "S7") == "incomparable"
    assert _origin(tmp_store, "revenue_q_growth_qoq", quarters[2], "S7") == "quarterly"


def _quarter(start: dt.date | None, end: dt.date) -> tuple[dt.date | None, dt.date]:
    return start, end


def _days(start: dt.date, days: int) -> tuple[dt.date, dt.date]:
    return start, start + dt.timedelta(days=days - 1)


_Q1_2024 = _days(dt.date(2024, 1, 1), 91)


@pytest.mark.parametrize(("older", "newer", "comparable"), [
    (_Q1_2024, _days(dt.date(2024, 4, 1), 91), True),
    (_days(dt.date(2024, 1, 1), 70), _days(dt.date(2024, 3, 11), 70), True),
    (_Q1_2024, _days(dt.date(2024, 4, 1), 69), False),
    (_days(dt.date(2024, 1, 1), 120), _days(dt.date(2024, 4, 30), 120), True),
    (_Q1_2024, _days(dt.date(2024, 4, 1), 121), False),
    (_days(dt.date(2024, 1, 1), 69), _days(dt.date(2024, 3, 10), 91), False),
    (_Q1_2024, _days(dt.date(2024, 3, 31), 91), False),  # starts on the prior quarter's end
    (_Q1_2024, _days(dt.date(2024, 3, 15), 91), False),  # overlap
    (_Q1_2024, _days(dt.date(2024, 4, 8), 91), True),  # 7-day gap
    (_Q1_2024, _days(dt.date(2024, 4, 9), 91), False),  # 8-day gap
    (_Q1_2024, _quarter(None, dt.date(2024, 6, 30)), False),  # no fiscal start
])
def test_one_bucket_adjacency_limits(older, newer, comparable):
    assert _one_bucket_apart_comparable(older, newer) is comparable


def test_one_bucket_adjacency_never_certifies_a_per_share_pair():
    newer = _days(dt.date(2024, 4, 1), 91)
    assert _one_bucket_apart_comparable(_Q1_2024, newer) is True
    assert _one_bucket_apart_comparable(_Q1_2024, newer, share_basis=True) is False
    basis = annual.share_basis_codes()
    assert {"eps_diluted", "eps_basic__1034", "eps_diluted_ttm", "eps_basic_ttm", "eps_ttm"} <= basis
    # Growth rates and margins are basis-free: accelerations keep their span proof.
    assert not {"eps_diluted_q_growth_yoy", "revenue_q_growth_yoy", "gross_margin_q_change_yoy",
                "revenue", "gross_margin_q"} & basis


def _one_bucket_apart_comparable(older, newer, *, share_basis: bool = False) -> bool:
    def span(dates, offset):
        start, end = (f"DATE '{value}'" if value is not None else "NULL::DATE" for value in dates)
        return annual.Span(start, end, "false", "true", offset, share_basis and offset == 0)

    combined = annual._combine(span(newer, 0), span(older, 1))
    with duckdb.connect(config={"memory_limit": "64MB", "threads": 1}) as con:
        return con.execute(f"SELECT {combined.coherent}").fetchone()[0]
