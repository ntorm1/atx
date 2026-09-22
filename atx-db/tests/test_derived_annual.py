"""Focused FY evidence fixtures. Prepared statically; root owns execution."""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db import _derived_annual as annual
from atx_db import _derived_pit as pit
from atx_db import derived_metrics as engine
from atx_db.api.catalog import DERIVED_METRICS_SCHEMA
from atx_db.api.models import RangeRequest
from atx_db.api.service import WarehouseReadService
from atx_db.derived_registry import default_derived_definitions
from atx_db.market_daily import MarketDailyOptions, refresh_market_daily_metrics

CORE = frozenset({
    "revenue_ttm", "gross_profit_ttm", "ebitda_ttm", "net_income_ttm", "net_income_common_ttm",
    "cfo_ttm", "capex_ttm", "fcf_ttm", "eps_basic_ttm", "eps_diluted_ttm", "eps_ttm",
    "sales_per_share", "cfo_per_share", "fcf_per_share", "gross_margin", "net_margin",
    "current_ratio", "revenue_growth_yoy", "revenue_cagr_1y", "revenue_cagr_3y",
    "revenue_q_growth_yoy", "revenue_q_growth_qoq", "gross_profit_q_growth_yoy",
    "revenue_growth_qoq", "roa", "roe", "total_accruals",
})


@pytest.fixture
def store(tmp_store, monkeypatch):
    tmp_store.con.execute("SET memory_limit='1GB'")
    tmp_store.con.execute("SET threads=1")
    by_code = {definition.metric_code: definition for definition in default_derived_definitions()
               if definition.window != "daily"}
    selected = set(CORE)
    while dependencies := {code for name in selected for code in by_code[name].metric_inputs} - selected:
        selected.update(dependencies)
    definitions = tuple(definition for code, definition in by_code.items() if code in selected)
    monkeypatch.setattr(engine, "default_derived_definitions", lambda: definitions)
    return tmp_store


def fact(store, code, value, *, start="2024-01-01", end="2024-12-31", at="2025-02-20",
         basis="annual", security="S1", revision=1):
    identity = f"{security}|{code}|{basis}|{start}|{end}|{at}|{revision}"
    store.con.execute("""
        INSERT INTO fundamental_standardized (
            standardized_id, source, security_id, item_id, canonical_code, basis,
            period_start, period_end, value, as_of_date, available_at, input_codes_json,
            input_item_ids_json, rule_id, combination_rule, revision_sequence, is_latest_revision
        ) VALUES (?, 'test', ?, 1, ?, ?, CAST(? AS DATE), CAST(? AS DATE), ?, CAST(? AS DATE),
                  CAST(? AS TIMESTAMP), '[]', '[]', 'r', 'direct', ?, true)
    """, [identity, security, code, basis, start, end, value, at, at, revision])


def current(store, code, *, end="2024-12-31", cutoff="2099-01-01", security="S1"):
    return store.con.execute("""
        SELECT value, value_origin, available_at, arithmetic_available_at,
               fiscal_period_start, fiscal_period_end, derived_value_id, inputs_hash, value_status
        FROM derived_metric_values
        WHERE security_id=? AND metric_code=? AND period_end=CAST(? AS DATE)
          AND available_at <= CAST(? AS TIMESTAMP)
        ORDER BY available_at DESC, derived_value_id DESC LIMIT 1
    """, [security, code, end, cutoff]).fetchone()


def bar(store, date):
    date = dt.date.fromisoformat(date)
    store.con.execute("""
        INSERT INTO equity_daily_bars (
            source, security_id, symbol, trade_date, open, high, low, close, adjusted_close,
            volume, is_adjusted, available_at, as_of_date, is_latest_revision, shares_outstanding
        ) VALUES ('test', 'S1', 'AAA', ?, 20, 20, 20, 20, 20, 1000, false, ?, ?, true, 50)
    """, [date, dt.datetime.combine(date, dt.time(22)), date])


def history(store):
    return store.con.execute("""
        SELECT derived_value_id, inputs_hash, value, available_at, value_origin,
               arithmetic_available_at, revision_sequence, revision_count, valid_to
        FROM derived_metric_values ORDER BY derived_value_id
    """).fetchall()


