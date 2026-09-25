"""R1b anomaly-breadth metrics: hand-computed economics on the real derived engine.

One synthetic issuer (S1) reports twenty contiguous calendar quarters of every
item any quarter-grid seed metric reads, filed 40 days after each quarter end,
and trades on flat weekday bars (no split; every split basis proven, R1d). The
whole quarterly catalog is computed once on it. That gives two things:

* hand-computed values for the R1b definitions (numerator/denominator timing,
  lagged and average denominators, seasonal differences, eight-quarter scales);
* a real-engine cross-check of the research catalog: every catalog row whose
  admission rests on the engine's span labels must be ``blocked_incomparable_origin``
  exactly when the engine labels its values ``value_origin='incomparable'``.
  S10 is S1 without bars: there, only ``split_basis`` rows may be incomparable.

Issuers S2-S9 carry the same values with specific buckets removed or made
non-positive, proving that a missing bucket yields no value rather than a
row-lag substitution, that non-positive opening balances yield no value, how an
untagged or partly tagged equity-issuance concept is read, and that a missing
debt, inventory or dividend concept is zero only for an issuer that tags none of
it throughout its presence window (never a tag switch or a skipped quarter).
"""

from __future__ import annotations

import datetime as dt
import json
import math
import statistics
from collections.abc import Callable, Iterator

import pandas as pd
import pytest

from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
from atx_db.derived_registry import (
    QUARTER_GRID_WINDOWS,
    default_derived_definitions,
    seed_derived_metric_definitions,
)
from atx_db.item_registry import read_fundamental_item_seed
from atx_db.research import default_anomaly_catalog
from atx_db.research.catalog import derive_metric_shapes
from atx_db.standardization import compute_standardized_rows, default_standardization_rules
from tests.conftest import _close_store, _open_template_copy

_DEBT = ("total_debt", "short_term_debt", "long_term_debt")

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


def _flat_bars(store, security_id: str) -> None:
    """Weekday bars with a flat vendor factor over every filing clock: no split
    happened and the split basis of every share-basis operand is proven (R1d).

    The factor is 0.95 (a total-return factor carrying later dividends): a load
    whose factor is 1 everywhere proves nothing under R1d fix 2."""
    store.con.execute(
        """
        INSERT INTO equity_daily_bars (source, security_id, symbol, trade_date, close, adjusted_close, available_at)
        SELECT 'test', ?, ?, d::DATE, 10.0, 9.5, d::DATE + INTERVAL 22 HOUR
        FROM generate_series(TIMESTAMP '2017-12-01', TIMESTAMP '2023-06-30', INTERVAL 1 DAY) t(d)
        WHERE dayofweek(d) BETWEEN 1 AND 5
        """,
        [security_id, security_id],
    )


_QUARTER_GRID_CODES = tuple(
    definition.metric_code for definition in default_derived_definitions()
    if definition.window in QUARTER_GRID_WINDOWS
)


