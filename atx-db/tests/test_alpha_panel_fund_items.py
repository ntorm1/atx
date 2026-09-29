"""Pure helpers of alpha-panel stage F v2 (fund_items): tiers, fallbacks, zero fills, F-score terms, templates,
currency passes, class-share fallback, staleness and the quarterly history."""

from __future__ import annotations

import datetime as dt
import math

import pytest

from atx_db.alpha_panel import fund_items as fi

D = dt.date
T = dt.datetime


def row(clock, accn, concept, start, end, value, unit="USD", form="10-K", fy=None, fp=None):
    """A fact row as fundamentals.stream_issuers yields it (without the cik)."""
    cid = fi.CID[concept]
    return (clock, accn, cid, start, end, value, unit, form, clock.date(), fy, fp, "fsds_accepted_utc")


def knowledge(rows):
    k = fi.Knowledge()
    for r in rows:
        k.apply(r[2], r[3], r[4], r[5], r[0])
    return k


FY_START, FY_END = D(2023, 1, 1), D(2023, 12, 31)
C1 = T(2024, 2, 20, 21, 0)


def quarters(concept, values, year=2023, unit="USD"):
    ends = [D(year, 3, 31), D(year, 6, 30), D(year, 9, 30), D(year, 12, 31)]
    starts = [D(year, 1, 1), D(year, 4, 1), D(year, 7, 1), D(year, 10, 1)]
    return [row(C1, "a1", concept, s, e, v, unit) for s, e, v in zip(starts, ends, values)]


# --- templates and structural NaN -------------------------------------------------------------------------

@pytest.mark.parametrize("sic,expected", [
    (6021, "bank"), (6211, "bank"), (6199, "bank"), (6311, "insurer"), (6324, "insurer"), (6399, "insurer"),
    (6798, "reit"), (4911, "utility"), (4941, "utility"), (4953, "other"), (6411, "other"), (6770, "other"),
    (3674, "other"), (None, "other")])
def test_fin_template_mapping(sic, expected):
    assert fi.fin_template(sic) == expected


def test_structural_items_per_template():
    assert fi.is_structural("bank", "gp_ttm") and fi.is_structural("insurer", "gp_ttm")
    assert fi.is_structural("bank", "oi_ttm") and not fi.is_structural("insurer", "oi_ttm")
    assert not fi.is_structural("other", "gp_ttm")
    assert all(item in fi.ALL_ITEMS for items in fi.STRUCTURAL.values() for item in items)


# --- F-score terms -------------------------------------------------------------------------------------------

def test_piotroski_terms_all_good_news():
    t = fi.piotroski_terms(ni=10, cfo=15, roa=0.10, roa4=0.05, lev=0.2, lev4=0.3, cr=2.0, cr4=1.5, sstk=0.0,
                           gm=0.4, gm4=0.3, turn=1.1, turn4=1.0)
    assert list(t) == list(fi.FSCORE_TERMS)
    assert all(v == 1 for v in t.values())


def test_piotroski_terms_bad_news_and_missing():
    t = fi.piotroski_terms(ni=-1, cfo=-2, roa=0.01, roa4=0.02, lev=0.3, lev4=0.3, cr=None, cr4=1.0, sstk=5.0,
                           gm=0.3, gm4=0.3, turn=None, turn4=None)
    assert t["f_roa"] == 0 and t["f_cfo"] == 0 and t["f_droa"] == 0
    assert t["f_accrual"] == 0          # cfo -2 not above ni -1
    assert t["f_dlever"] == 0           # leverage unchanged: strict fall required
    assert t["f_dliquid"] is None and t["f_dturn"] is None
    assert t["f_eq_offer"] == 0 and t["f_dmargin"] == 0


