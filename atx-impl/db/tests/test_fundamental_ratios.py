"""Tests for the derived ``fundamental_ratios`` dataset (S9a).

The ratio engine is split into a pure, DB-free transform (``compute_ratio_rows``)
that maps a wide per-(security, period) input frame to long ratio rows, plus a
thin DuckDB materializer (``refresh_fundamental_ratios`` / ``FundamentalRatiosDataset``)
that pivots the bitemporal TTM flows + instant balances into that wide frame and
writes the result. Tests cover the math, the bitemporal availability watermark,
the meaningfulness flags, idempotency, and the as-of reader.

No network: ratios derive purely from already-loaded warehouse tables.
"""
from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

import db.fundamental_ratios as fundamental_ratios_module
from db.item_registry import ratio_input_metrics
from db.fundamental_ratios import (
    FundamentalRatiosDataset,
    FundamentalRatiosOptions,
    RATIO_COLUMNS,
    RATIO_DEFS,
    _attach_prior_year,
    compute_ratio_rows,
    fundamental_ratios_asof,
    refresh_fundamental_ratios,
)


def _ts(s: str) -> pd.Timestamp:
    return pd.Timestamp(s)


def _wide_row(**overrides) -> dict:
    """One fully-populated wide input row; override fields per test."""
    base = {
        "security_id": "SEC-CIK-0000320193",
        "symbol": "AAPL",
        "cik": "320193",
        "upstream_source": "sec_companyfacts",
        "period_start": dt.date(2025, 3, 30),
        "period_end": dt.date(2026, 3, 28),
        "fiscal_year": 2026,
        "fiscal_period": "Q2",
        # flows (TTM)
        "rev": 400.0, "rev_av": _ts("2026-05-01"),
        "ni": 100.0, "ni_av": _ts("2026-05-02"),
        "oi": 120.0, "oi_av": _ts("2026-05-01"),
        "ocf": 130.0, "ocf_av": _ts("2026-05-01"),
        "capex": -30.0, "capex_av": _ts("2026-05-01"),
        "div": -15.0, "div_av": _ts("2026-05-01"),
        "repurch": -50.0, "repurch_av": _ts("2026-05-01"),
        # balances (instant @ period_end)
        "assets": 350.0, "assets_av": _ts("2026-05-03"),
        "liabilities": 290.0, "liabilities_av": _ts("2026-05-01"),
        "equity": 60.0, "equity_av": _ts("2026-05-01"),
        "shares": 15.0, "shares_av": _ts("2026-05-01"),
        # prior-year (YoY) inputs for growth + average-balance ratios (S9c/S9d)
        "rev_prior": 320.0, "rev_prior_av": _ts("2025-05-01"),
        "ni_prior": 80.0, "ni_prior_av": _ts("2025-05-02"),
        "oi_prior": 100.0, "oi_prior_av": _ts("2025-05-01"),
        "ocf_prior": 104.0, "ocf_prior_av": _ts("2025-05-01"),
        "assets_prior": 300.0, "assets_prior_av": _ts("2025-05-03"),
        "liabilities_prior": 250.0, "liabilities_prior_av": _ts("2025-05-01"),
        "equity_prior": 50.0, "equity_prior_av": _ts("2025-05-01"),
        # period-end common shares outstanding (XBRL instant) for the issuance signal (S10e)
        "common_shares_outstanding": 15.0, "common_shares_outstanding_av": _ts("2026-05-01"),
        # prior-year inputs for the Piotroski F-score year-over-year deltas (S10e)
        "long_term_debt_prior": 100.0, "long_term_debt_prior_av": _ts("2025-05-01"),
        "current_assets_prior": 150.0, "current_assets_prior_av": _ts("2025-05-01"),
        "current_liabilities_prior": 90.0, "current_liabilities_prior_av": _ts("2025-05-01"),
        "common_shares_outstanding_prior": 16.0, "common_shares_outstanding_prior_av": _ts("2025-05-01"),
        "gross_profit_prior": 140.0, "gross_profit_prior_av": _ts("2025-05-01"),
        "cost_of_revenue_prior": 180.0, "cost_of_revenue_prior_av": _ts("2025-05-01"),
        "depreciation_amortization_prior": 20.0, "depreciation_amortization_prior_av": _ts("2025-05-01"),
        "property_plant_equipment_net_prior": 110.0, "property_plant_equipment_net_prior_av": _ts("2025-05-01"),
        "accounts_receivable_prior": 40.0, "accounts_receivable_prior_av": _ts("2025-05-01"),
        "selling_general_and_administrative_expense_prior": 32.0,
        "selling_general_and_administrative_expense_prior_av": _ts("2025-05-01"),
        # consolidated inline-XBRL instant metrics for liquidity ratios (S10a)
        "current_assets": 200.0, "current_assets_av": _ts("2026-05-01"),
        "current_liabilities": 100.0, "current_liabilities_av": _ts("2026-05-01"),
        "cash_and_equivalents": 40.0, "cash_and_equivalents_av": _ts("2026-05-01"),
        "inventory": 30.0, "inventory_av": _ts("2026-05-01"),
        "long_term_debt": 90.0, "long_term_debt_av": _ts("2026-05-01"),
        # consolidated inline-XBRL instant asset-structure metrics (S10g)
        "property_plant_equipment_net": 120.0, "property_plant_equipment_net_av": _ts("2026-05-01"),
        "accounts_receivable": 50.0, "accounts_receivable_av": _ts("2026-05-01"),
        # consolidated inline-XBRL annual (duration) flow metrics for margin/coverage (S10c)
        "gross_profit": 180.0, "gross_profit_av": _ts("2026-05-01"),
        "cost_of_revenue": 220.0, "cost_of_revenue_av": _ts("2026-05-01"),
        "interest_expense": 10.0, "interest_expense_av": _ts("2026-05-01"),
        "depreciation_amortization": 25.0, "depreciation_amortization_av": _ts("2026-05-01"),
        "selling_general_and_administrative_expense": 44.0,
        "selling_general_and_administrative_expense_av": _ts("2026-05-01"),
        "retained_earnings": 200.0, "retained_earnings_av": _ts("2026-05-01"),
        # PF-S4 S4-2: accounts payable (DPO / cash-conversion cycle), goodwill +
        # other intangibles (tangible book value), pretax income + income tax
        # (extended DuPont, cash-interest-coverage), weighted-average basic/diluted
        # share counts (EPS), and the cash/inventory priors the Sloan accruals and
        # Montier C-score composites need.
        "accounts_payable": 35.0, "accounts_payable_av": _ts("2026-05-01"),
        "accounts_payable_prior": 28.0, "accounts_payable_prior_av": _ts("2025-05-01"),
        "goodwill": 20.0, "goodwill_av": _ts("2026-05-01"),
        "intangibles_other": 5.0, "intangibles_other_av": _ts("2026-05-01"),
        "pretax_income": 115.0, "pretax_income_av": _ts("2026-05-01"),
        "income_tax": 15.0, "income_tax_av": _ts("2026-05-01"),
        "shares_basic_avg": 14.5, "shares_basic_avg_av": _ts("2026-05-01"),
        "shares_diluted_avg": 15.2, "shares_diluted_avg_av": _ts("2026-05-01"),
        "inventory_prior": 25.0, "inventory_prior_av": _ts("2025-05-01"),
        "cash_and_equivalents_prior": 35.0, "cash_and_equivalents_prior_av": _ts("2025-05-01"),
    }
    base.update(overrides)
    return base


