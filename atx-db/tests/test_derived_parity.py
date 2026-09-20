"""Tier1-S3 T7: measured parity evidence and explicit retirement blockers."""

from __future__ import annotations

import datetime as dt
import importlib
import json
from collections import Counter
from dataclasses import replace

import pandas as pd
import pytest

from atx_db.derived_factor_projection import (
    FactorProjection,
    FactorProjectionOptions,
    compute_projection_rows,
    default_projections,
    load_projection_inputs,
    refresh_projected_factor_values,
)
from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
from atx_db.derived_registry import DERIVED_SOURCE_NAME, default_derived_definitions, seed_derived_metric_definitions
from atx_db.market_daily import MARKET_DAILY_SOURCE_NAME, MarketDailyOptions, refresh_market_daily_metrics

RELATIVE_TOLERANCE = 1e-9

PROJECTION_SOURCE = "atx-db derived factor projection v1"

# The 61 per-metric modules come in two shapes (audit S3.1c): 46 pandas modules
# expose ``load_<x>_inputs`` + ``compute_<x>_rows``, and 15 pure set-based
# modules expose only ``refresh_<x>_values``. ``annual_margin_change`` is a third
# case: it has the pandas pair and no refresh function at all. Each entry is
# (module, options class, kind, entry points, prerequisite refreshers); every
# name below was read out of the module on main, not guessed.
#
# ``asset_growth`` and ``net_operating_assets`` are added as prerequisites for
# several cases beyond what a literal reading of the brief's skeleton listed:
# ``beneish_m_score``, ``external_financing``, ``net_debt_financing`` and
# ``tax_to_book_income`` all read their "parent" annual-asset pair straight out
# of ``fundamental_factor_values`` (``json_extract`` on
# ``investment_conservative_asset_growth``'s own lineage), and ``rsst_accruals``
# reads ``quality_net_operating_assets``'s lineage the same way. Without running
# the parent refresh first those reads find nothing and the case always skips
# with "produced no rows" regardless of the fixture -- that skip message would
# be true but for the wrong reason. Running the parent once lets the *real*
# reason (a genuine formula/denominator difference, documented in
# ``KNOWN_DEFINITIONAL_SKIPS`` below) be the one that shows up if anyone ever
# removes the explicit skip and points the fixture at these cases.
MODULE_CASES = (
    (
        "altman_distress",
        "AltmanDistressOptions",
        "frame",
        ("load_altman_distress_inputs", "compute_altman_distress_rows"),
        (
            ("cash_flow_profitability", "refresh_cash_flow_profitability_values", "CashFlowProfitabilityOptions"),
            ("fundamental_signals", "refresh_fundamental_signal_values", "FundamentalSignalOptions"),
        ),
    ),
    (
        "net_operating_assets",
        "NetOperatingAssetsOptions",
        "frame",
        ("load_net_operating_assets_inputs", "compute_net_operating_assets_rows"),
        (),
    ),
    (
        "quarterly_working_capital_accruals",
        "QuarterlyWorkingCapitalAccrualsOptions",
        "frame",
        ("load_quarterly_working_capital_accruals_inputs", "compute_quarterly_working_capital_accruals_rows"),
        (),
    ),
    (
        "asset_turnover_change",
        "AssetTurnoverChangeOptions",
        "frame",
        ("load_asset_turnover_change_inputs", "compute_asset_turnover_change_rows"),
        (),
    ),
    (
        "annual_margin_change",
        "AnnualMarginChangeOptions",
        "frame",
        ("load_annual_margin_change_inputs", "compute_annual_margin_change_rows"),
        (),
    ),
    (
        "quarterly_gross_margin_change",
        "QuarterlyGrossMarginChangeOptions",
        "frame",
        ("load_quarterly_gross_margin_change_inputs", "compute_quarterly_gross_margin_change_rows"),
        (),
    ),
    (
        "quarterly_profitability_change",
        "QuarterlyProfitabilityChangeOptions",
        "frame",
        ("load_quarterly_profitability_change_inputs", "compute_quarterly_profitability_change_rows"),
        (),
    ),
    ("net_issuance", "NetIssuanceOptions", "frame", ("load_net_issuance_inputs", "compute_net_issuance_rows"), ()),
    ("net_payout", "NetPayoutOptions", "frame", ("load_net_payout_inputs", "compute_net_payout_rows"), ()),
    (
        "enterprise_yield",
        "EnterpriseYieldOptions",
        "frame",
        ("load_enterprise_yield_inputs", "compute_enterprise_yield_rows"),
        (),
    ),
    ("asset_growth", "AssetGrowthOptions", "frame", ("load_asset_growth_inputs", "compute_asset_growth_rows"), ()),
    (
        "beneish_m_score",
        "BeneishMScoreOptions",
        "refresh",
        ("refresh_beneish_m_score_values",),
        (("asset_growth", "refresh_asset_growth_values", "AssetGrowthOptions"),),
    ),
    (
        "rsst_accruals",
        "RsstAccrualsOptions",
        "refresh",
        ("refresh_rsst_accruals_values",),
        (("net_operating_assets", "refresh_net_operating_assets_values", "NetOperatingAssetsOptions"),),
    ),
    (
        "external_financing",
        "ExternalFinancingOptions",
        "refresh",
        ("refresh_external_financing_values",),
        (("asset_growth", "refresh_asset_growth_values", "AssetGrowthOptions"),),
    ),
    (
        "net_debt_financing",
        "NetDebtFinancingOptions",
        "refresh",
        ("refresh_net_debt_financing_values",),
        (("asset_growth", "refresh_asset_growth_values", "AssetGrowthOptions"),),
    ),
    ("rd_intensity", "RdIntensityOptions", "refresh", ("refresh_rd_intensity_values",), ()),
    ("rd_increase", "RdIncreaseOptions", "refresh", ("refresh_rd_increase_values",), ()),
    ("tax_expense_momentum", "TaxExpenseMomentumOptions", "refresh", ("refresh_tax_expense_momentum_values",), ()),
    (
        "tax_to_book_income",
        "TaxToBookIncomeOptions",
        "refresh",
        ("refresh_tax_to_book_income_values",),
        (("asset_growth", "refresh_asset_growth_values", "AssetGrowthOptions"),),
    ),
)

