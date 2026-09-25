"""R1b anomaly-breadth metrics: hand-computed economics on the real derived engine.

One synthetic issuer (S1) reports twenty contiguous calendar quarters of every
item any quarter-grid seed metric reads, filed 40 days after each quarter end.
The whole quarterly catalog is computed once on it. That gives two things:

* hand-computed values for the R1b definitions (numerator/denominator timing,
  lagged and average denominators, seasonal differences, eight-quarter scales);
* a real-engine cross-check of the research catalog: every catalog row whose
  admission rests on the engine's span labels must be ``blocked_incomparable_origin``
  exactly when the engine labels its values ``value_origin='incomparable'``.

Issuers S2-S6 carry the same values with specific buckets removed or made
non-positive, proving that a missing bucket yields no value rather than a
row-lag substitution, that non-positive opening balances yield no value, and
how an untagged or partly tagged equity-issuance concept is read.
"""

from __future__ import annotations

import datetime as dt
import math
import statistics
from collections.abc import Callable, Iterator

import pytest

from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
from atx_db.derived_registry import (
    QUARTER_GRID_WINDOWS,
    default_derived_definitions,
    seed_derived_metric_definitions,
)
from atx_db.item_registry import read_fundamental_item_seed
from atx_db.research import default_anomaly_catalog
from tests.conftest import _close_store, _open_template_copy

_ENDS = tuple(
    dt.date(year, month, day)
    for year in range(2018, 2023)
    for month, day in ((3, 31), (6, 30), (9, 30), (12, 31))
)
_LAST = len(_ENDS) - 1

# Plausible statement scales: positive margins, free cash flow and net operating
# assets, current assets plus net PP&E below total assets (Beneish AQI).
_BASE = {
    "revenue": 1000.0, "cost_of_revenue_cogs": 600.0, "gross_profit__1004": 410.0,
    "operating_income": 150.0, "net_income_total": 90.0, "net_income_to_common": 88.0,
    "pretax_income": 120.0, "income_tax_total": 30.0, "interest_expense_total": 10.0,
    "sg_and_a": 200.0, "r_and_d_expense": 50.0, "d_and_a_cash_flow": 40.0,
    "d_and_a_income_statement": 40.0, "ebitda_standardised": 190.0,
    "cash_flow_from_operations": 130.0, "capex__1305": -60.0,
    "eps_diluted": 0.9, "eps_basic__1034": 0.92,
    "weighted_avg_shares_diluted": 100.0, "weighted_avg_shares_basic": 98.0,
    "shares_outstanding_period_end": 99.0,
    "total_dividends_paid": 20.0, "common_dividends_paid": 18.0, "stock_repurchases_buybacks": 25.0,
    "stock_issuance": 8.0, "lt_debt_issued": 30.0, "lt_debt_repaid": 20.0,
    "stock_based_compensation": 12.0, "acquisitions": 15.0,
    "total_assets": 5000.0, "current_assets": 1500.0, "current_liabilities": 800.0,
    "pp_and_e_net": 1800.0, "pp_and_e_gross": 3000.0, "total_liabilities": 2600.0,
    "stockholders_equity": 2400.0, "common_equity": 2300.0, "preferred_stock": 50.0,
    "minority_interest_bs": 50.0, "total_debt": 1200.0, "short_term_debt": 200.0,
    "long_term_debt": 1000.0, "cash_and_st_investments": 400.0, "cash_only": 300.0,
    "short_term_investments": 100.0, "inventory": 300.0, "accounts_receivable": 350.0,
    "accounts_payable": 250.0, "goodwill": 400.0, "other_intangibles": 150.0,
    "retained_earnings": 1500.0, "long_term_investments": 200.0,
}


def _v(code: str, index: int) -> float:
    """Trend plus a period-5 wiggle, so seasonal (lag-4) differences vary."""
    wiggle = ((7 * index + len(code)) % 5) / 5.0
    return _BASE.get(code, 100.0) * (1.0 + 0.02 * index) * (1.0 + 0.05 * wiggle)