def test_annual_only_values_growth_api_and_daily(store):
    fact(store, "revenue", 900, start="2023-01-01", end="2023-12-31", at="2024-02-20")
    for code, value in {"total_assets": 800, "common_equity": 250}.items():
        fact(store, code, value, start=None, end="2023-12-31", at="2024-02-20", basis="instant")
    for code, value in {
        "revenue": 1000, "gross_profit__1004": 400, "net_income_total": 100,
        "net_income_to_common": 100, "cash_flow_from_operations": 160, "capex__1305": -60,
        "operating_income": 120, "d_and_a_cash_flow": 20, "weighted_avg_shares_diluted": 50,
        "eps_diluted": 2, "eps_basic__1034": 2,
    }.items():
        fact(store, code, value)
    for code, value in {
        "common_equity": 300, "total_assets": 1000, "current_assets": 250,
        "current_liabilities": 125, "shares_outstanding_period_end": 50,
    }.items():
        fact(store, code, value, start=None, basis="instant")
    engine.refresh_derived_metrics(store)
    for code, expected in {
        "revenue_ttm": 1000, "gross_profit_ttm": 400, "ebitda_ttm": 140,
        "capex_ttm": 60, "fcf_ttm": 100, "eps_ttm": 2, "eps_diluted_ttm": 2,
        "eps_basic_ttm": 2, "gross_margin": .4, "net_margin": .1,
        "sales_per_share": 20, "cfo_per_share": 3.2, "fcf_per_share": 2,
        "current_ratio": 2, "revenue_growth_yoy": 100 / 900, "revenue_cagr_1y": 100 / 900,
        "roa": 100 / 900, "roe": 100 / 275, "total_accruals": -60 / 900,
    }.items():
        assert current(store, code)[0] == pytest.approx(expected), code
    assert current(store, "revenue_ttm")[1:6] == (
        "annual_fallback", dt.datetime(2025, 2, 20), dt.datetime(2025, 2, 20),
        dt.date(2024, 1, 1), dt.date(2024, 12, 31),
    )
    assert current(store, "eps_ttm")[1] == "annual_dependency"
    for code in ("gross_profit_q", "ebitda_q", "capex_q", "fcf_q", "revenue_q_growth_yoy",
                 "revenue_q_growth_qoq", "gross_profit_q_growth_yoy", "revenue_growth_qoq"):
        assert current(store, code)[0] is None, code
    assert store.con.execute("SELECT count(*) FROM fundamental_standardized WHERE basis='quarterly'").fetchone() == (0,)
    request = RangeRequest(dataset="ATX.US.FUNDAMENTALS", schema="derived-metrics", symbols=["S1"],
                           stype_in="security_id", start=dt.date(2024, 1, 1), end=dt.date(2025, 1, 1),
                           items=["revenue_ttm"])
    rows = WarehouseReadService()._range_cursor(
        store.con, request, DERIVED_METRICS_SCHEMA, dt.datetime(2025, 2, 21),
        ["value", "value_origin", "fiscal_period_start"], ["S1"],
    ).fetchall()
    assert rows == [(1000, "annual_fallback", dt.date(2024, 1, 1))]
    bar(store, "2025-02-21")
    refresh_market_daily_metrics(store, MarketDailyOptions(security_ids=("S1",)))
    assert store.con.execute("""
        SELECT market_cap, pe_ttm, ps_ttm, pcf_ttm, fcf_yield FROM market_daily_metrics
    """).fetchone() == pytest.approx((1000, 10, 1, 6.25, .1))