# Every retired-module case the parity harness does NOT expect to reproduce a
# raw_value/value match for, with the concrete reason. Two shapes of reason:
#
#  - "infra": the module's own load function reads a table this task does not
#    populate (``fundamental_ttm_points``, ``enterprise_value``, or another
#    retired module's own factor chain such as ``quarterly_cash_profitability``
#    / ``quarterly_revenue_margin_confirmation``). Wiring those is a separate,
#    larger undertaking (each is itself a multi-hundred-line SQL module) that is
#    out of this task's file set; the case is unreachable, not mismatched.
#  - "definition": the module's own formula was read in full and provably
#    computes something the engine's declarative metric does not -- a different
#    denominator convention, a different transform, or a different statistic
#    entirely. Listed with the specific divergence so Task 8's controller can
#    rule on each one (accept the divergence, retire the module differently, or
#    change the engine formula) without re-deriving the algebra.
KNOWN_DEFINITIONAL_SKIPS = {
    "altman_distress": (
        "infra: load_altman_distress_inputs joins fundamental_ttm_points for "
        "ebit_ttm/revenue_ttm and cash_flow_profitability's own 'assets' lineage "
        "field; fundamental_ttm_points is not populated by this task's fixture "
        "or by any of MODULE_CASES' prerequisites"
    ),
    "quarterly_working_capital_accruals": (
        "fixture + definition + projection: missing quarterly_cash_profitability lineage; "
        "legacy computes quarterly (dAR+dInventory-dDeferredRevenue-dAP)/lagged_assets, "
        "engine computes annual change of broad operating working capital/average_assets. "
        "Legacy raw_value is un-oriented but value negates it; one projection orientation "
        "cannot preserve both columns even after the formula is aligned"
    ),
    "asset_turnover_change": (
        "definition: the module scales current_revenue/prior_revenue by a single "
        "lagged total_assets point (prior_assets, prior2_assets from consecutive "
        "annual filings); the engine's asset_turnover = revenue_ttm/"
        "total_assets_avg2 divides by the AVERAGE of the current and "
        "year-ago total_assets. The two denominators are provably unequal "
        "whenever total_assets changes year over year (required for "
        "asset_growth's own zscore to be non-degenerate), so no fixture can "
        "reconcile them without making assets flat"
    ),
    "quarterly_gross_margin_change": (
        "fixture + definition: missing quarterly_revenue_margin_confirmation's "
        "quarterly_operating_profitability/quarterly_revenue_growth parent rows; "
        "legacy compares same-quarter gross margins while the engine compares TTM margins"
    ),
    "quarterly_profitability_change": (
        "fixture + definition: missing quarterly_operating_profitability parent rows; "
        "legacy uses quarterly (revenue-cogs-sga+R&D)/one-quarter-lagged assets, "
        "engine uses TTM operating income/average current and year-ago assets"
    ),
    "net_issuance": (
        "definition: raw_value is -ln(current_shares*split_index / "
        "prior_shares*split_index) -- a LOG share-count change. The engine's "
        "shares_growth_yoy is yoy(shares_outstanding_period_end) = "
        "(current-prior)/abs(prior), an ARITHMETIC percent change. ln(a/b) != "
        "(a-b)/b except in the trivial a==b case, so no non-trivial fixture "
        "value can make the two equal at 1e-9"
    ),
    "net_payout": (
        "infra: load_net_payout_inputs requires ALL THREE of common_div_paid, "
        "share_repurchases and stock_issuance to be reported for the SAME "
        "accession in fundamental_ttm_points (a table this task does not "
        "populate)"
    ),
    "enterprise_yield": (
        "infra: load_enterprise_yield_inputs reads the enterprise_value table "
        "(a separate materialized dataset this task does not populate) joined "
        "to fundamental_ttm_points, and the gross_profit/operating_cash_flow "
        "variants additionally read profitability_gross_profitability / "
        "profitability_operating_cash_flow_to_assets factor lineage -- three "
        "more out-of-scope retired-module dependencies"
    ),
    "rsst_accruals": (
        "definition (intentional S3 T4 correction): legacy uses -delta(NOA)/average_assets; "
        "the engine additionally includes delta(long_term_investments)-delta(total_debt). "
        "A fixture making those changes cancel would hide the semantic difference"
    ),
    "external_financing": (
        "definition: raw_value is -financing_cash_flow/prior_assets, where "
        "financing_cash_flow is the single aggregate 'net cash from financing "
        "activities' line (dividends, buybacks, issuance and debt all netted "
        "together) and prior_assets is a single lagged annual point. The "
        "engine's external_financing sums only share issuance/repurchase and "
        "debt issuance/repayment (explicitly excluding dividends) and scales by "
        "total_assets_avg2 (the AVERAGE of current and year-ago assets, not a "
        "single lagged point) -- two independent divergences from the module"
    ),
    "net_debt_financing": (
        "definition: raw_value is -(debt_issued+debt_repaid)/prior_assets using "
        "a single lagged annual total_assets point; the engine's "
        "net_debt_issuance divides by total_assets_avg2 (the average of current "
        "and year-ago assets). Same avg2-vs-single-point denominator divergence "
        "as asset_turnover_change and external_financing"
    ),
    "rd_increase": (
        "definition: raw_value is a binary 0.0/1.0 'large R&D increase' event "
        "indicator (a threshold test in the module's own SQL CASE expression); "
        "the engine's rd_expense_growth_yoy is a continuous year-over-year "
        "growth ratio. A discrete indicator and a continuous ratio cannot "
        "satisfy the raw-value parity check by construction"
    ),
    "tax_expense_momentum": (
        "definition: raw_value is (current_tax-prior_tax)/prior_assets -- the "
        "level of the tax-expense change scaled by BALANCE-SHEET size. The "
        "engine's tax_expense_change_yoy is yoy(income_tax_ttm) = "
        "(current-prior)/abs(prior) -- a RELATIVE percent change scaled by the "
        "prior tax expense itself. The two use unrelated denominators (total "
        "assets vs. prior tax) and cannot coincide except by chance at a single "
        "date"
    ),
    "tax_to_book_income": (
        "definition: raw_value estimates after-tax taxable income from current "
        "tax expense via a hard-coded statutory-rate transform "
        "(current_tax*(1-rate)/rate, rate=0.35 pre-2018 else 0.21) divided by "
        "book net income. The engine's tax_to_book_income is the plain ratio "
        "safe_div(income_tax_ttm, net_income_ttm) with no rate transform at "
        "all -- the module's formula is a different statistic, not a "
        "differently-scoped version of the same one"
    ),
}