def _available(index: int) -> dt.datetime:
    return dt.datetime.combine(_ENDS[index] + dt.timedelta(days=40), dt.time(21, 0))


def _item_codes() -> tuple[str, ...]:
    return tuple(sorted({
        code for definition in default_derived_definitions()
        if definition.window in QUARTER_GRID_WINDOWS for code in definition.item_inputs
    }))


def _seed(store, security_id: str, *, drop: frozenset[tuple[str, int]] = frozenset(),
          override: dict[tuple[str, int], float] | None = None) -> None:
    instants = {row.canonical_code for row in read_fundamental_item_seed() if row.data_type == "instant"}
    rows = []
    for code in _item_codes():
        for index, period_end in enumerate(_ENDS):
            if (code, index) in drop:
                continue
            value = (override or {}).get((code, index), _v(code, index))
            instant = code in instants
            start = None if instant else (_ENDS[index - 1] + dt.timedelta(days=1) if index else dt.date(2018, 1, 1))
            rows.append([
                f"{security_id}|{code}|{period_end}", security_id, code, "instant" if instant else "quarterly",
                start, period_end, value, _available(index).date(), _available(index),
            ])
    store.con.executemany(
        """
        INSERT INTO fundamental_standardized (
            standardized_id, source, security_id, item_id, canonical_code, basis,
            period_start, period_end, value, as_of_date, available_at, input_codes_json,
            input_item_ids_json, rule_id, combination_rule, revision_sequence, is_latest_revision
        ) VALUES (?, 'test', ?, 1, ?, ?, ?, ?, ?, ?, ?, '[]', '[]', 'r', 'direct', 1, true)
        """,
        rows,
    )


@pytest.fixture(scope="module")
def engine_store(_schema_template, tmp_path_factory) -> Iterator[object]:
    store = _open_template_copy(_schema_template, tmp_path_factory.mktemp("r1b") / "warehouse.duckdb")
    try:
        store.con.execute("SET memory_limit = '256MB'")
        store.analytical_memory_limit = "256MB"
        seed_derived_metric_definitions(store)
        _seed(store, "S1")
        refresh_derived_metrics(store, DerivedMetricsOptions(security_ids=("S1",)))
        _seed(store, "S2", drop=frozenset({("net_income_total", 6)}))
        # Both common-equity sources (common_equity_q falls back to stockholders
        # equity less preferred) are absent at bucket 3.
        _seed(store, "S3", drop=frozenset({("common_equity", 3), ("stockholders_equity", 3)}),
              override={("common_equity", 7): -10.0})
        _seed(store, "S4", drop=frozenset({("pp_and_e_gross", 4), ("inventory", 13)}))
        # Equity-issuance proceeds never tagged (S5) or tagged for part of a year (S6).
        _seed(store, "S5", drop=frozenset(("stock_issuance", index) for index in range(len(_ENDS))))
        _seed(store, "S6", drop=frozenset({("stock_issuance", 17), ("stock_issuance", 18)}))
        refresh_derived_metrics(store, DerivedMetricsOptions(
            security_ids=("S2", "S3", "S4", "S5", "S6"),
            metric_codes=("sue_ni", "roe_variability_8q", "investment_to_assets", "inventory_change_to_assets",
                          "piotroski_f_cash_issuance"),
        ))
        yield store
    finally:
        _close_store(store)


def _state(store, code: str, index: int, security_id: str = "S1") -> tuple:
    row = store.con.execute(
        """SELECT value, value_status, value_origin FROM derived_metric_values
           WHERE security_id = ? AND metric_code = ? AND period_end = ?
           ORDER BY available_at DESC, derived_value_id DESC LIMIT 1""",
        [security_id, code, _ENDS[index]],
    ).fetchone()
    assert row is not None, (security_id, code, index)
    return row