def test_precedence_switches_and_null_never_resurrects(store):
    for start, end, value, at in (
        ("2024-01-01", "2024-03-31", 100, "2024-05-10"),
        ("2024-04-01", "2024-06-30", 200, "2024-08-10"),
        ("2024-07-01", "2024-09-30", 300, "2024-11-10"),
        # A later completed discrete Q4 (FY-minus-9M standardizer output).
        ("2024-10-01", "2024-12-31", 350, "2025-03-10"),
    ):
        fact(store, "revenue", value, start=start, end=end, at=at, basis="quarterly")
    fact(store, "revenue", 1000)
    fact(store, "revenue", 1100, at="2025-02-25", revision=2)
    # Standardized values are NOT NULL. An admitted nonfinite revision must
    # yield an explicit canonical NULL state, as zero division does in P1.
    fact(store, "revenue", float("nan"), start="2024-04-01", end="2024-06-30",
         at="2025-04-10", basis="quarterly", revision=2)
    fact(store, "revenue", float("nan"), at="2025-05-10", revision=3)
    fact(store, "revenue", 0, at="2025-06-10", revision=4)
    engine.refresh_derived_metrics(store)
    for cutoff, value, origin, event, arithmetic in (
        ("2025-02-21", 1000, "annual_fallback", "2025-02-20", "2025-02-20"),
        ("2025-03-01", 1100, "annual_fallback", "2025-02-25", "2025-02-25"),
        ("2025-03-11", 950, "quarterly", "2025-03-10", "2025-03-10"),
        ("2025-04-11", 1100, "annual_fallback", "2025-04-10", "2025-02-25"),
        ("2025-06-11", 0, "annual_fallback", "2025-06-10", "2025-06-10"),
    ):
        row = current(store, "revenue_ttm", cutoff=cutoff)
        assert row[:4] == (value, origin, dt.datetime.fromisoformat(event), dt.datetime.fromisoformat(arithmetic))
    invalid = current(store, "revenue_ttm", cutoff="2025-05-11")
    assert invalid[0] is None and invalid[1] == "unavailable"
    assert invalid[2] == dt.datetime(2025, 5, 10)
    assert current(store, "revenue_ttm")[8] == "valid"  # Zero is a real annual total.
    assert store.con.execute("""
        SELECT count(DISTINCT revision_group_id) FROM derived_metric_values
        WHERE metric_code='revenue_ttm' AND period_end=DATE '2024-12-31'
    """).fetchone() == (1,)
    assert store.con.execute("""
        SELECT count(*) FROM (
            SELECT *, lead(available_at) OVER (PARTITION BY revision_group_id ORDER BY available_at) AS expected_to
            FROM derived_metric_values
        ) WHERE valid_to IS DISTINCT FROM expected_to
    """).fetchone() == (0,)
    request = RangeRequest(dataset="ATX.US.FUNDAMENTALS", schema="derived-metrics", symbols=["S1"],
                           stype_in="security_id", start=dt.date(2024, 12, 31), end=dt.date(2025, 1, 1),
                           items=["revenue_ttm"])
    service = WarehouseReadService()
    assert service._range_cursor(store.con, request, DERIVED_METRICS_SCHEMA,
                                 dt.datetime(2025, 5, 11), ["value", "value_origin"], ["S1"]).fetchall() == [
        (None, "unavailable"),
    ]
    first = request.model_copy(update={"vintage": "first_reported"})
    assert service._range_cursor(store.con, first, DERIVED_METRICS_SCHEMA,
                                 dt.datetime(2025, 5, 11), ["value", "value_origin"], ["S1"]).fetchall() == [
        (1000, "annual_fallback"),
    ]
    for date in ("2025-02-21", "2025-05-11"):
        bar(store, date)
    refresh_market_daily_metrics(store, MarketDailyOptions(security_ids=("S1",)))
    assert store.con.execute("SELECT ps_ttm FROM market_daily_metrics ORDER BY trade_date").fetchall() == [(1,), (None,)]
    before = history(store)
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(event_chunk_size=1, max_frame_rows=64))
    assert history(store) == before


def test_missing_nine_months_needs_no_synthetic_quarter(store):
    fact(store, "revenue", 100, start="2024-01-01", end="2024-03-31", at="2024-05-10", basis="quarterly")
    fact(store, "revenue", 200, start="2024-04-01", end="2024-06-30", at="2024-08-10", basis="quarterly")
    fact(store, "revenue", 1000)
    engine.refresh_derived_metrics(store)
    assert current(store, "revenue_ttm")[:2] == (1000, "annual_fallback")
    assert current(store, "revenue_q_growth_qoq")[0] is None
    assert store.con.execute("""
        SELECT count(*) FROM fundamental_standardized WHERE basis='quarterly' AND period_end=DATE '2024-12-31'
    """).fetchone() == (0,)


@pytest.mark.parametrize("has_nine_month", [True, False])
def test_actual_standardizer_fy_minus_nine_month_precedence(store, has_nine_month):
    from atx_db.standardization import refresh_fundamental_standardized
    from tests.test_standardization import _insert_statement_duration

    inputs = [
        ("q1", "2025-03-31", "2025-05-10", "Q1", 100),
        ("six", "2025-06-30", "2025-08-10", "Q2", 300),
        ("fy", "2025-12-31", "2026-02-20", "FY", 1000),
    ]
    if has_nine_month:
        inputs.append(("nine", "2025-09-30", "2025-11-10", "Q3", 600))
    for identity, end, at, fiscal_period, value in inputs:
        _insert_statement_duration(
            store, statement_point_id=f"annual-test-{identity}", security_id="S1", value=value,
            period_start=dt.date(2025, 1, 1), period_end=dt.date.fromisoformat(end),
            available_at=dt.datetime.fromisoformat(at), fiscal_period=fiscal_period,
        )
    refresh_fundamental_standardized(store)
    engine.refresh_derived_metrics(store)
    expected_origin = "quarterly" if has_nine_month else "annual_fallback"
    assert current(store, "revenue_ttm", end="2025-12-31")[:2] == (1000, expected_origin)
    quarters = store.con.execute("""
        SELECT value FROM fundamental_standardized
        WHERE security_id='S1' AND canonical_code='revenue' AND basis='quarterly'
          AND period_end=DATE '2025-12-31'
    """).fetchall()
    assert quarters == ([(400,)] if has_nine_month else [])