def _fscore_issuer(extra=()):
    """One issuer with two fiscal years of balance sheets and flows (no current assets: 8 of 9 terms)."""
    rows = []
    for y, at, ni, cfo, sale, gp in ((2021, 1000.0, 50.0, 70.0, 900.0, 300.0), (2022, 1100.0, 60.0, 80.0, 1000.0, 350.0),
                                     (2023, 1200.0, 80.0, 90.0, 1200.0, 450.0)):
        c = T(y + 1, 2, 20, 21, 0)
        a = f"fy{y}"
        s, e = D(y, 1, 1), D(y, 12, 31)
        rows += [row(c, a, "Assets", None, e, at), row(c, a, "StockholdersEquity", None, e, at / 2),
                 row(c, a, "NetIncomeLoss", s, e, ni), row(c, a, "NetCashProvidedByUsedInOperatingActivities", s, e, cfo),
                 row(c, a, "Revenues", s, e, sale), row(c, a, "GrossProfit", s, e, gp),
                 row(c, a, "LongTermDebtNoncurrent", None, e, 100.0), row(c, a, "ProceedsFromIssuanceOfCommonStock", s, e, 0.0)]
        rows += [r for r in extra if r[0] == c]
    return rows


def test_fscore_partial_needs_six_terms_and_full_needs_nine():
    ev, _ = fi.company_events(1, _fscore_issuer(), T(2024, 1, 1), {})
    last = ev[-1]
    assert last["f_dliquid"] is None                    # no current assets / liabilities
    assert last["fscore_n"] == 8.0
    assert last["fscore"] is None                       # v1 rule: all nine required
    terms = [last[t] for t in fi.FSCORE_TERMS if last[t] is not None]
    assert last["fscore_partial"] == float(sum(terms))
    extra = []
    for y, act in ((2021, 400.0), (2022, 420.0), (2023, 500.0)):
        c, e = T(y + 1, 2, 20, 21, 0), D(y, 12, 31)
        extra += [row(c, f"fy{y}", "AssetsCurrent", None, e, act), row(c, f"fy{y}", "LiabilitiesCurrent", None, e, 200.0)]
    ev, _ = fi.company_events(1, sorted(_fscore_issuer(extra), key=lambda r: (r[0], r[1])), T(2024, 1, 1), {})
    last = ev[-1]
    assert last["fscore_n"] == 9.0 and last["fscore"] == last["fscore_partial"]


# --- tiers ---------------------------------------------------------------------------------------------------

def test_v1_tier_wins_over_extension_for_ttm():
    """Four v1 quarters (sum 100) beat an extension-tier fiscal-year fact (999) at the same end."""
    rows = quarters("Revenues", [10.0, 20.0, 30.0, 40.0]) + [
        row(C1, "a1", "OperatingLeaseLeaseIncome", FY_START, FY_END, 999.0)]
    s = fi.Snapshot(knowledge(rows))
    assert s.ttm("sale", FY_END) == 100.0
    only_ext = [row(C1, "a1", "OperatingLeaseLeaseIncome", FY_START, FY_END, 999.0)]
    assert fi.Snapshot(knowledge(only_ext)).ttm("sale", FY_END) == 999.0


def test_ifrs_concepts_fill_the_same_items():
    rows = [row(C1, "a1", "ifrs-full:Assets", None, FY_END, 500.0, "USD", "20-F"),
            row(C1, "a1", "ifrs-full:Revenue", FY_START, FY_END, 300.0, "USD", "20-F"),
            row(C1, "a1", "ifrs-full:CostOfSales", FY_START, FY_END, 120.0, "USD", "20-F"),
            row(C1, "a1", "ifrs-full:ProfitLossFromOperatingActivities", FY_START, FY_END, 90.0, "USD", "20-F"),
            row(C1, "a1", "ifrs-full:IncomeTaxExpenseContinuingOperations", FY_START, FY_END, 20.0, "USD", "20-F")]
    ev, _ = fi.company_events(1, rows, T(2024, 1, 1), {})
    e = ev[0]
    assert e["at"] == 500.0 and e["sale_ttm"] == 300.0 and e["gp_ttm"] == 180.0 and e["oi_ttm"] == 90.0
    assert e["gp_src"] == "sale_minus_cogs" and e["currency"] == "USD" and e["staleness_days"] == 400


# --- fallbacks and zero fills -----------------------------------------------------------------------------

def _base_is(extra, sic=None, pre=None):
    rows = [row(C1, "a1", "Assets", None, FY_END, 1000.0), row(C1, "a1", "NetIncomeLoss", FY_START, FY_END, 50.0),
            row(C1, "a1", "Revenues", FY_START, FY_END, 800.0)] + extra
    ev, _ = fi.company_events(1, rows, T(2024, 1, 1), {}, "USD", {"a1": sic} if sic else None,
                              {"a1": pre} if pre else None)
    return ev[0]


