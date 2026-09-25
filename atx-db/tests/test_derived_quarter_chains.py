"""R1c: rolling quarterly windows and lagged balance-sheet instants prove fiscal adjacency.

A ``stdev_q`` window is comparable only when every consecutive pair of its states
is a proven fiscal-quarter step, and a flow divided by the prior quarter's
balance is comparable only when that balance is dated at the flow's start.
Share-basis operands (per-share values and share counts) are never proven.
"""

from __future__ import annotations

import datetime as dt
import statistics
from pathlib import Path
from types import SimpleNamespace

import duckdb
import pytest

from atx_db import _derived_annual as annual
from atx_db import derived_metrics as engine
from atx_db.derived_registry import DerivedMetricDefinition
from atx_db.item_registry import read_fundamental_item_seed


def _definition(code: str, expression: str, inputs: tuple[str, ...], window: str = "q") -> DerivedMetricDefinition:
    return DerivedMetricDefinition(code, "growth", expression, window, inputs, False, f"R1c probe {code}.", "1")


CHAIN_CATALOG = (
    _definition("ni_change_t", "net_income_total - lag(net_income_total, 4)", ("item:net_income_total",)),
    _definition("sue_t", "safe_div(ni_change_t, stdev_q(ni_change_t, 4))", ("metric:ni_change_t",)),
    _definition("ni_vol_t", "stdev_q(net_income_total, 4)", ("item:net_income_total",)),
    _definition("eps_vol_t", "stdev_q(eps_diluted, 4)", ("item:eps_diluted",)),
    _definition("rev_ttm_t", "ttm(revenue)", ("item:revenue",), "ttm"),
    _definition("rev_ttm_vol_t", "stdev_q(rev_ttm_t, 4)", ("metric:rev_ttm_t",), "ttm"),
    _definition("roe_q_t", "safe_div(net_income_total, lag(common_equity, 1))",
                ("item:net_income_total", "item:common_equity")),
    _definition("roe_avg_q_t", "safe_div(net_income_total, (common_equity + lag(common_equity, 1)) / 2)",
                ("item:net_income_total", "item:common_equity")),
    _definition("equity_qoq_t", "qoq(common_equity)", ("item:common_equity",)),
    _definition("shares_qoq_t", "qoq(shares_outstanding_period_end)", ("item:shares_outstanding_period_end",)),
    # R1b's SUE form: the current change over the prior quarter's rolling volatility.
    _definition("sd4_t", "stdev_q(ni_change_t, 4)", ("metric:ni_change_t",)),
    _definition("sue_lag_t", "safe_div(ni_change_t, lag(sd4_t, 1))", ("metric:ni_change_t", "metric:sd4_t")),
    # A per-share seasonal difference outside the per_share family, and its window.
    _definition("eps_change_t", "eps_diluted - lag(eps_diluted, 4)", ("item:eps_diluted",)),
    _definition("eps_sd4_t", "stdev_q(eps_change_t, 4)", ("metric:eps_change_t",)),
    # Split-sensitive comparisons across periods.
    _definition("eps_yoy_t", "yoy(eps_diluted)", ("item:eps_diluted",)),
    _definition("wshares_yoy_t", "yoy(weighted_avg_shares_diluted)", ("item:weighted_avg_shares_diluted",)),
    _definition("shares_yoy_t", "yoy(shares_outstanding_period_end)", ("item:shares_outstanding_period_end",)),
    _definition("share_issuance_2y_t", "ln(safe_div(shares_outstanding_period_end, "
                "lag(shares_outstanding_period_end, 8)))", ("item:shares_outstanding_period_end",)),
    _definition("eps_cagr_2y_t", "cagr(eps_diluted, 2)", ("item:eps_diluted",)),
)