def _module_rows(store, case) -> pd.DataFrame:
    """Return the module's own (factor_id, security_id, as_of_date, raw_value, value) rows."""
    module_name, options_name, kind, entry_points, prerequisites = case
    module = importlib.import_module(f"atx_db.{module_name}")
    for prerequisite_module, prerequisite_function, prerequisite_options in prerequisites:
        prerequisite = importlib.import_module(f"atx_db.{prerequisite_module}")
        getattr(prerequisite, prerequisite_function)(store, getattr(prerequisite, prerequisite_options)())
    options = getattr(module, options_name)()
    if kind == "frame":
        loader_name, computer_name = entry_points
        frame = getattr(module, computer_name)(getattr(module, loader_name)(store, options), options)
        result = frame[["factor_id", "security_id", "as_of_date", "raw_value", "value"]].copy()
    else:
        getattr(module, entry_points[0])(store, options)
        result = store.con.execute(
            "SELECT factor_id, security_id, as_of_date, raw_value, value "
            "FROM fundamental_factor_values WHERE source = ? ORDER BY 1, 2, 3",
            [module.SOURCE_NAME],
        ).df()
    # Normalize to plain python date objects regardless of source: the
    # "frame" modules already return date objects (object dtype), but
    # DuckDB's .df() returns DATE columns as datetime64[us] for the "refresh"
    # modules' own SELECT above -- merging object against datetime64 later
    # raises rather than aligning, so pin one dtype here for both shapes.
    if not result.empty:
        result["as_of_date"] = pd.to_datetime(result["as_of_date"]).dt.date
    return result


def test_every_module_case_resolves_to_real_symbols():
    """The case table is the contract; a rename upstream must fail loudly here."""
    for module_name, options_name, kind, entry_points, prerequisites in MODULE_CASES:
        module = importlib.import_module(f"atx_db.{module_name}")
        assert hasattr(module, options_name), f"{module_name}.{options_name}"
        assert hasattr(module, "SOURCE_NAME"), f"{module_name}.SOURCE_NAME"
        assert kind in ("frame", "refresh")
        for name in entry_points:
            assert hasattr(module, name), f"{module_name}.{name}"
        for prerequisite_module, prerequisite_function, prerequisite_options in prerequisites:
            prerequisite = importlib.import_module(f"atx_db.{prerequisite_module}")
            assert hasattr(prerequisite, prerequisite_function)
            assert hasattr(prerequisite, prerequisite_options)


def test_every_projection_metric_exists_in_the_catalog():
    codes = {definition.metric_code for definition in default_derived_definitions()}
    missing = sorted(
        projection.metric_code for projection in default_projections() if projection.metric_code not in codes
    )
    assert missing == []


def test_every_projection_orientation_is_plus_or_minus_one():
    assert {projection.orientation for projection in default_projections()} == {1, -1}


def test_projection_covers_every_retired_module(retired_modules):
    projections = default_projections()
    counts = Counter(projection.retired_module for projection in projections)
    expected = dict.fromkeys(retired_modules, 1)
    expected.update(annual_margin_change=3, enterprise_yield=4, KEPT=1)
    assert counts == expected
    assert len(projections) == len({projection.factor_id for projection in projections}) == 24


def test_every_skipped_module_has_a_documented_reason(retired_modules):
    """Every case this harness cannot compare must say why (brief's own requirement)."""
    tested = {case[0] for case in MODULE_CASES}
    assert set(KNOWN_DEFINITIONAL_SKIPS) <= tested
    for module_name in retired_modules:
        if module_name not in {
            "beneish_m_score",
            "asset_growth",
            "net_operating_assets",
            "annual_margin_change",
            "rd_intensity",
        }:
            assert module_name in KNOWN_DEFINITIONAL_SKIPS, module_name


# Mark known gaps during collection so pytest does not build the expensive
# warehouse merely to skip its comparison. These marks are NOT parity evidence.
PARITY_CASES = [
    pytest.param(
        case,
        id=case[0],
        marks=pytest.mark.skip(reason=KNOWN_DEFINITIONAL_SKIPS[case[0]])
        if KNOWN_DEFINITIONAL_SKIPS.get(case[0])
        else (),
    )
    for case in MODULE_CASES
]


@pytest.fixture
def retired_modules():
    return {
        "altman_distress",
        "annual_margin_change",
        "asset_turnover_change",
        "beneish_m_score",
        "enterprise_yield",
        "external_financing",
        "net_debt_financing",
        "net_issuance",
        "net_operating_assets",
        "net_payout",
        "quarterly_gross_margin_change",
        "quarterly_profitability_change",
        "quarterly_working_capital_accruals",
        "rd_increase",
        "rd_intensity",
        "rsst_accruals",
        "tax_expense_momentum",
        "tax_to_book_income",
    }


# --- Synthetic fixture construction -----------------------------------------
#
# 25 securities x 4 fiscal years (2019-2022, 16 fiscal quarters) x monthly
# rebalances from 2019-01 through 2023-06. Every fact is written to BOTH
# fundamental_standardized (the engine's read path, keyed by the item
# registry's canonical_code) and fundamental_statement_points (the retired
# modules' read path, keyed by their own, older, short canonical_metric
# vocabulary -- e.g. 'ar' for accounts_receivable, 'cogs' for
# cost_of_revenue_cogs). Where the two vocabularies use the SAME string
# (revenue, operating_income, total_assets, total_liabilities, current_assets)
# one row serves both readers. Where they differ, the same value is written
# under BOTH codes into BOTH tables (the second copy carries basis='annual' in
# fundamental_standardized so the engine's own `basis IN ('quarterly',
# 'instant')` filter ignores it) -- this is what makes the parity_warehouse
# fixture's own "identical facts in both layers" assertion true by
# construction while still letting the retired module read its native alias.
#
# Duration (flow) items feeding the engine's ttm() rollups are "front-loaded":
# the full annual amount is placed at the Q4 (fiscal-year-end) quarter and
# zero at Q1-Q3. A trailing 4-quarter sum from ANY quarter of the following
# year then always contains exactly one non-zero (the most recent Q4), so
# ttm(x) equals "the most recently filed annual x" at every month -- exactly
# the annual-filing-refresh modules' own "held constant until the next 10-K"
# behavior. Instant (balance) items are simply held constant across the 4
# quarters of one fiscal year and change only at the year boundary, which
# both readers see identically at the fiscal year-end (10-K) point and lets
# the engine's own quarter-to-quarter reads inside a year stay well-defined.

_SECURITY_COUNT = 25
_YEARS = (2019, 2020, 2021, 2022)
_QUARTER_ENDS = {
    1: (3, 31),
    2: (6, 30),
    3: (9, 30),
    4: (12, 31),
}
_ANNUAL_FORMS = ("10-K", "10-K/A", "10-KT")
_QUARTERLY_FORMS = ("10-Q", "10-Q/A", "10-QT")


def _security_id(i: int) -> str:
    return f"PARITY{i:03d}"