PRE_NO_LINES = {"has_is": True, "has_cf": True, "is_rd": False, "is_rev": True, "is_tax": False, "is_int": False,
                "cf_capx": False}


def test_oi_fallback_pretax_plus_interest_and_bank_block():
    extra = [row(C1, "a1", "IncomeLossFromContinuingOperationsBeforeIncomeTaxesExtraordinaryItemsNoncontrollingInterest",
                 FY_START, FY_END, 100.0), row(C1, "a1", "InterestExpense", FY_START, FY_END, 20.0)]
    e = _base_is(extra)
    assert e["oi_ttm"] == 120.0 and e["oi_src"] == "pretax_plus_interest" and e["xint_ttm"] == 20.0
    assert _base_is(extra, sic=6022)["oi_ttm"] is None                     # commercial bank: no fallback
    assert _base_is(extra, sic=6211)["oi_ttm"] == 120.0                    # broker-dealer: fallback applies
    e = _base_is([extra[0]], pre=PRE_NO_LINES)                             # no interest line on the IS
    assert e["oi_ttm"] == 100.0 and e["oi_src"] == "pretax_no_interest_line" and e["xint_ttm"] == 0.0
    reported = extra + [row(C1, "a1", "OperatingIncomeLoss", FY_START, FY_END, 110.0)]
    assert _base_is(reported)["oi_ttm"] == 110.0 and _base_is(reported)["oi_src"] == "OperatingIncomeLoss"


def test_xrd_zero_only_when_the_income_statement_has_no_rd_line():
    e = _base_is([], pre=PRE_NO_LINES)
    assert e["xrd_ttm"] == 0.0 and e["xrd_reported_zero"] is True and "xrd_ttm" in e["zero_filled"].split(",")
    e = _base_is([], pre={**PRE_NO_LINES, "is_rd": True})                  # R&D line under a custom tag
    assert e["xrd_ttm"] is None and e["xrd_reported_zero"] is False
    e = _base_is([], pre={**PRE_NO_LINES, "has_is": False})               # statement missing
    assert e["xrd_ttm"] is None
    e = _base_is([])                                                       # no PRE flags: ni_ttm evidences the IS
    assert e["xrd_ttm"] == 0.0 and e["xrd_reported_zero"] is True
    e = _base_is([row(C1, "a1", "ResearchAndDevelopmentExpense", FY_START, FY_END, 70.0)], pre=PRE_NO_LINES)
    assert e["xrd_ttm"] == 70.0 and e["xrd_reported_zero"] is False


def test_xrd_nan_when_rd_facts_exist_but_no_ttm():
    old = [row(C1, "a1", "ResearchAndDevelopmentExpense", D(2023, 7, 1), D(2023, 9, 30), 5.0)]
    assert _base_is(old, pre=PRE_NO_LINES)["xrd_ttm"] is None


def test_sale_zero_fill_and_gp_fallback():
    rows = [row(C1, "a1", "Assets", None, FY_END, 1000.0), row(C1, "a1", "NetIncomeLoss", FY_START, FY_END, -50.0)]
    pre = {"a1": {**PRE_NO_LINES, "is_rev": False}}
    ev, _ = fi.company_events(1, rows, T(2024, 1, 1), {}, "USD", None, pre)
    assert ev[0]["sale_ttm"] == 0.0 and ev[0]["sale_src"] == "zero_no_revenue_line"
    ev, _ = fi.company_events(1, rows, T(2024, 1, 1), {}, "USD", {"a1": 6022}, pre)
    assert ev[0]["sale_ttm"] is None                                       # never for banks
    e = _base_is([row(C1, "a1", "CostOfRealEstateRevenue", FY_START, FY_END, 300.0)])
    assert e["cogs_ttm"] == 300.0 and e["gp_ttm"] == 500.0 and e["gp_src"] == "sale_minus_cogs"


def test_capx_and_tax_zero_fill():
    rows = [row(C1, "a1", "NetCashProvidedByUsedInOperatingActivities", FY_START, FY_END, 90.0)]
    e = _base_is(rows, pre=PRE_NO_LINES)
    assert e["capx_ttm"] == 0.0 and "capx_ttm" in e["zero_filled"]
    e = _base_is(rows, pre={**PRE_NO_LINES, "cf_capx": True})
    assert e["capx_ttm"] is None