def test_annual_composition_and_weighted_share_invalidations(store):
    for code, value in {
        "revenue": 1000, "cost_of_revenue_cogs": 600, "operating_income": 120,
        "d_and_a_income_statement": 10, "d_and_a_cash_flow": 20,
        "net_income_to_common": 100, "weighted_avg_shares_diluted": 50,
    }.items():
        fact(store, code, value)
    fact(store, "weighted_avg_shares_diluted", 0, at="2025-03-10", revision=2)
    # Keep the source contract: nonnull invalid input, then canonical NULL.
    fact(store, "weighted_avg_shares_diluted", float("nan"), at="2025-04-10", revision=3)
    engine.refresh_derived_metrics(store)
    assert current(store, "gross_profit_ttm")[:2] == (400, "annual_fallback")
    assert current(store, "ebitda_ttm")[:2] == (130, "annual_fallback")
    assert current(store, "eps_ttm", cutoff="2025-03-01")[0] == 2
    assert current(store, "eps_ttm", cutoff="2025-03-11")[8] == "zero_denominator"
    assert current(store, "eps_ttm")[0] is None
    assert current(store, "eps_ttm")[8] == "nonfinite"
    assert current(store, "eps_ttm")[2] == dt.datetime(2025, 4, 10)


def test_unrelated_annual_span_keeps_complete_quarterly_revenue(store):
    for start, end, value, at in (
        ("2024-01-01", "2024-03-31", 100, "2024-05-10"),
        ("2024-04-01", "2024-06-30", 200, "2024-08-10"),
        ("2024-07-01", "2024-09-30", 300, "2024-11-10"),
        ("2024-10-01", "2024-12-31", 400, "2025-02-10"),
    ):
        fact(store, "revenue", value, start=start, end=end, at=at, basis="quarterly")
    engine.refresh_derived_metrics(store)
    before = current(store, "revenue_ttm")
    assert before[:4] == (1000, "quarterly", dt.datetime(2025, 2, 10), dt.datetime(2025, 2, 10))

    # There is no annual revenue alternative. This unrelated 335-day cost
    # span must not invalidate the existing complete January-to-December sum.
    fact(store, "cost_of_revenue_cogs", 600, start="2024-02-01", at="2025-03-10")
    engine.refresh_derived_metrics(store)
    assert current(store, "revenue_ttm", cutoff="2025-03-01") == before
    after = current(store, "revenue_ttm")
    assert after[:4] == (1000, "quarterly", dt.datetime(2025, 3, 10), dt.datetime(2025, 2, 10))
    assert after[4:6] == before[4:6] == (dt.date(2024, 1, 1), dt.date(2024, 12, 31))
    assert after[8] == "valid"


def test_annual_eps_ignores_unselected_legacy_quarterly_share_span(store):
    fact(store, "net_income_to_common", 100)
    fact(store, "weighted_avg_shares_diluted", 50)
    engine.refresh_derived_metrics(store)
    before = current(store, "eps_ttm")
    assert before[:4] == (2, "annual_dependency", dt.datetime(2025, 2, 20), dt.datetime(2025, 2, 20))

    # A supported legacy quarterly row lacks its start. The selected annual
    # denominator is still 50 with its own complete fiscal-year span.
    fact(store, "weighted_avg_shares_diluted", 40, start=None, at="2025-03-10", basis="quarterly")
    engine.refresh_derived_metrics(store)
    assert current(store, "eps_ttm", cutoff="2025-03-01") == before
    after = current(store, "eps_ttm")
    assert after[:4] == (2, "annual_dependency", dt.datetime(2025, 3, 10), dt.datetime(2025, 2, 20))
    assert after[4:6] == before[4:6] == (dt.date(2024, 1, 1), dt.date(2024, 12, 31))
    assert after[8] == "valid"