QUARTERS = tuple(
    dt.date(year, month, day)
    for year in (2019, 2020, 2021)
    for month, day in ((3, 31), (6, 30), (9, 30), (12, 31))
)
NET_INCOME = (7.0, 8.0, 10.0, 30.0, 8.0, 9.0, 11.0, 50.0, 12.0, 11.0, 14.0, 66.0)
EPS = (0.5, 0.6, 0.7, 1.5, 0.4, 0.7, 0.9, 2.5, 0.6, 0.6, 1.0, 3.2)
REVENUE = (100.0, 110.0, 120.0, 200.0, 120.0, 130.0, 140.0, 260.0, 150.0, 160.0, 170.0, 300.0)
EQUITY = tuple(100.0 + 10.0 * index for index in range(12))
SHARES = tuple(50.0 + index for index in range(12))


@pytest.fixture
def store(monkeypatch):
    con = duckdb.connect(":memory:")
    con.execute("SET memory_limit='256MB'")
    con.execute("SET threads=1")
    con.execute("""
        CREATE TABLE fundamental_standardized (
            standardized_id VARCHAR PRIMARY KEY, source VARCHAR, security_id VARCHAR,
            cik VARCHAR, item_id INTEGER, canonical_code VARCHAR, basis VARCHAR,
            period_start DATE, period_end DATE, value DOUBLE, as_of_date DATE,
            available_at TIMESTAMP, input_codes_json VARCHAR, input_item_ids_json VARCHAR,
            rule_id VARCHAR, combination_rule VARCHAR, revision_sequence INTEGER,
            is_latest_revision BOOLEAN
        )
    """)
    con.execute("""
        CREATE TABLE derived_metric_values (
            derived_value_id VARCHAR PRIMARY KEY, source VARCHAR, security_id VARCHAR,
            metric_code VARCHAR, metric_window VARCHAR, period_end DATE, value DOUBLE,
            available_at TIMESTAMP, inputs_hash VARCHAR, as_of_date DATE,
            is_latest_revision BOOLEAN, run_id VARCHAR, value_status VARCHAR,
            revision_group_id VARCHAR, revision_sequence INTEGER, revision_count INTEGER,
            valid_to TIMESTAMP, target_bucket BIGINT, definition_hash VARCHAR,
            arithmetic_available_at TIMESTAMP, history_status VARCHAR,
            value_origin VARCHAR, fiscal_period_start DATE, fiscal_period_end DATE,
            selected_input_refs_json VARCHAR, selected_input_refs_hash VARCHAR
        )
    """)
    warehouse = SimpleNamespace(con=con, path=Path(":memory:"), initialize=lambda: None)
    monkeypatch.setattr(engine, "default_derived_definitions", lambda: CHAIN_CATALOG)
    try:
        yield warehouse
    finally:
        con.close()


def _insert(store, security_id, code, basis, start, end, value):
    available_at = dt.datetime.combine(end + dt.timedelta(days=40), dt.time(21))
    store.con.execute("""
        INSERT INTO fundamental_standardized (
            standardized_id, source, security_id, cik, item_id, canonical_code, basis,
            period_start, period_end, value, as_of_date, available_at,
            input_codes_json, input_item_ids_json, rule_id, combination_rule,
            revision_sequence, is_latest_revision
        ) VALUES (?, 'test', ?, '0000001234', 1, ?, ?, ?, ?, ?, ?, ?, '[]', '[]', 'r', 'direct', 1, true)
    """, [f"{security_id}|{code}|{end}", security_id, code, basis, start, end, value,
          available_at.date(), available_at])


def _seed(store, security_id: str, starts: dict[int, dt.date] | None = None) -> None:
    """Contiguous quarterly flows and quarter-end balance-sheet instants."""
    for index, end in enumerate(QUARTERS):
        start = QUARTERS[index - 1] + dt.timedelta(days=1) if index else end - dt.timedelta(days=90)
        start = (starts or {}).get(index, start)
        for code, series in (("net_income_total", NET_INCOME), ("eps_diluted", EPS), ("revenue", REVENUE),
                             ("weighted_avg_shares_diluted", SHARES)):
            _insert(store, security_id, code, "quarterly", start, end, series[index])
        for code, series in (("common_equity", EQUITY), ("shares_outstanding_period_end", SHARES)):
            _insert(store, security_id, code, "instant", None, end, series[index])