def _symbol(i: int) -> str:
    return f"PSY{i:03d}"


def _scale(i: int) -> float:
    return 1.0 + 0.02 * i


def _quarter_period_end(year: int, quarter: int) -> dt.date:
    month, day = _QUARTER_ENDS[quarter]
    return dt.date(year, month, day)


def _available_at(period_end: dt.date, *, annual: bool) -> dt.datetime:
    lag_days = 50 if annual else 35
    return dt.datetime.combine(period_end + dt.timedelta(days=lag_days), dt.time(21, 0))


class _FactWriter:
    """Accumulates rows for fundamental_standardized and fundamental_statement_points."""

    def __init__(self) -> None:
        self.standardized: list[dict] = []
        self.points: list[dict] = []
        self._point_seq = 0

    def _next_point_id(self, security_id: str) -> str:
        self._point_seq += 1
        return f"{security_id}-pt-{self._point_seq}"

    def _add_standardized(
        self, security_id: str, code: str, basis: str, period_end: dt.date, value: float, available_at: dt.datetime
    ) -> None:
        self.standardized.append(
            {
                "standardized_id": f"{security_id}|{code}|{basis}|{period_end}",
                "source": "test",
                "security_id": security_id,
                "item_id": 1,
                "canonical_code": code,
                "basis": basis,
                "period_end": period_end,
                "value": value,
                "as_of_date": available_at.date(),
                "available_at": available_at,
                "input_codes_json": "[]",
                "input_item_ids_json": "[]",
                "rule_id": "r",
                "combination_rule": "direct",
                "revision_sequence": 1,
                "is_latest_revision": True,
            }
        )

    def _add_point(
        self,
        security_id: str,
        symbol: str,
        code: str,
        period_type: str,
        period_start: dt.date | None,
        period_end: dt.date,
        value: float,
        available_at: dt.datetime,
        form: str,
        accession: str,
    ) -> None:
        self.points.append(
            {
                "statement_point_id": self._next_point_id(security_id),
                "fact_revision_id": self._next_point_id(security_id),
                "revision_group_id": self._next_point_id(security_id),
                "source": "test",
                "security_id": security_id,
                "symbol": symbol,
                "cik": "0000000000",
                "statement_type": "test",
                "statement_section": "test",
                "canonical_metric": code,
                "canonical_label": code,
                "taxonomy": "us-gaap",
                "concept": code,
                "unit": "USD",
                "unit_type": "USD",
                "period_type": period_type,
                "normal_balance": "debit",
                "period_start": period_start,
                "period_end": period_end,
                "as_of_date": period_end,
                "available_at": available_at,
                "fiscal_year": period_end.year,
                "fiscal_period": "FY" if form in _ANNUAL_FORMS else "Q",
                "form": form,
                "accession_number": accession,
                "source_accession": accession,
                "filed_date": available_at.date(),
                "revision_sequence": 1,
                "revision_count": 1,
                "is_latest_revision": True,
                "is_value_changed": True,
                "raw_value": value,
                "value": value,
                "previous_raw_value": None,
                "previous_value": None,
                "value_delta": None,
                "value_delta_percent": None,
                "run_id": None,
                "source_url": "http://test.invalid",
                "source_loaded_at": available_at,
            }
        )

    def fact(
        self,
        *,
        security_id: str,
        symbol: str,
        engine_code: str,
        module_code: str,
        period_type: str,
        period_end: dt.date,
        value: float,
        annual: bool,
        accession: str,
        period_start: dt.date | None = None,
    ) -> None:
        """Write one fact under BOTH vocabularies into BOTH tables (see module docstring)."""
        available_at = _available_at(period_end, annual=annual)
        form = _ANNUAL_FORMS[0] if annual else _QUARTERLY_FORMS[0]
        basis = "instant" if period_type == "instant" else "quarterly"
        self._add_standardized(security_id, engine_code, basis, period_end, value, available_at)
        self._add_point(
            security_id,
            symbol,
            engine_code,
            period_type,
            period_start,
            period_end,
            value,
            available_at,
            form,
            accession,
        )
        if module_code != engine_code:
            self._add_standardized(security_id, module_code, "annual", period_end, value, available_at)
            self._add_point(
                security_id,
                symbol,
                module_code,
                period_type,
                period_start,
                period_end,
                value,
                available_at,
                form,
                accession,
            )