def test_no_stale_fy_at_new_quarter_or_wrong_endpoint(store):
    fact(store, "revenue", 1000)
    fact(store, "gross_profit__1004", 400)
    fact(store, "revenue", 300, start="2025-01-01", end="2025-03-31", at="2025-05-10", basis="quarterly")
    # A newer observed representative date in the same bucket is not the FY end.
    fact(store, "current_assets", 250, start=None, end="2025-01-02", at="2025-03-10", basis="instant")
    engine.refresh_derived_metrics(store)
    assert current(store, "revenue_ttm", cutoff="2025-03-01")[:2] == (1000, "annual_fallback")
    assert current(store, "revenue_ttm", end="2025-01-02")[0] is None
    assert current(store, "revenue_ttm", end="2025-03-31")[0] is None
    assert current(store, "gross_margin", end="2025-03-31")[0] is None


@pytest.mark.parametrize("start,end", [("2024-04-01", "2024-12-31"), (None, "2024-12-31")])
def test_stub_or_unproven_annual_duration_is_not_full_year(store, start, end):
    fact(store, "revenue", 1000, start=start, end=end)
    fact(store, "current_assets", 250, start=None, basis="instant")
    engine.refresh_derived_metrics(store)
    assert current(store, "revenue_ttm")[0] is None


def test_mismatched_annual_spans_do_not_compose(store):
    fact(store, "revenue", 1000)
    fact(store, "cost_of_revenue_cogs", 600, start="2024-02-01", at="2025-03-10")
    fact(store, "net_income_to_common", 100)
    fact(store, "weighted_avg_shares_diluted", 50, start="2024-02-01", at="2025-03-10")
    engine.refresh_derived_metrics(store)
    assert current(store, "gross_profit_ttm")[0] is None
    assert current(store, "gross_margin")[0] is None
    assert current(store, "eps_ttm")[0] is None


def test_annual_missing_years_and_cagr_domain(store):
    fact(store, "revenue", 100, start="2021-01-01", end="2021-12-31", at="2022-02-20")
    fact(store, "revenue", 800)
    engine.refresh_derived_metrics(store)
    assert current(store, "revenue_growth_yoy")[0] is None
    assert current(store, "revenue_cagr_3y")[0] == pytest.approx(1)
    assert current(store, "revenue_ttm", end="2023-12-31") is None
    fact(store, "revenue", -800, at="2025-03-10", revision=2)
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(metric_codes=("revenue_ttm",)))
    assert current(store, "revenue_cagr_3y")[0] is None
    assert current(store, "revenue_cagr_3y", cutoff="2025-03-01")[0] == pytest.approx(1)


def test_annual_limits_and_atomic_failure_preserve_previous_scope(store, monkeypatch):
    fact(store, "revenue", 1000)
    fact(store, "revenue", 700, security="S2")
    engine.refresh_derived_metrics(store)
    before = history(store)
    fact(store, "revenue", 1100, at="2025-03-10", revision=2)
    with pytest.raises(RuntimeError, match="input limit"):
        engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",), max_input_rows=1))
    assert history(store) == before
    with pytest.raises(RuntimeError, match="candidate upper bound"):
        engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",), max_candidate_rows=1))
    assert history(store) == before
    original = pit.finish_metric
    injected = False

    def duplicate(con):
        nonlocal injected
        original(con)
        if not injected and con.execute("SELECT count(*) FROM _pit_stage").fetchone()[0]:
            con.execute("INSERT INTO _pit_stage SELECT * FROM _pit_stage LIMIT 1")
            injected = True

    monkeypatch.setattr(pit, "finish_metric", duplicate)
    with pytest.raises(Exception, match=r"[Dd]uplicate|[Pp]rimary|[Cc]onstraint"):
        engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",)))
    assert history(store) == before


def test_annual_event_candidates_remain_locally_bounded(store):
    definition = next(d for d in default_derived_definitions() if d.metric_code == "revenue_ttm")
    plan = annual.plan_for(definition, {definition.metric_code: definition})
    for year in range(2000, 2020):
        fact(store, "revenue", year, start=f"{year}-01-01", end=f"{year}-12-31", at=f"{year + 1}-02-20")
    try:
        pit.prepare_security(store.con, "S1", 100)
        count = pit.prepare_metric(store.con, definition, 3, 100, plan)
        assert count == 20
        assert store.con.execute("SELECT count(*) FROM _pit_items").fetchone() == (0,)
        assert store.con.execute("SELECT count(*) FROM _pit_annual_items").fetchone() == (20,)
        assert count * 4 < 20 * 20
    finally:
        pit.cleanup(store.con)