def test_stock_zero_fills_need_assets_and_no_recent_fact():
    e = _base_is([])
    assert e["gdwl"] == 0.0 and e["intan"] == 0.0 and e["mib"] == 0.0 and e["pstk"] == 0.0
    e = _base_is([row(C1, "a1", "Goodwill", None, D(2023, 3, 31), 40.0)])  # goodwill 9 months earlier
    assert e["gdwl"] is None
    e = _base_is([row(C1, "a1", "FiniteLivedIntangibleAssetsNet", None, FY_END, 30.0),
                  row(C1, "a1", "IndefiniteLivedIntangibleAssetsExcludingGoodwill", None, FY_END, 5.0)])
    assert e["intan"] == 35.0


def test_ebitda_and_buyback_point_values():
    extra = [row(C1, "a1", "OperatingIncomeLoss", FY_START, FY_END, 110.0),
             row(C1, "a1", "DepreciationDepletionAndAmortization", FY_START, FY_END, 15.0),
             row(C1, "a1", "StockRepurchaseProgramAuthorizedAmount1", None, D(2023, 5, 2), 500.0)]
    e = _base_is(extra)
    assert e["ebitda_ttm"] == 125.0 and e["buyback_authorized"] == 500.0 and e["buyback_remaining"] is None


# --- shares --------------------------------------------------------------------------------------------------

def test_class_sum_shares_are_the_last_fallback():
    rows = [row(C1, "a1", "Assets", None, FY_END, 1000.0),
            row(C1, "a1", "cls:CommonStockSharesOutstanding", None, D(2023, 12, 31), 300.0, "shares")]
    ev, _ = fi.company_events(1, rows, T(2024, 1, 1), {})
    assert ev[0]["shrs_q"] == 300.0 and ev[0]["shrs_src"] == "cls_cso"
    rows.append(row(C1, "a1", "WeightedAverageNumberOfSharesOutstandingBasic", FY_START, FY_END, 280.0, "shares"))
    ev, _ = fi.company_events(1, rows, T(2024, 1, 1), {})
    assert ev[0]["shrs_q"] == 280.0 and ev[0]["shrs_src"] == "waso"       # v1 source unchanged


def test_class_sum_matches_month_end_rounded_ddate():
    end = D(2023, 12, 30)                                                  # 52/53-week year
    rows = [row(C1, "a1", "Assets", None, end, 1000.0),
            row(C1, "a1", "cls:CommonStockSharesOutstanding", None, D(2023, 12, 31), 300.0, "shares")]
    ev, _ = fi.company_events(1, rows, T(2024, 1, 1), {})
    assert ev[0]["period_end"] == end and ev[0]["shrs_q"] == 300.0


# --- currency ------------------------------------------------------------------------------------------------

def test_non_usd_reporter_money_nan_unitless_from_native_pass():
    rows = []
    for y, at, ni in ((2021, 1000.0, 50.0), (2022, 1100.0, 60.0), (2023, 1200.0, 80.0)):
        c, a, s, e = T(y + 1, 4, 10, 10, 0), f"fy{y}", D(y, 1, 1), D(y, 12, 31)
        rows += [row(c, a, "ifrs-full:Assets", None, e, at, "TWD", "20-F"),
                 row(c, a, "ifrs-full:ProfitLoss", s, e, ni, "TWD", "20-F"),
                 row(c, a, "ifrs-full:CashFlowsFromUsedInOperatingActivities", s, e, ni * 1.5, "TWD", "20-F"),
                 row(c, a, "ifrs-full:Revenue", s, e, at, "TWD", "20-F"),
                 row(c, a, "ifrs-full:GrossProfit", s, e, at / 3, "TWD", "20-F"),
                 row(c, a, "ifrs-full:LongtermBorrowings", None, e, 100.0, "TWD", "20-F"),
                 row(c, a, "ifrs-full:ProceedsFromIssuingShares", s, e, 0.0, "TWD", "20-F"),
                 row(c, a, "EntityCommonStockSharesOutstanding", None, D(y + 1, 3, 31), 1e9, "shares", "20-F")]
    counters: dict[str, int] = {}
    hist: list[tuple] = []
    ev = fi.issuer_events(7, rows, T(2022, 1, 1), counters, None, None, hist)
    last = ev[-1]
    assert last["currency"] == "TWD"
    assert last["at"] is None and last["ni_ttm"] is None and last["sale_ttm"] is None   # USD columns only
    assert last["shrs_q"] == 1e9                                                        # shares are unit free
    assert last["fscore_n"] == 8.0 and last["f_roa"] == 1 and last["fscore_partial"] is not None
    assert counters["events_unitless_from_native"] == len(ev)
    at_rows = [h for h in hist if h[0] == "at"]
    assert at_rows and all(h[7] == "TWD" for h in at_rows) and at_rows[-1][4] == 1200.0