def _write_parity_facts(store) -> None:
    writer = _FactWriter()
    bars: list[dict] = []
    market_caps: list[dict] = []
    membership: list[dict] = []
    shares_history: list[dict] = []

    for i in range(_SECURITY_COUNT):
        security_id = _security_id(i)
        symbol = _symbol(i)
        scale = _scale(i)
        share_count = 10_000_000.0 * scale

        membership.append(
            {
                "universe_id": "us_common_equity_liquid_v1",
                "security_id": security_id,
                "symbol": symbol,
                "valid_from": dt.date(2018, 1, 1),
                "valid_to": None,
                "as_of_date": dt.date(2018, 1, 1),
                "is_member": True,
                "reason": "member",
                "rules_json": "{}",
                "decision_count": 1,
                "available_at": dt.datetime(2018, 1, 1, 0, 0),
                "source": "test",
                "run_id": None,
            }
        )
        shares_history.append(
            {
                "share_history_id": f"{security_id}-shares",
                "source": "test",
                "security_id": security_id,
                "symbol": symbol,
                "cik": "0000000000",
                "share_count_type": "shares_outstanding",
                "taxonomy": "dei",
                "concept": "EntityCommonStockSharesOutstanding",
                "unit": "shares",
                "period_type": "instant",
                "period_start": None,
                "period_end": dt.date(2018, 6, 1),
                "effective_date": dt.date(2018, 6, 1),
                "as_of_date": dt.date(2018, 6, 1),
                "available_at": dt.datetime(2018, 6, 1, 21, 0),
                "fiscal_year": 2018,
                "fiscal_period": "Q2",
                "form": "10-Q",
                "accession_number": f"{security_id}-shares-acc",
                "revision_sequence": 1,
                "revision_count": 1,
                "is_latest_revision": True,
                "share_count": share_count,
                "source_url": "http://test.invalid",
            }
        )

        # Per-security annual levels (thousands of USD, arbitrary units). All
        # grow monotonically year over year so asset_growth / margin-change /
        # beneish's year-over-year ratios are non-degenerate (never a constant
        # 0 or 1 that a std()==0 cross-section would zscore to NaN and drop).
        #
        # ``extra`` adds a small PER-ITEM, non-proportional offset on top of
        # the uniform per-security ``scale``. Every one of the ratio metrics
        # under test (noa_to_assets, beneish's eight indices, ...) divides
        # one of these items by another; with a single multiplicative
        # ``scale`` applied to every item, ``scale`` cancels out of EVERY
        # such ratio algebraically, so all 25 securities would carry the
        # mathematically identical raw_value. In floating point that
        # cancellation is inexact (module and engine reach the same ratio by
        # different arithmetic paths -- SQL division vs pandas division --
        # landing within 1e-9 of each other but essentially never bit-equal),
        # so the cross-sectional std the module and engine each compute is
        # itself a near-zero rounding artifact, computed by two DIFFERENT
        # numerical paths (DuckDB's stddev_samp for the SQL-based "refresh"
        # modules; pandas' std for the "frame" modules and for the engine's
        # own projection). Dividing by two independently-rounded near-zero
        # numbers amplifies that sub-1e-9 raw_value noise into an O(1)
        # zscore mismatch -- not a bug in either implementation, just an
        # unstable computation on a degenerate (zero-variance) input. The
        # ``extra`` offset breaks the proportionality so the cross-sectional
        # spread of every ratio is REAL (driven by the fixture's own
        # variation, not by rounding), which is what every actual
        # multi-security universe looks like.
        def _level(base: float, step: float, year_index: int, *, extra: float = 0.0, _scale: float = scale) -> float:
            return _scale * (base + step * year_index) + extra

        for year_index, year in enumerate(_YEARS):
            # Duration (annual, front-loaded) bases for THIS fiscal year --
            # see the front-load comment below for why only quarter 4 uses
            # these directly.
            revenue = _level(500.0, 40.0, year_index, extra=0.25 * i)
            cogs = _level(300.0, 20.0, year_index, extra=0.15 * i)
            gross_profit = revenue - cogs
            operating_income = _level(100.0, 10.0, year_index, extra=0.05 * i)
            net_income = _level(70.0, 5.0, year_index, extra=0.03 * i)
            sga = _level(60.0, 3.0, year_index, extra=0.03 * i)
            rd_expense = _level(30.0, 2.0, year_index, extra=0.015 * i)
            da_cf = _level(40.0, 3.0, year_index, extra=0.02 * i)
            cfo = _level(90.0, 6.0, year_index, extra=0.045 * i)

            for quarter in (1, 2, 3, 4):
                period_end = _quarter_period_end(year, quarter)
                annual = quarter == 4
                accession = f"{security_id}-{year}-Q{quarter}"

                # Instant (balance-sheet) items: a real annual filer's Q1-Q3
                # 10-Qs report balances observed AS OF that quarter, and an
                # annual-only module never learns the NEW fiscal year-end
                # balance until the year's own 10-K is filed (Q4).  Modeling
                # Q1-Q3 as already carrying the NEW year's balance (as an
                # earlier version of this fixture did) makes the engine's
                # continuous quarter-to-quarter reads race ahead of what an
                # annual-only module could possibly know yet, at whichever
                # month the calendar year turns over -- a real, measurable
                # divergence, not a rounding artifact. Instead, Q1-Q3 of
                # fiscal year Y hold the value effective since year Y-1's
                # 10-K (i.e. year_index - 1, clamped at the fixture's first
                # year), and only Q4 steps to year Y's own value -- so the
                # engine's per-quarter grid and the annual module's
                # once-a-year refresh agree at every rebalance date.
                instant_index = year_index if quarter == 4 else max(year_index - 1, 0)
                # Vary asset growth across securities too: proportional assets
                # make its cross-sectional standard deviation rounding noise.
                total_assets = _level(1000.0, 60.0, instant_index, extra=0.40 * i)
                total_liabilities = _level(600.0, 30.0, instant_index, extra=0.30 * i)
                cash_st = _level(80.0, 5.0, instant_index, extra=0.05 * i)
                st_debt = _level(40.0, 2.0, instant_index, extra=0.02 * i)
                lt_debt = _level(150.0, 5.0, instant_index, extra=0.08 * i)
                current_assets = _level(300.0, 15.0, instant_index, extra=0.15 * i)
                ppe_net = _level(400.0, 20.0, instant_index, extra=0.20 * i)
                receivables = _level(90.0, 4.0, instant_index, extra=0.04 * i)
                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="total_assets",
                    module_code="total_assets",
                    period_type="instant",
                    period_end=period_end,
                    value=total_assets,
                    annual=annual,
                    accession=accession,
                )
                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="total_liabilities",
                    module_code="total_liabilities",
                    period_type="instant",
                    period_end=period_end,
                    value=total_liabilities,
                    annual=annual,
                    accession=accession,
                )
                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="cash_and_st_investments",
                    module_code="cash_st_inv",
                    period_type="instant",
                    period_end=period_end,
                    value=cash_st,
                    annual=annual,
                    accession=accession,
                )
                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="short_term_debt",
                    module_code="st_debt",
                    period_type="instant",
                    period_end=period_end,
                    value=st_debt,
                    annual=annual,
                    accession=accession,
                )
                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="long_term_debt",
                    module_code="lt_debt",
                    period_type="instant",
                    period_end=period_end,
                    value=lt_debt,
                    annual=annual,
                    accession=accession,
                )
                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="current_assets",
                    module_code="current_assets",
                    period_type="instant",
                    period_end=period_end,
                    value=current_assets,
                    annual=annual,
                    accession=accession,
                )
                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="pp_and_e_net",
                    module_code="ppe_net",
                    period_type="instant",
                    period_end=period_end,
                    value=ppe_net,
                    annual=annual,
                    accession=accession,
                )
                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="accounts_receivable",
                    module_code="ar",
                    period_type="instant",
                    period_end=period_end,
                    value=receivables,
                    annual=annual,
                    accession=accession,
                )

                # Duration (flow) items: front-loaded onto Q4 (annual value),
                # zero at Q1-Q3, so the engine's rolling ttm() always equals
                # "the most recently filed annual figure" (see module docstring).
                is_q4 = quarter == 4
                period_start = dt.date(year, 1, 1) if is_q4 else dt.date(year, {1: 1, 2: 4, 3: 7, 4: 10}[quarter], 1)
                q_revenue = revenue if is_q4 else 0.0
                q_cogs = cogs if is_q4 else 0.0
                q_gross_profit = gross_profit if is_q4 else 0.0
                q_operating_income = operating_income if is_q4 else 0.0
                q_net_income = net_income if is_q4 else 0.0
                q_sga = sga if is_q4 else 0.0
                q_rd = rd_expense if is_q4 else 0.0
                q_da = da_cf if is_q4 else 0.0
                q_cfo = cfo if is_q4 else 0.0

                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="revenue",
                    module_code="revenue",
                    period_type="duration",
                    period_end=period_end,
                    value=q_revenue,
                    annual=annual,
                    accession=accession,
                    period_start=period_start,
                )
                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="cost_of_revenue_cogs",
                    module_code="cogs",
                    period_type="duration",
                    period_end=period_end,
                    value=q_cogs,
                    annual=annual,
                    accession=accession,
                    period_start=period_start,
                )
                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="gross_profit__1004",
                    module_code="gross_profit",
                    period_type="duration",
                    period_end=period_end,
                    value=q_gross_profit,
                    annual=annual,
                    accession=accession,
                    period_start=period_start,
                )
                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="operating_income",
                    module_code="operating_income",
                    period_type="duration",
                    period_end=period_end,
                    value=q_operating_income,
                    annual=annual,
                    accession=accession,
                    period_start=period_start,
                )
                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="net_income_total",
                    module_code="net_income",
                    period_type="duration",
                    period_end=period_end,
                    value=q_net_income,
                    annual=annual,
                    accession=accession,
                    period_start=period_start,
                )
                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="sg_and_a",
                    module_code="sga",
                    period_type="duration",
                    period_end=period_end,
                    value=q_sga,
                    annual=annual,
                    accession=accession,
                    period_start=period_start,
                )
                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="r_and_d_expense",
                    module_code="rd_expense",
                    period_type="duration",
                    period_end=period_end,
                    value=q_rd,
                    annual=annual,
                    accession=accession,
                    period_start=period_start,
                )
                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="d_and_a_cash_flow",
                    module_code="da_cf",
                    period_type="duration",
                    period_end=period_end,
                    value=q_da,
                    annual=annual,
                    accession=accession,
                    period_start=period_start,
                )
                writer.fact(
                    security_id=security_id,
                    symbol=symbol,
                    engine_code="cash_flow_from_operations",
                    module_code="operating_cash_flow",
                    period_type="duration",
                    period_end=period_end,
                    value=q_cfo,
                    annual=annual,
                    accession=accession,
                    period_start=period_start,
                )

        # Monthly bars, 2019-01 through 2023-06: the last calendar day of
        # every month. Price grows slowly and smoothly so market_cap (used by
        # rd_intensity) is always positive and finite.
        month_index = 0
        year_cursor, month_cursor = 2019, 1
        while (year_cursor, month_cursor) <= (2023, 6):
            if month_cursor == 12:
                next_month = dt.date(year_cursor + 1, 1, 1)
            else:
                next_month = dt.date(year_cursor, month_cursor + 1, 1)
            trade_date = next_month - dt.timedelta(days=1)
            close = scale * (20.0 + 0.15 * month_index)
            available_at = dt.datetime.combine(trade_date, dt.time(21, 0))
            bars.append(
                {
                    "source": "test",
                    "security_id": security_id,
                    "vendor_security_id": None,
                    "symbol": symbol,
                    "trade_date": trade_date,
                    "open": close,
                    "high": close,
                    "low": close,
                    "close": close,
                    "adjusted_close": close,
                    "volume": 1_000_000,
                    "vwap": close,
                    "dividend_amount": None,
                    "split_factor": None,
                    "is_adjusted": False,
                    "available_at": available_at,
                    "run_id": None,
                }
            )
            market_cap_value = close * share_count
            market_caps.append(
                {
                    "market_cap_id": f"{security_id}-{trade_date}",
                    "source": "test",
                    "price_source": "test",
                    "share_source": "test",
                    "security_id": security_id,
                    "symbol": symbol,
                    "trade_date": trade_date,
                    "close": close,
                    "share_count": share_count,
                    "share_count_type_used": "shares_outstanding",
                    "market_cap": market_cap_value,
                    "is_latest_revision": True,
                    "as_of_date": trade_date,
                    "available_at": available_at,
                    "price_available_at": available_at,
                    "share_available_at": dt.datetime(2018, 6, 1, 21, 0),
                    "price_run_id": None,
                    "share_run_id": None,
                    "share_history_id": f"{security_id}-shares",
                    "input_codes_json": "[]",
                    "input_lineage_json": "{}",
                    "run_id": None,
                }
            )
            month_index += 1
            year_cursor, month_cursor = next_month.year, next_month.month

    standardized_df = pd.DataFrame(writer.standardized)
    points_df = pd.DataFrame(writer.points)
    bars_df = pd.DataFrame(bars)
    market_cap_df = pd.DataFrame(market_caps)
    membership_df = pd.DataFrame(membership)
    shares_df = pd.DataFrame(shares_history)

    from atx_db.warehouse import insert_frame

    insert_frame(store, standardized_df, "fundamental_standardized", "parity_standardized_insert")
    insert_frame(store, points_df, "fundamental_statement_points", "parity_points_insert")
    insert_frame(store, bars_df, "equity_daily_bars", "parity_bars_insert")
    insert_frame(store, market_cap_df, "market_cap", "parity_market_cap_insert")
    insert_frame(store, membership_df, "universe_membership", "parity_membership_insert")
    insert_frame(store, shares_df, "shares_outstanding_history", "parity_shares_insert")