# ------------------------------------------------------------------ hand formulas


def _ttm(code: str, i: int) -> float:
    return sum(_v(code, j) for j in range(i - 3, i + 1))


def _avg_assets(i: int) -> float:
    return (_v("total_assets", i) + _v("total_assets", i - 4)) / 2.0


def _noa(i: int) -> float:
    return ((_v("total_assets", i) - _v("cash_and_st_investments", i))
            - (_v("total_liabilities", i) - _v("total_debt", i)))


def _growth(code: str, i: int) -> float:
    base = _v(code, i - 4)
    return (_v(code, i) - base) / abs(base)


def _capex_ttm(i: int) -> float:
    return sum(abs(_v("capex__1305", j)) for j in range(i - 3, i + 1))


def _roe_q(i: int) -> float:
    return _v("net_income_total", i) / _v("common_equity", i - 1)


def _roa_q(i: int) -> float:
    return _v("net_income_total", i) / _v("total_assets", i - 1)


def _cfo_to_assets_q(i: int) -> float:
    return _v("cash_flow_from_operations", i) / _v("total_assets", i - 1)


def _seasonal_change(code: str, i: int) -> float:
    return _v(code, i) - _v(code, i - 4)


def _sue(code: str, i: int) -> float:
    # Scale: the eight seasonal changes before this one, excluding the current.
    return _seasonal_change(code, i) / statistics.stdev(_seasonal_change(code, j) for j in range(i - 8, i))


def _noa_turnover(i: int) -> float:
    return _ttm("revenue", i) / _noa(i - 4)


def _piotroski_cash_issuance(i: int, no_issuance: float) -> float:
    """Piotroski (2000) nine signals on trailing flows; EQ_OFFER supplied."""
    def roa(j: int) -> float:
        return _ttm("net_income_total", j) / _avg_assets(j)

    def cfo(j: int) -> float:
        return _ttm("cash_flow_from_operations", j) / _avg_assets(j)

    def leverage(j: int) -> float:
        return _v("long_term_debt", j) / _v("total_assets", j)

    def liquidity(j: int) -> float:
        return _v("current_assets", j) / _v("current_liabilities", j)

    def margin(j: int) -> float:
        return _ttm("gross_profit__1004", j) / _ttm("revenue", j)

    def turnover(j: int) -> float:
        return _ttm("revenue", j) / _avg_assets(j)

    signals = (
        roa(i) > 0, cfo(i) > 0, roa(i) > roa(i - 4), cfo(i) > roa(i), leverage(i) < leverage(i - 4),
        liquidity(i) > liquidity(i - 4), margin(i) > margin(i - 4), turnover(i) > turnover(i - 4),
    )
    return float(sum(signals)) + no_issuance