@pytest.fixture(scope="module")
def engine_store(_schema_template, tmp_path_factory) -> Iterator[object]:
    store = _open_template_copy(_schema_template, tmp_path_factory.mktemp("r1b") / "warehouse.duckdb")
    try:
        store.con.execute("SET memory_limit = '256MB'")
        store.analytical_memory_limit = "256MB"
        seed_derived_metric_definitions(store)
        # S1 has daily bars (split basis proven); S10 is the same issuer without
        # bars, where every share-basis comparison stays unproven.
        _seed(store, "S1")
        _flat_bars(store, "S1")
        _seed(store, "S10")
        refresh_derived_metrics(store, DerivedMetricsOptions(
            security_ids=("S1", "S10"), metric_codes=_QUARTER_GRID_CODES))
        _seed(store, "S2", drop=frozenset({("net_income_total", 6)}))
        # Both common-equity sources (common_equity_q falls back to stockholders
        # equity less preferred) are absent at bucket 3.
        _seed(store, "S3", drop=frozenset({("common_equity", 3), ("stockholders_equity", 3)}),
              override={("common_equity", 7): -10.0})
        _seed(store, "S4", drop=frozenset({("pp_and_e_gross", 4), ("inventory", 13)}))
        # Equity-issuance proceeds never tagged (S5) or tagged for part of a year (S6).
        _seed(store, "S5", drop=frozenset(("stock_issuance", index) for index in range(len(_ENDS))))
        _seed(store, "S6", drop=frozenset({("stock_issuance", 17), ("stock_issuance", 18)}))
        # S7 never reports debt, inventory or dividends; S8 reports long-term debt
        # only; S9 switches its debt to unmapped aliases at bucket 8 (twelve quarters
        # of post-switch window, far past any five-quarter window), stops tagging
        # inventory at bucket 16 and skips its common dividend at bucket 18.
        _seed(store, "S7", drop=frozenset(
            (code, index) for code in (*_DEBT, "inventory", "common_dividends_paid", "total_dividends_paid")
            for index in range(len(_ENDS))))
        _seed(store, "S8", drop=frozenset(
            (code, index) for code in ("total_debt", "short_term_debt") for index in range(len(_ENDS))))
        _seed(store, "S9", drop=frozenset(
            {(code, index) for code in _DEBT for index in range(8, len(_ENDS))}
            | {("inventory", index) for index in range(16, len(_ENDS))}
            | {("common_dividends_paid", 18)}))
        refresh_derived_metrics(store, DerivedMetricsOptions(
            security_ids=("S2", "S3", "S4", "S5", "S6", "S7", "S8", "S9"),
            metric_codes=("sue_ni", "roe_variability_8q", "investment_to_assets", "inventory_change_to_assets",
                          "piotroski_f_cash_issuance", "debt_to_assets", "debt_to_assets_change_yoy",
                          "net_debt_to_book_equity", "sustainable_growth", "long_term_debt_to_assets",
                          "quick_ratio", "working_capital_accruals", "net_equity_issuance"),
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


def _piotroski_cash_issuance(i: int, no_issuance: float, *, debt_free: bool = False) -> float:
    """Piotroski (2000) nine signals on trailing flows; EQ_OFFER supplied."""
    def roa(j: int) -> float:
        return _ttm("net_income_total", j) / _avg_assets(j)

    def cfo(j: int) -> float:
        return _ttm("cash_flow_from_operations", j) / _avg_assets(j)

    def leverage(j: int) -> float:
        return 0.0 if debt_free else _v("long_term_debt", j) / _v("total_assets", j)

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
    # RSST total accruals: change in NOA less change in debt (R1a M4: no second LTI term).
    "rsst_accruals": lambda i: (
        (_noa(i) - _noa(i - 4)) - _seasonal_change("total_debt", i)) / _avg_assets(i),
    "capex_growth_2y": lambda i: _capex_ttm(i) / _capex_ttm(i - 8) - 1.0,
    "capex_growth_3y": lambda i: _capex_ttm(i) / _capex_ttm(i - 12) - 1.0,
    # Balance-sheet investment (net PP&E, R1b J2) and inventory changes against lagged/average assets.
    "investment_to_assets": lambda i: (
        (_seasonal_change("pp_and_e_net", i) + _seasonal_change("inventory", i)) / _v("total_assets", i - 4)),
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


def _net_ia(index: int) -> float:
    return (_seasonal_change("pp_and_e_net", index) + _seasonal_change("inventory", index)) / _v(
        "total_assets", index - 4)


def test_investment_to_assets_keeps_one_ppe_basis_and_treats_missing_inventory_like_its_sibling(engine_store):
    # R1b J2 on S4 (gross PP&E missing at bucket 4, inventory missing at bucket 13).
    # Net PP&E is the only basis: the gross gap (an annual-note-only gross line)
    # changes nothing, so no fiscal quarter switches basis and no issuer differs
    # from another by its gross/net reporting choice.
    for index in range(4, 13):
        assert _state(engine_store, "investment_to_assets", index, "S4")[0] == pytest.approx(
            _net_ia(index), rel=1e-12), index
    assert _state(engine_store, "investment_to_assets", 8, "S4")[0] != pytest.approx(
        (_seasonal_change("pp_and_e_gross", 8) + _seasonal_change("inventory", 8)) / _v("total_assets", 4),
        rel=1e-6)
    # An issuer that reports inventory has no inventory change at a missing end, in
    # both inventory features alike (never a zero).
    for index in (13, 17):
        for code in ("investment_to_assets", "inventory_change_to_assets"):
            assert _state(engine_store, code, index, "S4")[0] is None, (code, index)


def test_missing_debt_inventory_and_dividends_are_zero_only_for_issuers_that_never_report_them(engine_store):
    # R1b J4 / R1a M5. S7 never reports any debt, inventory or dividend concept.
    # Presence rules: debt and inventory never tagged up to the quarter (the
    # ever-reported chains), dividends untagged in all trailing four cash-flow statements.
    i = _LAST
    assert _state(engine_store, "debt_to_assets", i, "S7")[:2] == (0.0, "valid")
    # The zeros keep the spans of their statements: comparable, not incomparable.
    for code in ("debt_to_assets", "debt_to_assets_change_yoy", "net_debt_to_book_equity",
                 "inventory_change_to_assets", "investment_to_assets", "sustainable_growth"):
        assert _state(engine_store, code, i, "S7")[2] != "incomparable", code
    assert _state(engine_store, "debt_to_assets", i, "S8")[2] != "incomparable"
    # An issuer that has never tagged a debt alias reads zero from its first balance sheet.
    assert _state(engine_store, "debt_to_assets", 0, "S7")[:2] == (0.0, "valid")
    assert _state(engine_store, "debt_to_assets_change_yoy", i, "S7")[:2] == (0.0, "valid")
    assert _state(engine_store, "net_debt_to_book_equity", i, "S7")[0] == pytest.approx(
        -_v("cash_and_st_investments", i) / _v("common_equity", i), rel=1e-12)
    assert _state(engine_store, "inventory_change_to_assets", i, "S7")[:2] == (0.0, "valid")
    assert _state(engine_store, "investment_to_assets", i, "S7")[0] == pytest.approx(
        _seasonal_change("pp_and_e_net", i) / _v("total_assets", i - 4), rel=1e-12)
    assert _state(engine_store, "sustainable_growth", i, "S7")[0] == pytest.approx(
        _ttm("net_income_to_common", i) / ((_v("common_equity", i) + _v("common_equity", i - 4)) / 2.0),
        rel=1e-12)
    # The same rule for long-term debt (the Piotroski leverage signal), short-term
    # debt in operating working capital and inventory in the quick ratio: a debt-free,
    # inventory-free issuer keeps its leverage signal, F-score, accruals and quick ratio.
    assert _state(engine_store, "long_term_debt_to_assets", i, "S7")[:2] == (0.0, "valid")
    assert _state(engine_store, "quick_ratio", i, "S7")[0] == pytest.approx(
        _v("current_assets", i) / _v("current_liabilities", i), rel=1e-12)

    def owc(j: int) -> float:
        return _v("current_assets", j) - _v("cash_and_st_investments", j) - _v("current_liabilities", j)

    assert _state(engine_store, "working_capital_accruals", i, "S7")[0] == pytest.approx(
        (owc(i) - owc(i - 4)) / _avg_assets(i), rel=1e-12)
    assert _state(engine_store, "piotroski_f_cash_issuance", i, "S7")[0] == pytest.approx(
        _piotroski_cash_issuance(i, 0.0, debt_free=True))
    # S8 reports long-term debt only: a never-reported short-term component is zero.
    assert _state(engine_store, "debt_to_assets", i, "S8")[0] == pytest.approx(
        _v("long_term_debt", i) / _v("total_assets", i), rel=1e-12)
    assert _state(engine_store, "long_term_debt_to_assets", i, "S8")[0] == pytest.approx(
        _v("long_term_debt", i) / _v("total_assets", i), rel=1e-12)
    assert _state(engine_store, "working_capital_accruals", i, "S8")[0] == pytest.approx(
        (owc(i) - owc(i - 4)) / _avg_assets(i), rel=1e-12)
    # S9 switches debt to unmapped aliases at bucket 8: once a mapped alias has been
    # seen, absence is missing for every later quarter, never a zero (Re-review 1 N1).
    assert _state(engine_store, "debt_to_assets", 7, "S9")[0] == pytest.approx(
        _v("total_debt", 7) / _v("total_assets", 7), rel=1e-12)
    for index in range(8, len(_ENDS)):
        for code in ("debt_to_assets", "debt_to_assets_change_yoy", "net_debt_to_book_equity",
                     "long_term_debt_to_assets", "working_capital_accruals", "piotroski_f_cash_issuance"):
            assert _state(engine_store, code, index, "S9")[0] is None, (code, index)
    for index in range(16, len(_ENDS)):
        for code in ("investment_to_assets", "inventory_change_to_assets", "quick_ratio"):
            assert _state(engine_store, code, index, "S9")[0] is None, (code, index)
    # S9 skips its common dividend at bucket 18: the payer never reads as a non-payer.
    assert _state(engine_store, "sustainable_growth", 17, "S9")[0] == pytest.approx(
        _EXPECTED["sustainable_growth"](17), rel=1e-12)
    for index in (18, 19):
        assert _state(engine_store, "sustainable_growth", index, "S9")[0] is None, index


def test_excluded_duplicate_and_negation_are_exact_identities(engine_store):
    # R1a I3: the exclusions record identities, not approximations.
    growth = _state(engine_store, "revenue_growth_yoy", _LAST)[0]
    assert _state(engine_store, "revenue_cagr_1y", _LAST)[0] == pytest.approx(growth, rel=1e-12)
    issuance = _state(engine_store, "net_equity_issuance", _LAST)[0]
    assert _state(engine_store, "buyback_ratio", _LAST)[0] == pytest.approx(-issuance, rel=1e-12)


def _sga_candidate(item_id: int, concept: str, value: float) -> dict:
    return {
        "upstream_source": "fixture", "source": "fixture", "security_id": "SEC-CIK-0000000001",
        "symbol": "TST", "cik": "1", "item_id": item_id, "canonical_metric": concept, "concept": concept,
        "taxonomy": "us-gaap", "unit": "USD", "unit_type": "monetary", "basis": "annual",
        "period_start": dt.date(2025, 1, 1), "period_end": dt.date(2025, 12, 31), "fiscal_year": 2025,
        "fiscal_period": "FY", "accession_number": "acc", "source_accession": "acc",
        "filed_date": dt.date(2026, 2, 1), "value": value, "available_at": dt.datetime(2026, 2, 1, 22, 0),
        "input_rank": 10,
    }


def test_sga_composes_selling_plus_general_and_administrative_when_no_total_is_tagged():
    """R1b J3: filers that split SG&A (common in technology) keep the SG&A features."""
    rules = tuple(rule for rule in default_standardization_rules() if rule.rule_id == "std_annual_1005")
    split = [_sga_candidate(1006, "SellingAndMarketingExpense", 300.0),
             _sga_candidate(1007, "GeneralAndAdministrativeExpense", 120.0)]

    composed = compute_standardized_rows(pd.DataFrame(split), rules=rules)
    assert list(composed["value"]) == [420.0]
    # Labeled: the composite records its rule and both component items.
    assert composed["combination_rule"].iloc[0] == "coalesce_or_sum"
    assert json.loads(composed["input_item_ids_json"].iloc[0]) == [1006, 1007]

    direct = compute_standardized_rows(pd.DataFrame(
        [_sga_candidate(1005, "SellingGeneralAndAdministrativeExpense", 450.0), *split]), rules=rules)
    assert list(direct["value"]) == [450.0]
    # Both components are required; one alone is not SG&A.
    assert compute_standardized_rows(pd.DataFrame(split[:1]), rules=rules).empty


def test_cash_issuance_signal_distinguishes_untagged_years_from_partial_years(engine_store):
    # S5 never tags equity proceeds but reports operating cash flow every quarter.
    assert _state(engine_store, "no_equity_issuance_ttm", _LAST, "S5")[:2] == (1.0, "valid")
    assert _state(engine_store, "piotroski_f_cash_issuance", _LAST, "S5")[0] == pytest.approx(
        _piotroski_cash_issuance(_LAST, 1.0))
    # S6 tags proceeds in buckets 16 and 19 only: a partial year has no value, not "no issuance".
    assert _state(engine_store, "no_equity_issuance_ttm", _LAST, "S6")[:2] == (None, "missing_input_or_domain")
    assert _state(engine_store, "piotroski_f_cash_issuance", _LAST, "S6")[0] is None
    assert _state(engine_store, "no_equity_issuance_ttm", 15, "S6")[:2] == (0.0, "valid")
    # Re-review 1 N2: the same presence guard on the trailing flow itself. A never-
    # issuer's issuance is an imputed zero; a partly tagged year has no value.
    assert _state(engine_store, "net_equity_issuance", _LAST, "S5")[:2] == (
        pytest.approx(-_ttm("stock_repurchases_buybacks", _LAST) / _avg_assets(_LAST), rel=1e-12), "valid")
    assert _state(engine_store, "net_equity_issuance", _LAST, "S6")[0] is None


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
    admission_of = {entry.feature_id: entry.admission for entry in default_anomaly_catalog()}
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
            problems.append(f"{feature}/{code}: cataloged {admission_of[feature]} but the engine labels it "
                            "incomparable")

    assert problems == []
    # Every quarter-grid seed row plus the composition metric legs (146 operands).
    assert checked >= 140


def _incomparable_at_last_bucket(store, security_id: str) -> set[str]:
    return {row[0] for row in store.con.execute(
        """SELECT metric_code FROM derived_metric_values
           WHERE security_id = ? AND period_end = ? AND value_status = 'valid'
             AND value_origin = 'incomparable'""",
        [security_id, _ENDS[_LAST]]).fetchall()}


def test_split_basis_rows_are_the_only_ones_unproven_without_daily_bars(engine_store):
    """R1d: S10 (S1 without bars) leaves exactly the mirror's share-basis metrics unproven."""
    by_id = {entry.feature_id: entry for entry in default_anomaly_catalog()}
    windows = {definition.metric_code: definition.window for definition in default_derived_definitions()}
    shapes = derive_metric_shapes()
    unproven = _incomparable_at_last_bucket(engine_store, "S10") - _incomparable_at_last_bucket(engine_store, "S1")

    assert unproven == {
        code for code, shape in shapes.items()
        if shape.split_gated and shape.incomparable_reason is None and windows[code] in QUARTER_GRID_WINDOWS
    }
    # Every share-count and per-share comparison across filings, trailing EPS included.
    assert {"eps_diluted_q_growth_yoy", "eps_basic_q_growth_yoy", "eps_diluted_q_growth_yoy_accel",
            "eps_diluted_growth_yoy", "eps_diluted_ttm", "eps_basic_ttm", "share_issuance_1y",
            "share_issuance_3y", "shares_growth_yoy", "eps_cagr_3y", "piotroski_f"} <= unproven
    for code in unproven:
        # Same value, only the label differs: bars prove the basis, they change nothing.
        assert _state(engine_store, code, _LAST, "S10")[0] == pytest.approx(
            _state(engine_store, code, _LAST)[0], rel=1e-12), code
    for feature, code, blocked in _engine_labeled_operands():
        if code in unproven and not blocked:
            assert "split_basis" in by_id[feature].caveat_codes, (feature, code)