def _state(store, security_id: str, code: str, index: int):
    return store.con.execute("""
        SELECT value, value_status, value_origin FROM derived_metric_values
        WHERE security_id = ? AND metric_code = ? AND period_end = ?
        ORDER BY available_at DESC, derived_value_id DESC LIMIT 1
    """, [security_id, code, QUARTERS[index]]).fetchone()


def _change(index: int) -> float:
    return NET_INCOME[index] - NET_INCOME[index - 4]


def test_share_basis_codes_cover_per_share_values_and_every_share_count():
    basis = annual.share_basis_codes()
    share_counts = {row.canonical_code for row in read_fundamental_item_seed()
                    if row.unit_type == "quantity" and "shares" in row.canonical_code}
    # Drift guard: every registered share-count item is named explicitly.
    assert share_counts == annual.SHARE_COUNT_CODES
    assert annual.SHARE_COUNT_CODES | {"eps_diluted", "eps_basic__1034", "eps_diluted_ttm", "eps_ttm"} <= basis
    assert not {"net_income_total", "common_equity", "total_assets", "revenue",
                "eps_diluted_q_growth_yoy", "shares_growth_yoy"} & basis


def test_split_sensitivity_follows_formula_operands_not_the_family():
    by_code = {definition.metric_code: definition for definition in CHAIN_CATALOG}
    exponents = dict(annual.plan_for(by_code["eps_sd4_t"], by_code).share_exponents)
    # A growth-family per-share difference is still a per-share value.
    assert exponents == {"eps_change_t": -1}
    assert dict(annual.plan_for(by_code["sue_lag_t"], by_code).share_exponents) == {
        "ni_change_t": 0, "sd4_t": 0}
    unit = {"eps_diluted": -1, "weighted_avg_shares_diluted": 1, "net_income_total": 0}.get

    def exponent(expression):
        return annual.share_exponent(annual.parse_expression(expression), unit)

    assert exponent("safe_div(net_income_total, weighted_avg_shares_diluted)") == -1
    assert exponent("eps_diluted * weighted_avg_shares_diluted") == 0
    assert exponent("yoy(eps_diluted)") == 0
    assert exponent("stdev_q(eps_diluted, 4)") == -1
    assert exponent("eps_diluted + net_income_total") is None


def test_rolling_standard_deviation_is_quarterly_only_across_a_proven_quarter_chain(store):
    _seed(store, "S1")
    # S4: the quarter ending 2021-06-30 starts after a 7-day gap (tolerated).
    _seed(store, "S4", {9: QUARTERS[8] + dt.timedelta(days=8)})
    # S5: the same quarter starts after an 8-day gap (unproven).
    _seed(store, "S5", {9: QUARTERS[8] + dt.timedelta(days=9)})
    engine.refresh_derived_metrics(store)

    for index in (7, 11):
        # SUE-style: net-income change over the rolling volatility of that change.
        expected = _change(index) / statistics.stdev(_change(i) for i in range(index - 3, index + 1))
        assert _state(store, "S1", "sue_t", index) == (pytest.approx(expected), "valid", "quarterly")
    assert _state(store, "S1", "ni_vol_t", 3) == (
        pytest.approx(statistics.stdev(NET_INCOME[:4])), "valid", "quarterly")
    # Minimum count: all four buckets must hold a state; three are not enough.
    assert _state(store, "S1", "sue_t", 6) == (None, "missing_input_or_domain", "unavailable")

    assert _state(store, "S4", "sue_t", 11)[1:] == ("valid", "quarterly")
    for index in (9, 10, 11):
        # Every window containing the unproven step is incomparable; values still publish.
        for code in ("sue_t", "ni_vol_t"):
            assert _state(store, "S5", code, index)[1:] == ("valid", "incomparable"), (code, index)
    assert _state(store, "S5", "sue_t", 8)[1:] == ("valid", "quarterly")

    # Per-share operands have no split guard, even inside a growth-family metric;
    # trailing windows are not single quarters.
    assert _state(store, "S1", "eps_vol_t", 11)[1:] == ("valid", "incomparable")
    assert _state(store, "S1", "eps_sd4_t", 11)[1:] == ("valid", "incomparable")
    assert _state(store, "S1", "rev_ttm_vol_t", 11)[1:] == ("valid", "incomparable")