_EXPECTED: dict[str, Callable[[int], float]] = {
    # Quarterly profitability over the quarter's opening balance (prior quarter end).
    "roe_q": _roe_q,
    "roa_q": _roa_q,
    "rnoa_q": lambda i: _v("operating_income", i) / _noa(i - 1),
    "gross_profitability_q": lambda i: _v("gross_profit__1004", i) / _v("total_assets", i - 1),
    "operating_profitability_q": lambda i: (
        (_v("gross_profit__1004", i) - _v("sg_and_a", i)) / _v("total_assets", i - 1)),
    "cfo_to_assets_q": _cfo_to_assets_q,
    # Trailing flows over the opening (four-quarters-earlier) or average balance.
    "rnoa": lambda i: _ttm("operating_income", i) / _noa(i - 4),
    "noa_turnover": _noa_turnover,
    "noa_turnover_change_yoy": lambda i: _noa_turnover(i) - _noa_turnover(i - 4),
    "fcf_to_assets": lambda i: (_ttm("cash_flow_from_operations", i) - _capex_ttm(i)) / _avg_assets(i),
    "capex_to_assets": lambda i: _capex_ttm(i) / _avg_assets(i),
    "rd_to_assets": lambda i: _ttm("r_and_d_expense", i) / _avg_assets(i),
    "sga_to_sales": lambda i: _ttm("sg_and_a", i) / _ttm("revenue", i),
    "operating_leverage": lambda i: (_ttm("cost_of_revenue_cogs", i) + _ttm("sg_and_a", i)) / _avg_assets(i),
    "sustainable_growth": lambda i: (
        (_ttm("net_income_to_common", i) - _ttm("common_dividends_paid", i))
        / ((_v("common_equity", i) + _v("common_equity", i - 4)) / 2.0)),
    "capex_growth_2y": lambda i: _capex_ttm(i) / _capex_ttm(i - 8) - 1.0,
    "capex_growth_3y": lambda i: _capex_ttm(i) / _capex_ttm(i - 12) - 1.0,
    # Balance-sheet investment and inventory changes against lagged/average assets.
    "investment_to_assets": lambda i: (
        (_seasonal_change("pp_and_e_gross", i) + _seasonal_change("inventory", i)) / _v("total_assets", i - 4)),
    "inventory_change_to_assets": lambda i: _seasonal_change("inventory", i) / _avg_assets(i),
    # Issuance: split-consistent weighted shares (1y) and period-end shares (3y).
    "share_issuance_1y": lambda i: math.log(_v("weighted_avg_shares_basic", i) / _v("weighted_avg_shares_basic", i - 4)),
    "share_issuance_3y": lambda i: math.log(
        _v("shares_outstanding_period_end", i) / _v("shares_outstanding_period_end", i - 12)),
    # Abarbanell-Bushee signals on seasonally matched quarters.
    "sga_growth_less_sales_growth": lambda i: _growth("sg_and_a", i) - _growth("revenue", i),
    "inventory_growth_less_sales_growth": lambda i: _growth("inventory", i) - _growth("revenue", i),
    "receivables_growth_less_sales_growth": lambda i: _growth("accounts_receivable", i) - _growth("revenue", i),
    "sales_growth_less_gross_profit_growth": lambda i: _growth("revenue", i) - _growth("gross_profit__1004", i),
    # Leverage and liquidity.
    "debt_to_assets_change_yoy": lambda i: (
        _v("total_debt", i) / _v("total_assets", i) - _v("total_debt", i - 4) / _v("total_assets", i - 4)),
    "net_debt_to_book_equity": lambda i: (
        (_v("total_debt", i) - _v("cash_and_st_investments", i)) / _v("common_equity", i)),
    "current_ratio_change_yoy": lambda i: (
        _v("current_assets", i) / _v("current_liabilities", i)
        - _v("current_assets", i - 4) / _v("current_liabilities", i - 4)),
    "cash_to_assets": lambda i: _v("cash_and_st_investments", i) / _v("total_assets", i),
    # Piotroski with cash-flow equity issuance: S1 raises equity every quarter.
    "no_equity_issuance_ttm": lambda i: 0.0,
    "piotroski_f_cash_issuance": lambda i: _piotroski_cash_issuance(i, 0.0),
    # Seasonal differences, surprises and their changes.
    "ni_q_change_yoy": lambda i: _seasonal_change("net_income_total", i),
    "revenue_q_change_yoy": lambda i: _seasonal_change("revenue", i),
    "sue_ni": lambda i: _sue("net_income_total", i),
    "sue_revenue": lambda i: _sue("revenue", i),
    "roe_q_change_yoy": lambda i: _roe_q(i) - _roe_q(i - 4),
    "roa_q_change_yoy": lambda i: _roa_q(i) - _roa_q(i - 4),
    # Eight-quarter sample standard deviations ending this bucket.
    "ni_q_change_yoy_sd8": lambda i: statistics.stdev(
        _seasonal_change("net_income_total", j) for j in range(i - 7, i + 1)),
    "roe_variability_8q": lambda i: statistics.stdev(_roe_q(j) for j in range(i - 7, i + 1)),
    "roa_variability_8q": lambda i: statistics.stdev(_roa_q(j) for j in range(i - 7, i + 1)),
    "cfo_variability_8q": lambda i: statistics.stdev(_cfo_to_assets_q(j) for j in range(i - 7, i + 1)),
    "sales_growth_variability_8q": lambda i: statistics.stdev(_growth("revenue", j) for j in range(i - 7, i + 1)),
}