@pytest.fixture
def parity_facts():
    """A callable that writes one list of quarterly facts into a store (see module docstring)."""
    return _write_parity_facts


@pytest.fixture
def parity_warehouse(tmp_store, parity_facts):
    """A warehouse whose statement-points and standardized layers carry identical facts.

    The per-metric modules read ``fundamental_statement_points``; the engine reads
    ``fundamental_standardized``. Parity at 1e-9 is only meaningful when both
    tables carry the same numbers, so the fixture writes both from one list and
    asserts the precondition before any comparison runs.
    """
    seed_derived_metric_definitions(tmp_store)
    parity_facts(tmp_store)
    mismatched = tmp_store.con.execute(
        """
        SELECT count(*) FROM fundamental_standardized s
        FULL OUTER JOIN fundamental_statement_points p
          ON p.security_id = s.security_id AND p.canonical_metric = s.canonical_code
         AND p.period_end = s.period_end
        WHERE s.value IS DISTINCT FROM p.value
        """
    ).fetchone()[0]
    assert mismatched == 0, "the parity fixture must write identical facts to both layers"
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    refresh_projected_factor_values(tmp_store, FactorProjectionOptions())
    return tmp_store


@pytest.mark.parametrize("case", PARITY_CASES)
def test_engine_metric_matches_the_module_raw_value(parity_warehouse, case, record_property):
    module_name = case[0]
    reason = KNOWN_DEFINITIONAL_SKIPS.get(module_name)
    if reason:
        pytest.skip(reason)
    expected = _module_rows(parity_warehouse, case)
    if expected.empty:
        pytest.skip(f"{module_name} produced no rows on the parity fixture")

    projected_ids = {
        projection.factor_id
        for projection in default_projections()
        if projection.retired_module == ("KEPT" if module_name == "asset_growth" else module_name)
    }
    actual = parity_warehouse.con.execute(
        "SELECT factor_id, security_id, as_of_date, raw_value FROM fundamental_factor_values "
        "WHERE source = ? ORDER BY 1, 2, 3",
        [PROJECTION_SOURCE],
    ).df()
    # DuckDB's .df() returns DATE columns as datetime64[us]; the pandas-side
    # modules normalize as_of_date to plain python date objects (object
    # dtype) before returning their frame. Align both sides to date before
    # merging on it, or pandas refuses to merge object against datetime64.
    actual["as_of_date"] = pd.to_datetime(actual["as_of_date"]).dt.date

    merged = expected[["factor_id", "security_id", "as_of_date", "raw_value"]].merge(
        actual, on=["factor_id", "security_id", "as_of_date"], suffixes=("_module", "_engine")
    )
    compared = merged[merged["factor_id"].isin(projected_ids)]
    assert not compared.empty, f"no overlapping rows for {module_name}"
    assert set(compared["factor_id"]) == projected_ids, "every mapped factor must have raw-value evidence"
    assert compared.groupby("factor_id")["security_id"].nunique().min() >= 20
    difference = (compared["raw_value_engine"] - compared["raw_value_module"]).abs()
    scale = compared["raw_value_module"].abs().clip(lower=1.0)
    record_property(
        "raw_evidence",
        json.dumps(
            {
                "module": module_name,
                "factors": sorted(compared["factor_id"].unique()),
                "rows": len(compared),
                "securities": int(compared["security_id"].nunique()),
                "dates": int(compared["as_of_date"].nunique()),
                "maximum_scaled_error": float((difference / scale).max()),
            }
        ),
    )
    assert (difference / scale).max() <= RELATIVE_TOLERANCE