def test_reporting_currency_tie_prefers_usd():
    from collections import Counter
    assert fi.reporting_currency(Counter({"CNY": 5, "USD": 5})) == "USD"
    assert fi.reporting_currency(Counter({"CNY": 6, "USD": 5})) == "CNY"
    assert fi.reporting_currency(Counter()) is None


# --- staleness and history -------------------------------------------------------------------------------------

def test_staleness_days_rule():
    clk = T(2024, 6, 1)
    assert fi.staleness_days([T(2023, 8, 1)], clk) == 200
    assert fi.staleness_days([T(2023, 4, 1)], clk) == 400            # 427 days earlier
    assert fi.staleness_days([T(2023, 8, 1), T(2024, 7, 1)], clk) == 200
    assert fi.staleness_days([], clk) == 400


def test_quarterly_history_vintages():
    q1s, q1e = D(2024, 1, 1), D(2024, 3, 31)
    c1, c2 = T(2024, 5, 1, 20, 0), T(2024, 8, 1, 20, 0)
    rows = [row(c1, "q1", "Revenues", q1s, q1e, 100.0, form="10-Q", fy=2024, fp="Q1"),
            row(c1, "q1", "Assets", None, q1e, 900.0, form="10-Q", fy=2024, fp="Q1"),
            # Q2 10-Q repeats Q1 assets (no new vintage) and restates Q1 revenue (new vintage)
            row(c2, "q2", "Revenues", q1s, q1e, 104.0, form="10-Q", fy=2024, fp="Q2"),
            row(c2, "q2", "Assets", None, q1e, 900.0, form="10-Q", fy=2024, fp="Q2"),
            row(c2, "q2", "Revenues", D(2024, 4, 1), D(2024, 6, 30), 120.0, form="10-Q", fy=2024, fp="Q2")]
    hist: list[tuple] = []
    ev = fi.issuer_events(1, rows, T(2024, 1, 1), {}, None, None, hist)
    assert [e["staleness_days"] for e in ev] == [200, 200]
    sale_q1 = [h for h in hist if h[0] == "sale_q" and h[1] == q1e]
    assert [(h[3], h[4], h[5]) for h in sale_q1] == [("q1", 100.0, c1), ("q2", 104.0, c2)]
    assert all(h[2] == "Q1" for h in sale_q1)                         # label of the filing that first reported it
    assert len([h for h in hist if h[0] == "at" and h[1] == q1e]) == 1
    assert [h[4] for h in hist if h[0] == "sale_q" and h[1] == D(2024, 6, 30)] == [120.0]


def test_legacy_values_unchanged_by_extension_concepts():
    """Adding extension-tier facts never changes an item the v1 chains already derive."""
    base = [row(C1, "a1", "Assets", None, FY_END, 1000.0), row(C1, "a1", "NetIncomeLoss", FY_START, FY_END, 50.0)]
    base += quarters("Revenues", [10.0, 20.0, 30.0, 40.0])
    base += quarters("CostOfRevenue", [5.0, 5.0, 5.0, 5.0])
    ext = [row(C1, "a1", "CostOfRealEstateRevenue", FY_START, FY_END, 999.0),
           row(C1, "a1", "RealEstateRevenueNet", FY_START, FY_END, 999.0),
           row(C1, "a1", "IncomeLossFromContinuingOperationsBeforeIncomeTaxesExtraordinaryItemsNoncontrollingInterest",
               FY_START, FY_END, 70.0)]
    a, _ = fi.company_events(1, base, T(2024, 1, 1), {})
    b, _ = fi.company_events(1, base + ext, T(2024, 1, 1), {})
    for c in fi.ITEM_COLUMNS:
        va, vb = a[0][c], b[0][c]
        assert va == vb or (va is None and vb is not None) or (isinstance(va, float) and math.isnan(va)), c