@pytest.mark.parametrize("code", sorted(_EXPECTED))
def test_r1b_definitions_match_hand_computed_economics(engine_store, code):
    value, status, _origin = _state(engine_store, code, _LAST)

    assert status == "valid"
    assert value == pytest.approx(_EXPECTED[code](_LAST), rel=1e-9)


def test_quarterly_returns_scale_by_the_opening_not_the_closing_balance(engine_store):
    index = 10
    roe, _, _ = _state(engine_store, "roe_q", index)

    assert roe == pytest.approx(_v("net_income_total", index) / _v("common_equity", index - 1), rel=1e-12)
    assert roe != pytest.approx(_v("net_income_total", index) / _v("common_equity", index), rel=1e-6)
    # The first quarter has no opening balance at all.
    assert _state(engine_store, "roe_q", 0)[:2] == (None, "missing_input_or_domain")


def test_roe_q_has_no_value_for_missing_or_non_positive_opening_equity(engine_store):
    # S3: every common-equity source missing at bucket 3; common equity -10 at bucket 7.
    assert _state(engine_store, "roe_q", 3, "S3")[0] == pytest.approx(_roe_q(3), rel=1e-12)
    # No row-lag substitution: bucket 4 never falls back to the bucket-2 equity.
    assert _state(engine_store, "roe_q", 4, "S3")[:2] == (None, "missing_input_or_domain")
    assert _state(engine_store, "roe_q", 8, "S3")[:2] == (None, "zero_denominator")
    assert _state(engine_store, "roe_q", 9, "S3")[0] == pytest.approx(
        _v("net_income_total", 9) / _v("common_equity", 8), rel=1e-12)


def test_eight_quarter_scales_need_eight_valid_quarters(engine_store):
    # First valid bucket = first bucket whose eight-bucket window is fully valid.
    for code, first_valid in (
        ("ni_q_change_yoy_sd8", 11), ("roe_variability_8q", 8), ("roa_variability_8q", 8),
        ("cfo_variability_8q", 8), ("sales_growth_variability_8q", 11),
    ):
        assert _state(engine_store, code, first_valid - 1)[:2] == (None, "missing_input_or_domain"), code
        value, status, _ = _state(engine_store, code, first_valid)
        assert (value, status) == (pytest.approx(_EXPECTED[code](first_valid), rel=1e-9), "valid"), code
    # SUE uses the prior eight changes, so it starts one bucket after the scale.
    assert _state(engine_store, "sue_ni", 11)[:2] == (None, "missing_input_or_domain")
    assert _state(engine_store, "sue_ni", 12)[0] == pytest.approx(_sue("net_income_total", 12), rel=1e-9)


def test_a_missing_bucket_voids_every_window_that_contains_it(engine_store):
    # S2: net income missing at bucket 6 -> seasonal changes at 6 and 10 are void.
    for index in (6, 10):
        assert _state(engine_store, "ni_q_change_yoy", index, "S2")[:2] == (None, "missing_input_or_domain")
    assert _state(engine_store, "ni_q_change_yoy", 11, "S2")[0] == pytest.approx(
        _seasonal_change("net_income_total", 11), rel=1e-12)
    # Every prior-eight scale for buckets 12..18 contains bucket 6 or 10.
    for index in range(12, 19):
        assert _state(engine_store, "sue_ni", index, "S2")[:2] == (None, "missing_input_or_domain"), index
    assert _state(engine_store, "sue_ni", 19, "S2")[0] == pytest.approx(_sue("net_income_total", 19), rel=1e-9)
    assert _state(engine_store, "roe_variability_8q", 13, "S2")[:2] == (None, "missing_input_or_domain")
    assert _state(engine_store, "roe_variability_8q", 14, "S2")[0] == pytest.approx(
        statistics.stdev(_roe_q(j) for j in range(7, 15)), rel=1e-9)