def _by_code(frame: pd.DataFrame) -> dict[str, pd.Series]:
    return {row.ratio_code: row for row in frame.itertuples(index=False)}


S6_2_VALUATION_CODES = {
    "price_to_earnings",
    "price_to_book",
    "price_to_sales",
    "enterprise_value",
    "ev_to_ebitda",
    "ev_to_sales",
    "fcf_yield",
    "earnings_yield",
    "dividend_yield",
}


class TestComputeRatioRows:
    def test_ratio_defs_exclude_s6_2_valuation_codes(self):
        assert S6_2_VALUATION_CODES.isdisjoint({d.code for d in RATIO_DEFS})

    def test_compute_ratio_rows_does_not_emit_valuation_codes_when_inputs_present(self):
        row = _wide_row(
            market_cap=1000.0,
            market_cap_av=_ts("2026-05-04"),
            enterprise_value=1150.0,
            enterprise_value_av=_ts("2026-05-05"),
            ebitda=145.0,
            ebitda_av=_ts("2026-05-05"),
        )

        out = compute_ratio_rows(pd.DataFrame([row]))

        assert S6_2_VALUATION_CODES.isdisjoint(set(out["ratio_code"]))

    def test_existing_fundamental_outputs_remain_intact_after_valuation_split(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))

        assert rows["net_profit_margin"].value == pytest.approx(0.25)
        assert rows["current_ratio"].value == pytest.approx(2.0)
        assert rows["ebitda"].value == pytest.approx(145.0)

    def test_full_row_emits_all_ratio_codes(self):
        row = _wide_row()
        out = compute_ratio_rows(pd.DataFrame([row]))
        codes = {d.code for d in RATIO_DEFS if all(key in row for key in d.inputs)}
        assert set(out["ratio_code"]) == codes
        assert len(out) == len(codes)

    @pytest.mark.parametrize(
        "code,expected",
        [
            ("net_profit_margin", 0.25),
            ("operating_margin", 0.30),
            ("return_on_assets", 100.0 / 350.0),
            ("return_on_equity", 100.0 / 60.0),
            ("assets_to_equity", 350.0 / 60.0),
            ("liabilities_to_assets", 290.0 / 350.0),
            ("liabilities_to_equity", 290.0 / 60.0),
            ("free_cash_flow", 100.0),
            ("fcf_margin", 0.25),
            ("operating_cash_flow_to_net_income", 1.3),
            ("capex_to_revenue", 0.075),
            ("dividend_payout_ratio", 0.15),
            ("buyback_to_net_income", 0.50),
            ("total_payout_ratio", 0.65),
            ("book_value_per_share", 4.0),
            # S9b: efficiency / cash-flow-coverage / reinvestment ratios
            ("asset_turnover", 400.0 / 350.0),
            ("equity_turnover", 400.0 / 60.0),
            ("operating_return_on_assets", 120.0 / 350.0),
            ("operating_cash_flow_margin", 130.0 / 400.0),
            ("operating_cash_flow_to_assets", 130.0 / 350.0),
            ("operating_cash_flow_to_liabilities", 130.0 / 290.0),
            ("capex_to_operating_cash_flow", 30.0 / 130.0),
            ("retention_ratio", (100.0 - 15.0) / 100.0),
            # S9c: year-over-year growth ratios
            ("revenue_growth_yoy", (400.0 - 320.0) / 320.0),
            ("net_income_growth_yoy", (100.0 - 80.0) / 80.0),
            ("operating_income_growth_yoy", (120.0 - 100.0) / 100.0),
            ("operating_cash_flow_growth_yoy", (130.0 - 104.0) / 104.0),
            ("assets_growth_yoy", (350.0 - 300.0) / 300.0),
            # S9d: average-balance returns + book-equity growth
            ("average_return_on_assets", 100.0 / ((350.0 + 300.0) / 2)),
            ("average_return_on_equity", 100.0 / ((60.0 + 50.0) / 2)),
            ("operating_cash_flow_to_average_assets", 130.0 / ((350.0 + 300.0) / 2)),
            ("equity_growth_yoy", (60.0 - 50.0) / 50.0),
            # S10a: liquidity / working-capital ratios (consolidated XBRL instants)
            ("current_ratio", 200.0 / 100.0),
            ("quick_ratio", (200.0 - 30.0) / 100.0),
            ("cash_ratio", 40.0 / 100.0),
            ("working_capital", 100.0),
            ("working_capital_to_assets", (200.0 - 100.0) / 350.0),
            # S10b: debt / solvency ratios (long-term debt vs equity/assets, net of cash)
            ("long_term_debt_to_equity", 90.0 / 60.0),
            ("long_term_debt_to_assets", 90.0 / 350.0),
            ("net_debt", 90.0 - 40.0),
            ("net_debt_to_assets", (90.0 - 40.0) / 350.0),
            # S10c: margin / coverage / EBITDA (annual XBRL flows over TTM revenue)
            ("gross_margin", 180.0 / 400.0),
            ("cost_of_revenue_to_revenue", 220.0 / 400.0),
            ("interest_coverage", 120.0 / 10.0),
            ("ebitda", 120.0 + 25.0),
            ("ebitda_margin", (120.0 + 25.0) / 400.0),
            # S10d: Altman Z'' components
            ("retained_earnings_to_assets", 200.0 / 350.0),
            ("equity_to_liabilities", 60.0 / 290.0),
            # S10e: Piotroski F-score (all 9 signals pass on the canonical full row)
            ("piotroski_f_score", 9.0),
            # S10g: asset-structure / activity ratios (consolidated XBRL instants)
            ("fixed_asset_turnover", 400.0 / 120.0),
            ("receivables_turnover", 400.0 / 50.0),
            ("ppe_to_assets", 120.0 / 350.0),
            # PF-S4 S4-2: DuPont decomposition (both telescope to NI/Equity == return_on_equity)
            ("roe_dupont_3way", 100.0 / 60.0),
            ("roe_dupont_5way", 100.0 / 60.0),
            # PF-S4 S4-2: coverage ratios
            ("ebitda_interest_coverage", (120.0 + 25.0) / 10.0),
            ("cash_interest_coverage", (130.0 + 10.0 + 15.0) / 10.0),
            # PF-S4 S4-2: Sloan accruals
            ("total_accruals", (100.0 - 130.0) / ((350.0 + 300.0) / 2)),
            ("working_capital_accruals", (((200.0 - 150.0) - (40.0 - 35.0)) - (100.0 - 90.0)) / ((350.0 + 300.0) / 2)),
            # PF-S4 S4-2: cash conversion cycle
            ("days_sales_outstanding", (50.0 + 40.0) / 2 / 400.0 * 365.0),
            ("days_inventory_outstanding", (30.0 + 25.0) / 2 / 220.0 * 365.0),
            ("days_payables_outstanding", (35.0 + 28.0) / 2 / 220.0 * 365.0),
            (
                "cash_conversion_cycle",
                (50.0 + 40.0) / 2 / 400.0 * 365.0
                + (30.0 + 25.0) / 2 / 220.0 * 365.0
                - (35.0 + 28.0) / 2 / 220.0 * 365.0,
            ),
            # PF-S4 S4-2: per-share suite
            ("eps_basic", 100.0 / 14.5),
            ("eps_diluted", 100.0 / 15.2),
            ("sales_per_share", 400.0 / 15.0),
            ("cash_flow_per_share", 130.0 / 15.0),
            ("fcf_per_share", (130.0 - 30.0) / 15.0),
            ("tangible_book_value_per_share", (60.0 - 20.0 - 5.0) / 15.0),
            # PF-S4 S4-2: Montier C-score (hand-checked: flag 4 other-current-assets-share
            # rising, flag 6 asset growth 16.7% > 10% trigger; the rest do not -> score 2)
            ("montier_c_score", 2.0),
        ],
    )
    def test_ratio_values(self, code, expected):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        assert rows[code].value == pytest.approx(expected)

    def test_efficiency_ratios_have_efficiency_category(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        assert rows["asset_turnover"].ratio_category == "efficiency"
        assert rows["equity_turnover"].ratio_category == "efficiency"
        # equity_turnover denominator is equity -> not meaningful when negative
        neg = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row(equity=-10.0)])))
        assert neg["equity_turnover"].is_meaningful is False

    def test_growth_ratio_kind_and_lineage(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        g = rows["revenue_growth_yoy"]
        assert g.ratio_kind == "growth"
        assert g.ratio_category == "growth"
        assert g.numerator_value == 400.0   # current TTM
        assert g.denominator_value == 320.0  # prior-year TTM
        # available_at = max(current_av 2026-05-01, prior_av 2025-05-01)
        assert g.available_at == _ts("2026-05-01")

    def test_growth_dropped_when_prior_missing(self):
        row = _wide_row(rev_prior=None, rev_prior_av=None)
        rows = _by_code(compute_ratio_rows(pd.DataFrame([row])))
        assert "revenue_growth_yoy" not in rows
        # other growth ratios with priors present still emit
        assert rows["net_income_growth_yoy"].value == pytest.approx(0.25)

    def test_growth_negative_prior_not_meaningful(self):
        row = _wide_row(ni_prior=-40.0)  # swung from loss to profit
        rows = _by_code(compute_ratio_rows(pd.DataFrame([row])))
        g = rows["net_income_growth_yoy"]
        assert g.is_meaningful is False  # sign of % change is ambiguous off a negative base

    def test_quick_ratio_treats_missing_inventory_as_zero(self):
        # firms that report no InventoryNet (software/aerospace) -> quick == current
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row(inventory=None, inventory_av=None)])))
        assert rows["quick_ratio"].value == pytest.approx(200.0 / 100.0)
        assert rows["current_ratio"].value == pytest.approx(200.0 / 100.0)

    def test_quick_ratio_item_ids_include_inventory_without_changing_existing_lineage(self):
        row = _wide_row(inventory_av=_ts("2026-05-09"))
        rows = _by_code(compute_ratio_rows(pd.DataFrame([row])))
        quick = rows["quick_ratio"]

        assert quick.value == pytest.approx((200.0 - 30.0) / 100.0)
        assert quick.available_at == _ts("2026-05-01")
        assert json.loads(quick.input_codes_json) == ["current_assets", "current_liabilities"]
        assert json.loads(quick.input_item_ids_json) == [1102, 1107, 1202]

    def test_liquidity_dropped_without_current_liabilities(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row(current_liabilities=None, current_liabilities_av=None)])))
        for code in ("current_ratio", "quick_ratio", "cash_ratio", "working_capital"):
            assert code not in rows

    def test_average_balance_returns_need_both_balances(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        # denominator is the mean of ending and prior-year balance
        assert rows["average_return_on_equity"].denominator_value == pytest.approx(55.0)
        # drop when the prior-year balance is unavailable
        gone = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row(assets_prior=None, assets_prior_av=None)])))
        assert "average_return_on_assets" not in gone
        assert "operating_cash_flow_to_average_assets" not in gone

    def test_altman_z_double_prime_composite_score(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        z = rows["altman_z_double_prime"]
        expected = (
            6.56 * (200.0 - 100.0) / 350.0   # working_capital / assets
            + 3.26 * 200.0 / 350.0           # retained_earnings / assets
            + 6.72 * 120.0 / 350.0           # operating_income (EBIT proxy) / assets
            + 1.05 * 60.0 / 290.0            # book equity / total liabilities
        )
        assert z.value == pytest.approx(expected)
        assert z.ratio_kind == "score"
        assert z.ratio_category == "health"
        assert z.is_meaningful is True

    def test_altman_z_dropped_when_component_missing(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row(retained_earnings=None, retained_earnings_av=None)])))
        assert "altman_z_double_prime" not in rows

    def test_piotroski_f_score_all_signals_pass(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        f = rows["piotroski_f_score"]
        assert f.value == pytest.approx(9.0)
        assert f.ratio_kind == "score"
        assert f.ratio_category == "health"
        assert f.unit == "score"
        assert f.is_meaningful is True
        # availability watermark is the max over all 17 (current + prior) inputs
        assert f.available_at == _ts("2026-05-03")  # assets_av is the latest

    def test_piotroski_share_issuance_drops_one_signal(self):
        # shares rose vs prior year -> "no new shares" signal fails -> F = 8
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row(common_shares_outstanding_prior=14.0)])))
        assert rows["piotroski_f_score"].value == pytest.approx(8.0)

    def test_piotroski_weak_firm_low_score(self):
        # loss-making, cash-burning, levering-up, diluting, margin-shrinking firm
        weak = _wide_row(
            ni=-20.0, ocf=-30.0,                     # ROA<0, CFO<0, accruals CFO<NI
            ni_prior=10.0,                           # ROA fell
            long_term_debt=120.0, long_term_debt_prior=80.0,   # leverage rose
            current_assets=120.0, current_liabilities=100.0,   # current ratio 1.2
            current_assets_prior=180.0, current_liabilities_prior=90.0,  # was 2.0 -> fell
            common_shares_outstanding=20.0, common_shares_outstanding_prior=15.0,  # diluted
            gross_profit=120.0, gross_profit_prior=160.0,  # gross margin fell
            rev=400.0, rev_prior=320.0,              # asset turnover: 1.14 vs 1.07 -> rose (1 pt)
        )
        # only asset-turnover-up survives -> F = 1
        assert _by_code(compute_ratio_rows(pd.DataFrame([weak])))["piotroski_f_score"].value == pytest.approx(1.0)

    def test_piotroski_dropped_when_prior_input_missing(self):
        # no prior-year gross profit -> the delta-gross-margin signal is uncomputable -> drop
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row(gross_profit_prior=None, gross_profit_prior_av=None)])))
        assert "piotroski_f_score" not in rows

    def test_ohlson_o_score_composite(self):
        import math
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        o = rows["ohlson_o_score"]
        ta, tl, ca, cl, ni, ni0, ocf = 350.0, 290.0, 200.0, 100.0, 100.0, 80.0, 130.0
        expected = (
            -1.32 - 0.407 * math.log(ta) + 6.03 * (tl / ta) - 1.43 * ((ca - cl) / ta)
            + 0.0757 * (cl / ca) - 1.72 * 0.0 - 2.37 * (ni / ta) - 1.83 * (ocf / tl)
            + 0.285 * 0.0 - 0.521 * ((ni - ni0) / (abs(ni) + abs(ni0)))
        )
        assert o.value == pytest.approx(expected)
        assert o.ratio_kind == "score"
        assert o.ratio_category == "health"
        assert o.unit == "score"
        assert o.is_meaningful is True

    def test_ohlson_distress_raises_score(self):
        # book-insolvent (liabilities > assets) with two consecutive yearly losses
        distressed = _by_code(compute_ratio_rows(pd.DataFrame([
            _wide_row(liabilities=400.0, ni=-50.0, ni_prior=-30.0)
        ])))["ohlson_o_score"].value
        healthy = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))["ohlson_o_score"].value
        # higher O => higher modeled bankruptcy probability
        assert distressed > healthy

    def test_ohlson_dropped_when_input_missing(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row(liabilities=None, liabilities_av=None)])))
        assert "ohlson_o_score" not in rows
        # also drops without the prior-year net income (INTWO / CHIN terms uncomputable)
        gone = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row(ni_prior=None, ni_prior_av=None)])))
        assert "ohlson_o_score" not in gone

    def test_beneish_m_score_composite(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        m = rows["beneish_m_score"]
        dsri = (50.0 / 400.0) / (40.0 / 320.0)
        gmi = ((320.0 - 180.0) / 320.0) / ((400.0 - 220.0) / 400.0)
        aqi = (1.0 - ((200.0 + 120.0) / 350.0)) / (1.0 - ((150.0 + 110.0) / 300.0))
        sgi = 400.0 / 320.0
        depi = (20.0 / (20.0 + 110.0)) / (25.0 / (25.0 + 120.0))
        sgai = (44.0 / 400.0) / (32.0 / 320.0)
        tata = (100.0 - 130.0) / 350.0
        lvgi = (290.0 / 350.0) / (250.0 / 300.0)
        expected = (
            -4.84 + 0.920 * dsri + 0.528 * gmi + 0.404 * aqi + 0.892 * sgi
            + 0.115 * depi - 0.172 * sgai + 4.679 * tata - 0.327 * lvgi
        )
        assert m.value == pytest.approx(expected)
        assert m.ratio_kind == "score"
        assert m.ratio_category == "health"
        assert m.unit == "score"
        assert m.is_meaningful is True

    def test_beneish_dropped_when_prior_input_missing_or_margin_unusable(self):
        missing_prior = _by_code(compute_ratio_rows(pd.DataFrame([
            _wide_row(
                selling_general_and_administrative_expense_prior=None,
                selling_general_and_administrative_expense_prior_av=None,
            )
        ])))
        assert "beneish_m_score" not in missing_prior

        no_gross_margin = _by_code(compute_ratio_rows(pd.DataFrame([
            _wide_row(cost_of_revenue=400.0)
        ])))
        assert "beneish_m_score" not in no_gross_margin

    def test_free_cash_flow_is_level_kind_in_currency(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        fcf = rows["free_cash_flow"]
        assert fcf.ratio_kind == "level"
        assert fcf.unit == "currency"
        assert fcf.numerator_value == 130.0  # ocf
        assert fcf.denominator_value == -30.0  # capex (negative)

    def test_available_at_is_max_of_inputs(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        # net_profit_margin gated by rev_av(05-01), ni_av(05-02) -> 05-02
        assert rows["net_profit_margin"].available_at == _ts("2026-05-02")
        # return_on_assets gated by ni_av(05-02), assets_av(05-03) -> 05-03
        assert rows["return_on_assets"].available_at == _ts("2026-05-03")

    def test_provenance_follows_balance_and_xbrl_availability_driver(self):
        row = _wide_row(
            ni_accession="NI-ACC",
            ni_filed_date=dt.date(2026, 5, 2),
            equity_av=_ts("2026-05-04"),
            equity_accession="EQUITY-ACC",
            equity_filed_date=dt.date(2026, 5, 4),
            current_assets_accession="CA-XBRL-ACC",
            current_assets_filed_date=dt.date(2026, 5, 1),
            current_liabilities_av=_ts("2026-05-05"),
            current_liabilities_accession="CL-XBRL-ACC",
            current_liabilities_filed_date=dt.date(2026, 5, 5),
        )

        rows = _by_code(compute_ratio_rows(pd.DataFrame([row])))

        assert rows["return_on_equity"].available_at == _ts("2026-05-04")
        assert rows["return_on_equity"].source_accession == "EQUITY-ACC"
        assert rows["return_on_equity"].filed_date == dt.date(2026, 5, 4)
        assert rows["current_ratio"].available_at == _ts("2026-05-05")
        assert rows["current_ratio"].source_accession == "CL-XBRL-ACC"
        assert rows["current_ratio"].filed_date == dt.date(2026, 5, 5)

    def test_as_of_date_is_period_end(self):
        rows = _by_code(compute_ratio_rows(pd.DataFrame([_wide_row()])))
        assert rows["net_profit_margin"].as_of_date == dt.date(2026, 3, 28)

    def test_missing_input_drops_only_dependent_ratios(self):
        row = _wide_row(rev=None, rev_av=None)
        rows = _by_code(compute_ratio_rows(pd.DataFrame([row])))
        # revenue-dependent ratios absent
        for code in ("net_profit_margin", "operating_margin", "fcf_margin", "capex_to_revenue"):
            assert code not in rows
        # non-revenue ratios still present
        assert rows["return_on_assets"].value == pytest.approx(100.0 / 350.0)
        assert rows["book_value_per_share"].value == pytest.approx(4.0)

    def test_negative_denominator_flags_not_meaningful(self):
        row = _wide_row(equity=-10.0)
        rows = _by_code(compute_ratio_rows(pd.DataFrame([row])))
        roe = rows["return_on_equity"]
        assert roe.value == pytest.approx(-10.0)
        assert roe.is_meaningful is False
        # assets>0 ratio stays meaningful
        assert rows["return_on_assets"].is_meaningful is True

    def test_zero_denominator_skips_ratio(self):
        row = _wide_row(ni=0.0)
        rows = _by_code(compute_ratio_rows(pd.DataFrame([row])))
        assert "dividend_payout_ratio" not in rows  # den = ni = 0
        assert "operating_cash_flow_to_net_income" not in rows
        # numerator-zero ratio still emitted
        assert rows["net_profit_margin"].value == pytest.approx(0.0)

    def test_deterministic_ratio_id(self):
        a = compute_ratio_rows(pd.DataFrame([_wide_row()]))
        b = compute_ratio_rows(pd.DataFrame([_wide_row()]))
        assert list(a["ratio_id"]) == list(b["ratio_id"])
        assert a["ratio_id"].is_unique

    def test_empty_input_returns_empty_frame(self):
        out = compute_ratio_rows(pd.DataFrame())
        assert out.empty
        assert "ratio_code" in out.columns


class TestAttachPriorYear:
    def _base(self, period_end, rev, av):
        return {
            "security_id": "S1", "symbol": "AAPL", "period_end": period_end,
            "rev": rev, "rev_av": av,
            "ni": rev / 4, "ni_av": av, "oi": rev / 3, "oi_av": av,
            "ocf": rev / 3, "ocf_av": av, "assets": rev * 2, "assets_av": av,
        }

    def test_pairs_row_to_prior_year(self):
        cur = self._base(dt.date(2026, 3, 28), 400.0, _ts("2026-05-01"))
        prior = self._base(dt.date(2025, 3, 29), 320.0, _ts("2025-05-01"))
        out = _attach_prior_year(pd.DataFrame([prior, cur]))
        by_end = {r.period_end: r for r in out.itertuples(index=False)}
        # 2026 row gets the 2025 values as *_prior
        assert by_end[dt.date(2026, 3, 28)].rev_prior == 320.0
        assert by_end[dt.date(2026, 3, 28)].rev_prior_av == _ts("2025-05-01")
        # 2025 row has no prior in the frame
        assert pd.isna(by_end[dt.date(2025, 3, 29)].rev_prior)

    def test_no_match_outside_one_year_window(self):
        cur = self._base(dt.date(2026, 3, 28), 400.0, _ts("2026-05-01"))
        far = self._base(dt.date(2024, 1, 1), 200.0, _ts("2024-02-01"))  # ~2yr prior
        out = _attach_prior_year(pd.DataFrame([far, cur]))
        by_end = {r.period_end: r for r in out.itertuples(index=False)}
        assert pd.isna(by_end[dt.date(2026, 3, 28)].rev_prior)


# --------------------------------------------------------------------------- #
# Integration: pivot real warehouse input tables -> ratios
# --------------------------------------------------------------------------- #

_TTM_DEFAULTS = {
    "ttm_revision_group_id": "g", "anchor_statement_point_id": "a",
    "cik": "320193", "statement_type": "income_statement",
    "statement_section": "profitability", "canonical_label": "lbl",
    "unit": "USD", "unit_type": "monetary", "as_of_date": dt.date(2026, 3, 28),
    "accession_number": "acc", "quarter_count": 4, "coverage_days": 365,
    "input_statement_point_ids_json": "[]", "input_accessions_json": "[]",
    "input_period_ends_json": "[]", "revision_sequence": 0, "revision_count": 1,
    "is_latest_revision": True, "is_value_changed": False,
    "calculation_method": "sum_4q",
}
_STMT_DEFAULTS = {
    "fact_revision_id": "fr", "revision_group_id": "rg",
    "statement_type": "balance_sheet", "statement_section": "assets",
    "canonical_label": "lbl", "taxonomy": "us-gaap", "concept": "Assets",
    "unit": "USD", "unit_type": "monetary", "period_type": "instant",
    "normal_balance": "debit", "as_of_date": dt.date(2026, 3, 28),
    "accession_number": "acc", "revision_sequence": 0, "revision_count": 1,
    "is_latest_revision": True, "is_value_changed": False,
    "source_url": "http://x",
}


def _insert_ttm(store, *, security_id, symbol, metric, value, end, av,
                start=dt.date(2025, 3, 30), source="sec_companyfacts"):
    cols = dict(_TTM_DEFAULTS)
    cols.update(
        ttm_point_id=f"{security_id}|{metric}|{end}", source=source,
        security_id=security_id, symbol=symbol, canonical_metric=metric,
        ttm_value=value, ttm_start_date=start, ttm_end_date=end,
        available_at=av, updated_at=av,
    )
    keys = ", ".join(cols)
    store.con.execute(
        f"INSERT INTO fundamental_ttm_points ({keys}) VALUES ({', '.join(['?'] * len(cols))})",
        list(cols.values()),
    )


def _insert_stmt(store, *, security_id, symbol, metric, value, end, av,
                 source="sec_companyfacts"):
    cols = dict(_STMT_DEFAULTS)
    cols.update(
        statement_point_id=f"{security_id}|{metric}|{end}", source=source,
        security_id=security_id, symbol=symbol, cik="320193",
        canonical_metric=metric, value=value, period_end=end,
        available_at=av, updated_at=av,
    )
    keys = ", ".join(cols)
    store.con.execute(
        f"INSERT INTO fundamental_statement_points ({keys}) VALUES ({', '.join(['?'] * len(cols))})",
        list(cols.values()),
    )


def _insert_xbrl_metric(store, *, security_id, symbol, metric, value, end, av,
                        period_type="instant", period_start=None, source="sec_inline_xbrl_v1"):
    store.con.execute(
        """
        INSERT INTO fundamental_xbrl_metric (
            metric_id, source, security_id, symbol, cik, canonical_metric, concept,
            taxonomy, period_type, period_start, period_end, value,
            is_latest_revision, as_of_date, available_at
        ) VALUES (?, ?, ?, ?, '012927', ?, ?, 'us-gaap', ?, ?, ?, ?, true, ?, ?)
        """,
        [f"{metric}|{period_type}|{end}", source, security_id, symbol, metric, metric,
         period_type, period_start, end, value, end, av],
    )


def _seed_minimal(store):
    sid, sym, end = "SEC-CIK-0000320193", "AAPL", dt.date(2026, 3, 28)
    av = dt.datetime(2026, 5, 1, 22, 0)
    _insert_ttm(store, security_id=sid, symbol=sym, metric="revenue", value=400.0, end=end, av=av)
    _insert_ttm(store, security_id=sid, symbol=sym, metric="net_income", value=100.0, end=end, av=av)
    _insert_ttm(store, security_id=sid, symbol=sym, metric="operating_cash_flow", value=130.0, end=end, av=av)
    _insert_ttm(store, security_id=sid, symbol=sym, metric="capital_expenditures", value=-30.0, end=end, av=av)
    _insert_stmt(store, security_id=sid, symbol=sym, metric="assets", value=350.0, end=end, av=av)
    _insert_stmt(store, security_id=sid, symbol=sym, metric="stockholders_equity", value=60.0, end=end, av=av)
    return sid, sym, end


class TestRefreshIntegration:
    def test_materializes_ratios_from_warehouse_tables(self, tmp_store):
        _seed_minimal(tmp_store)
        rows = refresh_fundamental_ratios(tmp_store, FundamentalRatiosOptions())
        assert rows > 0
        df = tmp_store.con.execute(
            "SELECT ratio_code, value FROM fundamental_ratios ORDER BY ratio_code"
        ).df()
        codes = dict(zip(df["ratio_code"], df["value"]))
        assert codes["net_profit_margin"] == pytest.approx(0.25)
        assert codes["return_on_equity"] == pytest.approx(100.0 / 60.0)
        assert codes["free_cash_flow"] == pytest.approx(100.0)

    def test_dataset_run_is_idempotent(self, tmp_store):
        _seed_minimal(tmp_store)
        ds = FundamentalRatiosDataset()
        r1 = ds.run(tmp_store, FundamentalRatiosOptions())
        n1 = tmp_store.con.execute("SELECT count(*) FROM fundamental_ratios").fetchone()[0]
        r2 = ds.run(tmp_store, FundamentalRatiosOptions())
        n2 = tmp_store.con.execute("SELECT count(*) FROM fundamental_ratios").fetchone()[0]
        assert r1.rows_loaded == r2.rows_loaded
        assert n1 == n2  # no duplication on re-run

    def test_reads_liquidity_metrics_from_xbrl_table(self, tmp_store):
        sid, sym, end = _seed_minimal(tmp_store)
        av = dt.datetime(2026, 5, 1, 22, 0)

        def _xm(metric, value, accession, metric_av=av):
            tmp_store.con.execute(
                """
                INSERT INTO fundamental_xbrl_metric (
                    metric_id, source, security_id, symbol, cik, canonical_metric, concept,
                    taxonomy, period_type, period_end, accession_number, value,
                    is_latest_revision, as_of_date, available_at
                ) VALUES (?, 'x', ?, ?, '320193', ?, ?, 'us-gaap', 'instant', ?, ?, ?, true, ?, ?)
                """,
                [f"{metric}|{end}", sid, sym, metric, metric, end, accession, value, end, metric_av],
            )

        _xm("current_assets", 200.0, "CA-XBRL-ACC")
        _xm(
            "current_liabilities",
            100.0,
            "CL-XBRL-ACC",
            dt.datetime(2026, 5, 2, 22, 0),
        )
        _xm("cash_and_equivalents", 40.0, "CASH-XBRL-ACC")
        _xm("inventory", 30.0, "INV-XBRL-ACC")
        FundamentalRatiosDataset().run(tmp_store, FundamentalRatiosOptions())
        df = tmp_store.con.execute(
            """
            SELECT ratio_code, value, available_at, source_accession
            FROM fundamental_ratios
            WHERE ratio_category='liquidity'
            """
        ).df()
        codes = dict(zip(df["ratio_code"], df["value"]))
        assert codes["current_ratio"] == pytest.approx(2.0)
        assert codes["quick_ratio"] == pytest.approx(1.7)
        assert codes["cash_ratio"] == pytest.approx(0.4)
        current = df.loc[df["ratio_code"] == "current_ratio"].iloc[0]
        assert pd.Timestamp(current["available_at"]) == _ts("2026-05-02 22:00")
        assert current["source_accession"] == "CL-XBRL-ACC"

    def test_piotroski_emits_at_consecutive_fiscal_years(self, tmp_store):
        # Two fiscal years of every input -> the F-score emits only at the later year
        # (the earlier year has no prior to delta against). Proves the full chain:
        # TTM flows + statement balances + XBRL instants/annual-flows -> prior-year
        # pairing -> composite score.
        sid, sym = "SEC-CIK-0000012927", "BA"
        years = {
            2024: (dt.date(2024, 12, 31), dt.datetime(2025, 2, 1, 22, 0)),
            2025: (dt.date(2025, 12, 31), dt.datetime(2026, 2, 1, 22, 0)),
        }
        flows = {2024: dict(revenue=360.0, net_income=80.0, operating_cash_flow=110.0),
                 2025: dict(revenue=400.0, net_income=100.0, operating_cash_flow=130.0)}
        assets = {2024: 320.0, 2025: 350.0}
        instants = {
            2024: dict(current_assets=180.0, current_liabilities=95.0, long_term_debt=100.0, common_shares_outstanding=16.0),
            2025: dict(current_assets=200.0, current_liabilities=100.0, long_term_debt=90.0, common_shares_outstanding=15.0),
        }
        gross_profit = {2024: 150.0, 2025: 180.0}
        for y, (end, av) in years.items():
            start = end - dt.timedelta(days=365)
            for metric, val in flows[y].items():
                _insert_ttm(tmp_store, security_id=sid, symbol=sym, metric=metric, value=val, end=end, av=av, start=start)
            _insert_stmt(tmp_store, security_id=sid, symbol=sym, metric="assets", value=assets[y], end=end, av=av)
            for metric, val in instants[y].items():
                _insert_xbrl_metric(tmp_store, security_id=sid, symbol=sym, metric=metric, value=val, end=end, av=av)
            _insert_xbrl_metric(tmp_store, security_id=sid, symbol=sym, metric="gross_profit", value=gross_profit[y],
                                end=end, av=av, period_type="duration", period_start=start)

        FundamentalRatiosDataset().run(tmp_store, FundamentalRatiosOptions())
        df = tmp_store.con.execute(
            "SELECT period_end, value FROM fundamental_ratios WHERE ratio_code='piotroski_f_score' ORDER BY period_end"
        ).df()
        assert len(df) == 1  # only the later fiscal year has a prior to compare against
        assert pd.Timestamp(df.iloc[0]["period_end"]) == pd.Timestamp("2025-12-31")
        assert df.iloc[0]["value"] == pytest.approx(9.0)

    def test_no_duplicate_natural_keys(self, tmp_store):
        _seed_minimal(tmp_store)
        FundamentalRatiosDataset().run(tmp_store, FundamentalRatiosOptions())
        dupes = tmp_store.con.execute(
            """
            SELECT count(*) FROM (
                SELECT source, security_id, ratio_code, basis, period_end, count(*) n
                FROM fundamental_ratios GROUP BY 1,2,3,4,5 HAVING count(*) > 1
            )
            """
        ).fetchone()[0]
        assert dupes == 0


class TestAsofReader:
    def test_filters_by_available_at(self, tmp_store):
        _seed_minimal(tmp_store)
        FundamentalRatiosDataset().run(tmp_store, FundamentalRatiosOptions())
        # before availability (input available_at = 2026-05-01): nothing visible
        early = fundamental_ratios_asof(dt.date(2026, 4, 1), store=tmp_store, symbols=["AAPL"])
        assert early.empty
        # after availability: ratios visible
        late = fundamental_ratios_asof(dt.date(2026, 6, 1), store=tmp_store, symbols=["AAPL"])
        assert not late.empty
        assert "net_profit_margin" in set(late["ratio_code"])


def test_migration_0063_adds_and_catalogs_input_item_ids_json(tmp_store):
    """PF-S1 S1-3 migration is append-only, idempotent, and catalogued."""
    from db.migrations import _fundamental_ratios_input_item_ids

    columns = tmp_store.con.execute(
        """
        SELECT column_name
        FROM information_schema.columns
        WHERE table_schema = 'main' AND table_name = 'fundamental_ratios'
        """
    ).fetchall()
    versions = tmp_store.con.execute(
        "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
    ).fetchall()
    catalog_count = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM field_catalog
        WHERE table_name = 'fundamental_ratios'
          AND field_name = 'input_item_ids_json'
          AND semantic_type = 'json'
        """
    ).fetchone()[0]

    assert ("input_item_ids_json",) in columns
    assert 63 in {row[0] for row in versions}
    assert catalog_count == 1

    _fundamental_ratios_input_item_ids(tmp_store.con)
    _fundamental_ratios_input_item_ids(tmp_store.con)
    catalog_count_after = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM field_catalog
        WHERE table_name = 'fundamental_ratios'
          AND field_name = 'input_item_ids_json'
        """
    ).fetchone()[0]
    assert catalog_count_after == 1


# --------------------------------------------------------------------------- #
# PF-S1 S1-3: byte-identity gate -- governed registry sourcing must not change
# any pre-existing column of a fundamental_ratios rebuild.
# --------------------------------------------------------------------------- #
#
# The four input dicts (TTM_INPUTS, BALANCE_INPUTS, XBRL_BALANCE_INPUTS,
# XBRL_FLOW_INPUTS) move from hand-typed literals to a governed db.item_registry
# map, but the canonical_metric strings they hold -- and therefore every ratio
# VALUE -- must stay byte-identical. This fixture panel exercises every pivot
# source table (TTM, statement, xbrl balance, xbrl flow) plus two fiscal years
# so YoY/average-balance ratios and Piotroski/Beneish all emit, and asserts the
# rebuilt frame equals a golden capture on every pre-existing RATIO_COLUMNS entry.

def _seed_byte_identity_panel(store):
    sid, sym = "SEC-CIK-0000320193", "AAPL"
    years = {
        2024: (dt.date(2024, 12, 31), dt.datetime(2025, 2, 1, 22, 0)),
        2025: (dt.date(2025, 12, 31), dt.datetime(2026, 2, 1, 22, 0)),
    }
    ttm_flows = {
        2024: dict(revenue=360.0, net_income=80.0, operating_income=95.0,
                   operating_cash_flow=110.0, capital_expenditures=-25.0,
                   dividends_paid=-10.0, share_repurchases=-20.0),
        2025: dict(revenue=400.0, net_income=100.0, operating_income=120.0,
                   operating_cash_flow=130.0, capital_expenditures=-30.0,
                   dividends_paid=-15.0, share_repurchases=-50.0),
    }
    stmt_balances = {
        2024: dict(assets=300.0, liabilities=250.0, stockholders_equity=50.0, shares_outstanding=16.0),
        2025: dict(assets=350.0, liabilities=290.0, stockholders_equity=60.0, shares_outstanding=15.0),
    }
    xbrl_instants = {
        2024: dict(current_assets=180.0, current_liabilities=95.0, cash_and_equivalents=35.0,
                   inventory=25.0, long_term_debt=100.0, retained_earnings=170.0,
                   common_shares_outstanding=16.0, property_plant_equipment_net=110.0,
                   accounts_receivable=40.0),
        2025: dict(current_assets=200.0, current_liabilities=100.0, cash_and_equivalents=40.0,
                   inventory=30.0, long_term_debt=90.0, retained_earnings=200.0,
                   common_shares_outstanding=15.0, property_plant_equipment_net=120.0,
                   accounts_receivable=50.0),
    }
    xbrl_flows = {
        2024: dict(gross_profit=140.0, cost_of_revenue=180.0, interest_expense=9.0,
                   depreciation_amortization=20.0, selling_general_and_administrative_expense=32.0),
        2025: dict(gross_profit=180.0, cost_of_revenue=220.0, interest_expense=10.0,
                   depreciation_amortization=25.0, selling_general_and_administrative_expense=44.0),
    }
    for y, (end, av) in years.items():
        start = end - dt.timedelta(days=365)
        for metric, val in ttm_flows[y].items():
            _insert_ttm(store, security_id=sid, symbol=sym, metric=metric, value=val, end=end, av=av, start=start)
        for metric, val in stmt_balances[y].items():
            _insert_stmt(store, security_id=sid, symbol=sym, metric=metric, value=val, end=end, av=av)
        for metric, val in xbrl_instants[y].items():
            _insert_xbrl_metric(store, security_id=sid, symbol=sym, metric=metric, value=val, end=end, av=av,
                                period_type="instant")
        for metric, val in xbrl_flows[y].items():
            _insert_xbrl_metric(store, security_id=sid, symbol=sym, metric=metric, value=val, end=end, av=av,
                                period_type="duration", period_start=start)
    return sid, sym


# Exactly the columns that existed before additive lineage/linkage work; exclude
# later metadata columns so this list never drifts from "pre-existing values".
_PRE_EXISTING_RATIO_COLUMNS = [
    c for c in RATIO_COLUMNS
    if c not in {"input_item_ids_json", "source_accession", "filed_date", "vintage_class"}
]

_LITERAL_TTM_INPUTS = {
    "rev": "revenue",
    "ni": "net_income",
    "oi": "operating_income",
    "ocf": "operating_cash_flow",
    "capex": "capital_expenditures",
    "div": "dividends_paid",
    "repurch": "share_repurchases",
}
_LITERAL_BALANCE_INPUTS = {
    "assets": "assets",
    "liabilities": "liabilities",
    "equity": "stockholders_equity",
    "shares": "shares_outstanding",
}
_LITERAL_XBRL_BALANCE_INPUTS = {
    "current_assets": "current_assets",
    "current_liabilities": "current_liabilities",
    "cash_and_equivalents": "cash_and_equivalents",
    "inventory": "inventory",
    "long_term_debt": "long_term_debt",
    "retained_earnings": "retained_earnings",
    "common_shares_outstanding": "common_shares_outstanding",
    "property_plant_equipment_net": "property_plant_equipment_net",
    "accounts_receivable": "accounts_receivable",
    # PF-S4 S4-2 additions (accounts_payable, goodwill, intangibles_other)
    "accounts_payable": "ap",
    "goodwill": "goodwill",
    "intangibles_other": "intangibles_other",
}
_LITERAL_XBRL_FLOW_INPUTS = {
    "gross_profit": "gross_profit",
    "cost_of_revenue": "cost_of_revenue",
    "interest_expense": "interest_expense",
    "depreciation_amortization": "depreciation_amortization",
    "selling_general_and_administrative_expense": "selling_general_and_administrative_expense",
    # PF-S4 S4-2 additions (pretax_income, income_tax, shares_basic_avg, shares_diluted_avg)
    "pretax_income": "pretax_income",
    "income_tax": "income_tax",
    "shares_basic_avg": "shares_basic_avg",
    "shares_diluted_avg": "shares_diluted_avg",
}


class TestByteIdentityRegression:
    """RED-first regression: capture ratios with today's literal dicts, then assert
    the post-refactor (registry-sourced) rebuild is identical on every pre-existing
    column. Any drift here is a correctness bug in the S1-3 change, never a data update.
    """

    def test_registry_sourced_rebuild_is_byte_identical_to_literal_dict_rebuild(self, tmp_store, monkeypatch):
        sid, sym = _seed_byte_identity_panel(tmp_store)

        monkeypatch.setattr(fundamental_ratios_module, "TTM_INPUTS", dict(_LITERAL_TTM_INPUTS))
        monkeypatch.setattr(fundamental_ratios_module, "BALANCE_INPUTS", dict(_LITERAL_BALANCE_INPUTS))
        monkeypatch.setattr(
            fundamental_ratios_module,
            "XBRL_BALANCE_INPUTS",
            dict(_LITERAL_XBRL_BALANCE_INPUTS),
        )
        monkeypatch.setattr(
            fundamental_ratios_module,
            "XBRL_FLOW_INPUTS",
            dict(_LITERAL_XBRL_FLOW_INPUTS),
        )
        golden = fundamental_ratios_module.refresh_fundamental_ratios(
            tmp_store,
            FundamentalRatiosOptions(),
        )
        golden_df = tmp_store.con.execute(
            f"SELECT {', '.join(_PRE_EXISTING_RATIO_COLUMNS)} FROM fundamental_ratios "
            "ORDER BY security_id, ratio_code, period_end"
        ).df()
        assert golden > 0
        assert len(golden_df) > 0

        monkeypatch.setattr(fundamental_ratios_module, "TTM_INPUTS", ratio_input_metrics("ttm"))
        monkeypatch.setattr(fundamental_ratios_module, "BALANCE_INPUTS", ratio_input_metrics("balance"))
        monkeypatch.setattr(
            fundamental_ratios_module,
            "XBRL_BALANCE_INPUTS",
            ratio_input_metrics("xbrl_balance"),
        )
        monkeypatch.setattr(
            fundamental_ratios_module,
            "XBRL_FLOW_INPUTS",
            ratio_input_metrics("xbrl_flow"),
        )
        rebuilt = fundamental_ratios_module.refresh_fundamental_ratios(
            tmp_store,
            FundamentalRatiosOptions(),
        )
        rebuilt_df = tmp_store.con.execute(
            f"SELECT {', '.join(_PRE_EXISTING_RATIO_COLUMNS)} FROM fundamental_ratios "
            "ORDER BY security_id, ratio_code, period_end"
        ).df()

        assert rebuilt == golden
        pd.testing.assert_frame_equal(
            rebuilt_df.reset_index(drop=True), golden_df.reset_index(drop=True)
        )

    def test_input_codes_json_unchanged_by_registry_sourcing(self, tmp_store):
        """input_codes_json (pre-existing) must stay exactly the RATIO_DEFS raw-key list."""
        _seed_byte_identity_panel(tmp_store)
        refresh_fundamental_ratios(tmp_store, FundamentalRatiosOptions())
        row = tmp_store.con.execute(
            "SELECT input_codes_json FROM fundamental_ratios WHERE ratio_code = 'net_profit_margin' LIMIT 1"
        ).fetchone()
        import json
        assert json.loads(row[0]) == ["ni", "rev"]


class TestInputItemIdsJsonColumn:
    """input_item_ids_json is additive metadata; byte-identity of VALUES is untouched."""

    def test_column_present_and_populated_for_simple_ratio(self, tmp_store):
        _seed_byte_identity_panel(tmp_store)
        refresh_fundamental_ratios(tmp_store, FundamentalRatiosOptions())
        row = tmp_store.con.execute(
            "SELECT input_item_ids_json FROM fundamental_ratios WHERE ratio_code = 'net_profit_margin' LIMIT 1"
        ).fetchone()
        import json
        assert row is not None
        ids = json.loads(row[0])
        # net_profit_margin inputs: ni (net_income -> 1031), rev (revenue -> 1001)
        assert ids == [1001, 1031]

    def test_quick_ratio_item_ids_include_inventory(self, tmp_store):
        _seed_byte_identity_panel(tmp_store)
        refresh_fundamental_ratios(tmp_store, FundamentalRatiosOptions())
        row = tmp_store.con.execute(
            """
            SELECT input_codes_json, input_item_ids_json, available_at
            FROM fundamental_ratios
            WHERE ratio_code = 'quick_ratio'
            ORDER BY period_end DESC
            LIMIT 1
            """
        ).fetchone()

        assert row is not None
        assert json.loads(row[0]) == ["current_assets", "current_liabilities"]
        assert json.loads(row[1]) == [1102, 1107, 1202]
        assert row[2] == dt.datetime(2026, 2, 1, 22, 0)

    def test_beneish_m_score_item_ids_include_documented_gap_handling(self, tmp_store):
        """beneish_m_score omits the controller-design SG&A gap from item linkage."""
        _seed_byte_identity_panel(tmp_store)
        refresh_fundamental_ratios(tmp_store, FundamentalRatiosOptions())
        row = tmp_store.con.execute(
            "SELECT input_item_ids_json FROM fundamental_ratios WHERE ratio_code = 'beneish_m_score' LIMIT 1"
        ).fetchone()
        import json
        assert row is not None
        ids = json.loads(row[0])
        assert ids == sorted({1001, 1106, 1003, 1102, 1110, 1101, 1307, 1201, 1031, 1301})
        assert 1005 not in ids

    def test_piotroski_f_score_common_shares_outstanding_stays_unmapped(self, tmp_store):
        """common_shares_outstanding is a documented S1-3 metadata gap, not item 1039."""
        _seed_byte_identity_panel(tmp_store)
        refresh_fundamental_ratios(tmp_store, FundamentalRatiosOptions())
        row = tmp_store.con.execute(
            "SELECT input_item_ids_json FROM fundamental_ratios WHERE ratio_code = 'piotroski_f_score' LIMIT 1"
        ).fetchone()
        import json
        assert row is not None
        ids = json.loads(row[0])
        assert ids == sorted({1001, 1004, 1031, 1101, 1102, 1202, 1207, 1301})
        assert 1039 not in ids