@pytest.mark.parametrize("case", PARITY_CASES)
def test_projection_matches_module_value_where_the_cohort_matches(parity_warehouse, case, record_property):
    """The z-scored column is reproduced when the two grids select the same cohort."""
    module_name = case[0]
    reason = KNOWN_DEFINITIONAL_SKIPS.get(module_name)
    if reason:
        pytest.skip(reason)
    expected = _module_rows(parity_warehouse, case)
    if expected.empty:
        pytest.skip(f"{module_name} produced no rows on the parity fixture")
    actual = parity_warehouse.con.execute(
        "SELECT factor_id, security_id, as_of_date, value FROM fundamental_factor_values WHERE source = ?",
        [PROJECTION_SOURCE],
    ).df()
    actual["as_of_date"] = pd.to_datetime(actual["as_of_date"]).dt.date
    merged = expected[["factor_id", "security_id", "as_of_date", "value"]].merge(
        actual, on=["factor_id", "security_id", "as_of_date"], suffixes=("_module", "_engine")
    )
    if merged.empty:
        pytest.skip(f"{module_name}: no overlapping rebalance rows")
    module_sizes = expected.groupby(["factor_id", "as_of_date"]).size()
    actual_sizes = actual.groupby(["factor_id", "as_of_date"]).size()
    merged_sizes = merged.groupby(["factor_id", "as_of_date"]).size()
    same_cohort = merged_sizes.index[
        merged_sizes.eq(module_sizes.reindex(merged_sizes.index))
        & merged_sizes.eq(actual_sizes.reindex(merged_sizes.index))
    ]
    aligned = merged.set_index(["factor_id", "as_of_date"]).loc[same_cohort]
    if aligned.empty:
        pytest.skip(f"{module_name}: no rebalance date where both grids select the same cohort")
    difference = (aligned["value_engine"] - aligned["value_module"]).abs()
    record_property(
        "zscore_evidence",
        json.dumps(
            {
                "module": module_name,
                "rows": len(aligned),
                "maximum_error": float(difference.max()),
            }
        ),
    )
    assert difference.max() <= 1e-9


def test_projection_output_has_the_canonical_fifteen_column_shape(parity_warehouse):
    from atx_db.derived_factor_projection import PROJECTION_OUTPUT_COLUMNS

    assert PROJECTION_OUTPUT_COLUMNS == (
        "factor_value_id",
        "factor_id",
        "factor_name",
        "family",
        "security_id",
        "symbol",
        "as_of_date",
        "raw_value",
        "value",
        "available_at",
        "input_ids_json",
        "input_lineage_json",
        "is_latest_revision",
        "run_id",
        "source",
    )


def test_projection_available_at_never_precedes_the_metric_availability(parity_warehouse):
    violations = parity_warehouse.con.execute(
        """
        SELECT count(*) FROM fundamental_factor_values f
        WHERE f.source = 'atx-db derived factor projection v1'
          AND (
              f.available_at < CAST(f.as_of_date AS TIMESTAMP)
              OR f.available_at < CAST(json_extract_string(
                  f.input_lineage_json, '$.metric.available_at') AS TIMESTAMP)
          )
        """
    ).fetchone()[0]
    assert violations == 0


@pytest.fixture
def projection_store(tmp_store):
    """Small independent grid for PIT and scoped-refresh regression checks."""
    con = tmp_store.con
    for index, security_id in enumerate(("A", "B", "C")):
        con.execute(
            "INSERT INTO universe_membership "
            "(universe_id,security_id,valid_from,as_of_date,is_member,source,available_at,reason,rules_json) "
            "VALUES ('us_common_equity_liquid_v1',?,'2021-01-01','2021-01-01',"
            "true,'test','2021-01-01','member','{}')",
            [security_id],
        )
        for date in (dt.date(2022, 1, 31), dt.date(2022, 2, 28), dt.date(2022, 3, 31)):
            con.execute(
                "INSERT INTO equity_daily_bars "
                "(source,security_id,symbol,trade_date,close,available_at) VALUES ('test',?,?,?,10,?)",
                [security_id, security_id, date, dt.datetime.combine(date, dt.time(21))],
            )
        for seq, (period, available, value) in enumerate(
            (
                ("2021-09-30", "2022-01-01", 10.0),
                ("2021-12-31", "2022-01-15", 20.0),
                ("2021-12-31", "2022-02-15", 25.0),
                ("2021-09-30", "2022-03-01", 999.0),
                ("2021-12-31", "2022-04-01", 30.0),
            )
        ):
            con.execute(
                "INSERT INTO derived_metric_values "
                "(derived_value_id,source,security_id,metric_code,metric_window,period_end,"
                "value,available_at,inputs_hash,as_of_date,is_latest_revision) "
                "VALUES (?, ?,?,'asset_growth','q',?,?,?,'test',?,?)",
                [
                    f"{security_id}-{seq}",
                    DERIVED_SOURCE_NAME,
                    security_id,
                    period,
                    value + index,
                    available,
                    available,
                    seq == 4,
                ],
            )
    return tmp_store