def test_investment_to_assets_falls_back_to_net_ppe_and_zero_inventory_change(engine_store):
    # S4: gross PP&E missing at bucket 4, inventory missing at bucket 13.
    assert _state(engine_store, "investment_to_assets", 8, "S4")[0] == pytest.approx(
        (_seasonal_change("pp_and_e_net", 8) + _seasonal_change("inventory", 8)) / _v("total_assets", 4), rel=1e-12)
    for index in (13, 17):
        assert _state(engine_store, "investment_to_assets", index, "S4")[0] == pytest.approx(
            _seasonal_change("pp_and_e_gross", index) / _v("total_assets", index - 4), rel=1e-12)
        assert _state(engine_store, "inventory_change_to_assets", index, "S4")[:2] == (
            None, "missing_input_or_domain")


def test_cash_issuance_signal_distinguishes_untagged_years_from_partial_years(engine_store):
    # S5 never tags equity proceeds but reports operating cash flow every quarter.
    assert _state(engine_store, "no_equity_issuance_ttm", _LAST, "S5")[:2] == (1.0, "valid")
    assert _state(engine_store, "piotroski_f_cash_issuance", _LAST, "S5")[0] == pytest.approx(
        _piotroski_cash_issuance(_LAST, 1.0))
    # S6 tags proceeds in buckets 16 and 19 only: a partial year has no value, not "no issuance".
    assert _state(engine_store, "no_equity_issuance_ttm", _LAST, "S6")[:2] == (None, "missing_input_or_domain")
    assert _state(engine_store, "piotroski_f_cash_issuance", _LAST, "S6")[0] is None
    assert _state(engine_store, "no_equity_issuance_ttm", 15, "S6")[:2] == (0.0, "valid")


# ------------------------------------------------- catalog admission vs the engine


def _engine_labeled_operands() -> Iterator[tuple[str, str, bool]]:
    """(feature, quarter-grid metric, blocked) for every engine-labeled catalog row.

    A seed-metric row on the quarter grid is labeled by the engine itself; a
    composition inherits the labels of its quarter-grid metric legs. Daily seed
    metrics and item legs carry no span label.
    """
    windows = {definition.metric_code: definition.window for definition in default_derived_definitions()}
    for entry in default_anomaly_catalog():
        blocked = entry.admission == "blocked_incomparable_origin"
        for operand in entry.operands:
            kind, _, code = operand.partition(":")
            if kind == "metric" and windows[code] in QUARTER_GRID_WINDOWS:
                yield entry.feature_id, code, blocked


def test_catalog_admission_matches_the_engine_value_origin(engine_store):
    problems: list[str] = []
    checked = 0
    for feature, code, blocked in _engine_labeled_operands():
        value, status, origin = _state(engine_store, code, _LAST)
        checked += 1
        if status != "valid" or value is None:
            problems.append(f"{feature}/{code}: the consecutive-quarter fixture yields {status}")
            continue
        if blocked:
            origins = {row[0] for row in engine_store.con.execute(
                "SELECT DISTINCT value_origin FROM derived_metric_values "
                "WHERE security_id = 'S1' AND metric_code = ? AND value_status = 'valid'", [code]).fetchall()}
            if origins != {"incomparable"}:
                problems.append(f"{feature}/{code}: cataloged blocked but the engine labels {sorted(origins)}")
        elif origin == "incomparable":
            problems.append(f"{feature}/{code}: cataloged eligible but the engine labels it incomparable")

    assert problems == []
    # Every quarter-grid seed row (144) plus five composition metric legs.
    assert checked >= 140