def test_one_unproven_element_poisons_every_window_and_sue_that_contains_it(store):
    _seed(store, "S1")
    # S7: the Q5 flow starts 40 days early, so the Q9 seasonal change fails its own
    # year-over-year start check while the Q8..Q11 quarter chain is intact.
    _seed(store, "S7", {5: QUARTERS[4] - dt.timedelta(days=40)})
    engine.refresh_derived_metrics(store)

    for index in (8, 11):
        # R1b's form: the current change over the rolling volatility through t-1.
        expected = _change(index) / statistics.stdev(_change(i) for i in range(index - 4, index))
        assert _state(store, "S1", "sue_lag_t", index) == (pytest.approx(expected), "valid", "quarterly")
    assert _state(store, "S7", "ni_change_t", 9)[1:] == ("valid", "incomparable")
    for index in (9, 10, 11):
        assert _state(store, "S7", "sd4_t", index)[1:] == ("valid", "incomparable"), index
        assert _state(store, "S7", "sue_lag_t", index)[1:] == ("valid", "incomparable"), index


def test_flows_over_prior_quarter_balances_prove_the_balance_date(store):
    _seed(store, "S1")
    _seed(store, "S4", {9: QUARTERS[8] + dt.timedelta(days=8)})
    _seed(store, "S5", {9: QUARTERS[8] + dt.timedelta(days=9)})
    engine.refresh_derived_metrics(store)

    # HXZ-style quarterly ROE on one-quarter-lagged book equity.
    assert _state(store, "S1", "roe_q_t", 9) == (pytest.approx(NET_INCOME[9] / EQUITY[8]), "valid", "quarterly")
    assert _state(store, "S1", "roe_avg_q_t", 9) == (
        pytest.approx(NET_INCOME[9] / ((EQUITY[9] + EQUITY[8]) / 2)), "valid", "quarterly")
    assert _state(store, "S1", "roe_q_t", 0) == (None, "missing_input_or_domain", "unavailable")
    # Consecutive quarter-end balances are an instant chain.
    assert _state(store, "S1", "equity_qoq_t", 9) == (
        pytest.approx((EQUITY[9] - EQUITY[8]) / EQUITY[8]), "valid", "instant")
    # Share counts move with splits: never proven one quarter apart.
    assert _state(store, "S1", "shares_qoq_t", 9)[1:] == ("valid", "incomparable")

    assert _state(store, "S4", "roe_q_t", 9)[1:] == ("valid", "quarterly")
    # The balance dated 2021-03-31 is 9 days before a flow starting 2021-04-09.
    assert _state(store, "S5", "roe_q_t", 9)[1:] == ("valid", "incomparable")
    assert _state(store, "S5", "roe_q_t", 10)[1:] == ("valid", "quarterly")


@pytest.mark.parametrize(("balance_date", "lagged", "averaged"), [
    (dt.date(2021, 3, 31), "quarterly", "quarterly"),  # the day before the flow starts
    (dt.date(2021, 3, 28), "quarterly", "incomparable"),  # 3 days early: averaged form needs exact
    (dt.date(2021, 4, 5), "incomparable", "incomparable"),  # inside the flow period
    (dt.date(2021, 4, 21), "incomparable", "incomparable"),  # 20 days inside, 70 days before the next
])
def test_opening_balance_is_never_dated_inside_the_flow(store, balance_date, lagged, averaged):
    _seed(store, "S8")
    store.con.execute("""
        UPDATE fundamental_standardized SET period_end = ?
        WHERE security_id = 'S8' AND canonical_code = 'common_equity' AND period_end = ?
    """, [balance_date, QUARTERS[8]])
    engine.refresh_derived_metrics(store)
    # The flow ending 2021-06-30 starts 2021-04-01.
    assert _state(store, "S8", "roe_q_t", 9)[1:] == ("valid", lagged)
    assert _state(store, "S8", "roe_avg_q_t", 9)[1:] == ("valid", averaged)