def test_projection_standardized_values_wait_for_eligible_cohort():
    projection = FactorProjection(
        factor_id="test_factor",
        metric_code="test_metric",
        source_window="quarter",
        orientation=1,
        factor_name="Test factor",
        family="test",
        winsor_limit=0.0,
        minimum_names_per_date=3,
        retired_module="KEPT",
    )
    inputs = []
    for day, hour, minutes in ((31, 21, (0, 30, 15)), (30, 20, (0, 10, 5))):
        date = dt.date(2022, 1, day)
        for security_id, value, minute in zip(("A", "B", "C"), (1.0, 2.0, 9.0), minutes, strict=True):
            available_at = dt.datetime.combine(date, dt.time(hour, minute))
            inputs.append(
                {
                    "security_id": security_id,
                    "symbol": security_id,
                    "as_of_date": date,
                    "metric_value": value,
                    "metric_available_at": available_at - dt.timedelta(minutes=5),
                    "decision_available_at": available_at,
                    "period_end": dt.date(2021, 12, 31),
                }
            )
    # An ineligible peer must not postpone publication of the actual cohort.
    inputs.append(
        {
            **inputs[0],
            "security_id": "INELIGIBLE",
            "metric_value": float("inf"),
            "decision_available_at": dt.datetime(2022, 1, 31, 22),
        }
    )
    frame = pd.DataFrame(inputs)
    rows = compute_projection_rows(frame, projection, FactorProjectionOptions())
    assert len(rows) == 6
    for date, expected in (
        (dt.date(2022, 1, 31), pd.Timestamp("2022-01-31 21:30")),
        (dt.date(2022, 1, 30), pd.Timestamp("2022-01-30 20:10")),
    ):
        cohort = rows[rows["as_of_date"] == date]
        assert set(cohort["security_id"]) == {"A", "B", "C"}
        assert set(cohort["available_at"]) == {expected}
        first = cohort[cohort["security_id"] == "A"].iloc[0]
        assert first["raw_value"] == 1.0
        # Sample standard deviation of (1, 2, 9) is sqrt(19): the later peer
        # contributes to the earlier security's actual published score.
        assert first["value"] == pytest.approx(-3.0 / 19.0**0.5)
        for row in cohort.itertuples():
            lineage = json.loads(row.input_lineage_json)
            own_input = frame[(frame["as_of_date"] == date) & (frame["security_id"] == row.security_id)].iloc[0]
            assert pd.Timestamp(lineage["decision"]["available_at"]) == expected
            assert pd.Timestamp(lineage["decision"]["input_available_at"]) == own_input["decision_available_at"]
            assert pd.Timestamp(lineage["metric"]["available_at"]) == own_input["metric_available_at"]


def test_projection_selects_latest_known_period_and_preserves_historical_revisions(projection_store):
    projection = next(p for p in default_projections() if p.retired_module == "KEPT")
    rows = load_projection_inputs(projection_store, projection, FactorProjectionOptions())
    actual = rows[rows["security_id"] == "A"]
    assert actual["metric_value"].tolist() == [20.0, 25.0, 25.0]
    assert actual["metric_available_at"].dt.date.tolist() == [
        dt.date(2022, 1, 15),
        dt.date(2022, 2, 15),
        dt.date(2022, 2, 15),
    ]
    assert set(actual["period_end"].dt.date) == {dt.date(2021, 12, 31)}


def test_projection_daily_rows_respect_cutoff_and_choose_one_revision(projection_store):
    projection = next(p for p in default_projections() if p.metric_code == "rd_to_market_equity")
    con = projection_store.con
    con.execute(
        "INSERT INTO equity_daily_bars (source,security_id,symbol,trade_date,close,available_at) "
        "VALUES ('late','A','A','2022-01-31',30,'2022-01-31 23:00:00')"
    )
    for suffix, hour, value in (("early", 20, 1.0), ("latest", 21, 2.0), ("future", 23, 99.0)):
        con.execute(
            "INSERT INTO market_daily_metrics "
            "(market_daily_id,source,security_id,trade_date,rd_to_market_equity,available_at,inputs_hash,as_of_date) "
            "VALUES (?,?,'A','2022-01-31',?,?,'test','2022-01-31')",
            [suffix, MARKET_DAILY_SOURCE_NAME, value, dt.datetime(2022, 1, 31, hour)],
        )
    rows = load_projection_inputs(projection_store, projection, FactorProjectionOptions())
    assert rows["metric_value"].tolist() == [2.0]
    assert rows["metric_available_at"].tolist() == [pd.Timestamp("2022-01-31 21:00")]
    assert rows["decision_available_at"].tolist() == [pd.Timestamp("2022-01-31 21:00")]


@pytest.mark.parametrize("empty_refresh", [False, True])
def test_projection_scoped_refresh_preserves_other_dates(projection_store, monkeypatch, empty_refresh):
    projection = replace(next(p for p in default_projections() if p.retired_module == "KEPT"), minimum_names_per_date=2)
    monkeypatch.setattr("atx_db.derived_factor_projection.default_projections", lambda: (projection,))
    assert refresh_projected_factor_values(projection_store) == 9
    before = projection_store.con.execute(
        "SELECT * FROM fundamental_factor_values WHERE as_of_date <> '2022-02-28' ORDER BY factor_value_id"
    ).fetchall()
    if empty_refresh:
        projection_store.con.execute("DELETE FROM derived_metric_values")
    count = refresh_projected_factor_values(
        projection_store, FactorProjectionOptions(start_date=dt.date(2022, 2, 28), end_date=dt.date(2022, 2, 28))
    )
    assert count == (0 if empty_refresh else 3)
    assert (
        projection_store.con.execute(
            "SELECT * FROM fundamental_factor_values WHERE as_of_date <> '2022-02-28' ORDER BY factor_value_id"
        ).fetchall()
        == before
    )
    assert (
        projection_store.con.execute(
            "SELECT count(*) FROM fundamental_factor_values WHERE as_of_date = '2022-02-28'"
        ).fetchone()[0]
        == count
    )