# --- nil-as-zero, Compustat-style cost of revenue, quarterly tax ------------------------------------------------

def test_nil_current_value_reads_as_zero():
    """A 10-Q that reports prior-year 9M capex but no current 9M capex presented a blank: 0 for the current 9M."""
    c10k, c9m = T(2024, 3, 1, 21, 0), T(2024, 11, 5, 21, 0)
    rows = [row(c10k, "k23", "PaymentsToAcquirePropertyPlantAndEquipment", D(2023, 1, 1), D(2023, 12, 31), 50.0),
            row(c10k, "k23", "PaymentsToAcquirePropertyPlantAndEquipment", D(2023, 1, 1), D(2023, 9, 30), 30.0),
            row(c9m, "q324", "NetCashProvidedByUsedInOperatingActivities", D(2024, 1, 1), D(2024, 9, 30), 80.0, form="10-Q"),
            row(c9m, "q324", "Assets", None, D(2024, 9, 30), 900.0, form="10-Q"),
            row(c9m, "q324", "PaymentsToAcquirePropertyPlantAndEquipment", D(2023, 1, 1), D(2023, 9, 30), 30.0, form="10-Q")]
    counters: dict[str, int] = {}
    ev, _ = fi.company_events(1, rows, T(2024, 1, 1), counters)
    assert counters["nil_zero_facts"] == 1
    assert ev[-1]["capx_ttm"] == 20.0                      # 0 (9M 2024) + 50 (FY 2023) - 30 (9M 2023)


def test_nil_zero_never_undoes_a_nonzero_shorter_period():
    c6, c9 = T(2024, 8, 1, 21, 0), T(2024, 11, 5, 21, 0)
    rows = [row(c6, "q2", "PaymentsToAcquirePropertyPlantAndEquipment", D(2024, 1, 1), D(2024, 6, 30), 12.0, form="10-Q"),
            row(c9, "q3", "Assets", None, D(2024, 9, 30), 900.0, form="10-Q"),
            row(c9, "q3", "NetCashProvidedByUsedInOperatingActivities", D(2024, 1, 1), D(2024, 9, 30), 80.0, form="10-Q"),
            row(c9, "q3", "PaymentsToAcquirePropertyPlantAndEquipment", D(2023, 1, 1), D(2023, 9, 30), 30.0, form="10-Q")]
    counters: dict[str, int] = {}
    fi.company_events(1, rows, T(2024, 1, 1), counters)
    assert "nil_zero_facts" not in counters


def test_gp_compustat_style_cost_of_revenue():
    extra = [row(C1, "a1", "CostsAndExpenses", FY_START, FY_END, 700.0),
             row(C1, "a1", "SellingGeneralAndAdministrativeExpense", FY_START, FY_END, 150.0),
             row(C1, "a1", "DepreciationDepletionAndAmortization", FY_START, FY_END, 50.0)]
    e = _base_is(extra)
    assert e["gp_ttm"] == 800.0 - (700.0 - 150.0 - 50.0) and e["gp_src"] == "sale_minus_opcost_ex_sga_rd_dp"
    e = _base_is(extra + [row(C1, "a1", "CostOfRevenue", FY_START, FY_END, 450.0)])
    assert e["gp_ttm"] == 350.0 and e["gp_src"] == "sale_minus_cogs"


def test_quarterly_tax_zero_when_that_quarter_shows_no_tax_line():
    cq = T(2024, 5, 5, 21, 0)
    rows = [row(C1, "k23", "IncomeTaxExpenseBenefit", FY_START, FY_END, 9.0),
            row(cq, "q1", "Assets", None, D(2024, 3, 31), 900.0, form="10-Q"),
            row(cq, "q1", "NetIncomeLoss", D(2024, 1, 1), D(2024, 3, 31), 4.0, form="10-Q")]
    pre = {"q1": PRE_NO_LINES}
    ev, _ = fi.company_events(1, rows, T(2024, 4, 1), {}, "USD", None, pre)
    assert ev[-1]["txt_q"] == 0.0 and "txt_q" in ev[-1]["zero_filled"]
    ev, _ = fi.company_events(1, rows, T(2024, 4, 1), {}, "USD", None, {"q1": {**PRE_NO_LINES, "is_tax": True}})
    assert ev[-1]["txt_q"] is None