def test_split_sensitive_comparisons_across_periods_need_flows_four_quarters_apart(store):
    _seed(store, "S10")
    # 2:1 split after 2020-12-31: later period-end share counts double, while the
    # current 10-Q restates the prior-year quarter's weighted shares and EPS.
    store.con.execute("""
        UPDATE fundamental_standardized SET value = value * 2
        WHERE security_id = 'S10' AND canonical_code = 'shares_outstanding_period_end'
          AND period_end > DATE '2020-12-31'
    """)
    engine.refresh_derived_metrics(store)

    assert _state(store, "S10", "eps_yoy_t", 8)[1:] == ("valid", "quarterly")
    assert _state(store, "S10", "wshares_yoy_t", 8)[1:] == ("valid", "quarterly")
    # Balances are never restated for a split, and nothing two or more years back is.
    assert _state(store, "S10", "shares_yoy_t", 8) == (
        pytest.approx(2 * SHARES[8] / SHARES[4] - 1), "valid", "incomparable")
    assert _state(store, "S10", "share_issuance_2y_t", 11)[1:] == ("valid", "incomparable")
    assert _state(store, "S10", "eps_cagr_2y_t", 11)[1:] == ("valid", "incomparable")


def _pair(older: tuple[dt.date | None, dt.date], newer: tuple[dt.date | None, dt.date]) -> bool:
    def span(dates, offset):
        start, end = (f"DATE '{value}'" if value is not None else "NULL::DATE" for value in dates)
        return annual.Span(start, end, "false", "true", offset)

    combined = annual._combine(span(newer, 0), span(older, 1))
    with duckdb.connect(config={"memory_limit": "64MB", "threads": 1}) as con:
        return con.execute(f"SELECT {combined.coherent}").fetchone()[0]


_Q2_2024 = (dt.date(2024, 4, 1), dt.date(2024, 6, 30))


@pytest.mark.parametrize(("older", "newer", "comparable"), [
    ((None, dt.date(2024, 3, 31)), _Q2_2024, True),  # balance the day before the flow
    ((None, dt.date(2024, 3, 24)), _Q2_2024, True),  # 7 days early
    ((None, dt.date(2024, 3, 23)), _Q2_2024, False),  # 8 days early
    ((None, dt.date(2024, 4, 1)), _Q2_2024, False),  # on the flow's first day
    ((None, dt.date(2024, 4, 7)), _Q2_2024, False),  # inside the flow period
    ((None, dt.date(2024, 3, 31)), (dt.date(2024, 4, 1), dt.date(2024, 5, 30)), False),  # 60-day stub flow
    ((None, dt.date(2024, 3, 31)), (None, dt.date(2024, 6, 30)), True),  # balances 91 days apart
    ((None, dt.date(2024, 4, 21)), (None, dt.date(2024, 6, 30)), True),  # 70 days apart
    ((None, dt.date(2024, 4, 22)), (None, dt.date(2024, 6, 30)), False),  # 69 days apart
    ((None, dt.date(2024, 3, 1)), (None, dt.date(2024, 6, 29)), True),  # 120 days apart
    ((None, dt.date(2024, 2, 29)), (None, dt.date(2024, 6, 29)), False),  # 121 days apart
    ((dt.date(2024, 1, 1), dt.date(2024, 3, 31)), (None, dt.date(2024, 6, 30)), False),  # balance after a flow
])
def test_one_bucket_instant_limits(older, newer, comparable):
    assert _pair(older, newer) is comparable
