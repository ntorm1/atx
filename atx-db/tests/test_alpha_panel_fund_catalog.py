"""S4.2 catalog items (fund_catalog + fund_items.catalog_items): chains, statement-line zero rule, derived items,
per-share and average-share kinds, industry applicability, the FSDS PRE label fallback tier."""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db.alpha_panel import fund_catalog as fcat
from atx_db.alpha_panel import fund_items as fi

D = dt.date
T = dt.datetime
C1 = T(2024, 2, 20, 21)
S, E = D(2023, 1, 1), D(2023, 12, 31)


def row(concept, start, end, value, unit="USD", clock=C1, accn="a1", form="10-K"):
    return (clock, accn, fi.CID[concept], start, end, value, unit, form, clock.date(), 2023, "FY", "fsds_accepted_utc")


BASE = [row("Assets", None, E, 1000.0), row("StockholdersEquity", None, E, 400.0),
        row("NetIncomeLoss", S, E, 50.0), row("Revenues", S, E, 800.0),
        row("NetCashProvidedByUsedInOperatingActivities", S, E, 90.0)]


def ev(extra=(), sic=None, pre=None, **kw):
    rows = sorted(BASE + list(extra), key=lambda r: (r[0], r[1], r[2]))
    out, _ = fi.company_events(1, rows, T(2024, 1, 1), {}, "USD", {"a1": sic} if sic else None,
                               {"a1": pre} if pre else None, **kw)
    return out[-1]


PRE_BASE = {f: False for f in fi.PRE_FLAGS} | {"has_is": True, "has_cf": True, "has_bs": True, "is_rev": True}


def test_catalog_shape():
    assert len(fcat.CATALOG) >= 70 and len(set(fcat.CAT_COLUMNS)) == len(fcat.CATALOG)
    assert not set(fcat.CAT_COLUMNS) & set(fi.ALL_ITEMS)                       # no clash with the v9 columns
    assert all(c.kind in ("stock", "flow", "per_share", "avg_shares", "derived") for c in fcat.CATALOG)
    assert all(c.chain or c.kind == "derived" for c in fcat.CATALOG)
    assert set(fi.CAT_FX_BALANCE) | set(fi.CAT_FX_FLOW) | set(fi.CAT_SHARES) == set(fcat.CAT_COLUMNS)


def test_stock_chain_and_statement_zero_rule():
    e = ev([row("RetainedEarningsAccumulatedDeficit", None, E, 300.0), row("TreasuryStockValue", None, E, 25.0)])
    assert e["re"] == 300.0 and e["tstk"] == 25.0 and "tstk" not in e["catalog_zero_filled"]
    e = ev(pre=PRE_BASE)                                                     # BS shown, no treasury line
    assert e["tstk"] == 0.0 and "tstk" in e["catalog_zero_filled"].split(",")
    e = ev(pre=PRE_BASE | {"c_tstk": True})                                  # a treasury line under a custom tag
    assert e["tstk"] is None
    e = ev()                                                                 # no PRE: assets evidence the BS
    assert e["tstk"] == 0.0
    assert e["re"] is None                                                   # no zero rule without statement evidence?
    e = ev(pre=PRE_BASE | {"c_re": True})
    assert e["re"] is None


def test_flow_chain_and_cash_flow_zero_rule():
    e = ev([row("ProceedsFromIssuanceOfLongTermDebt", S, E, 120.0), row("RepaymentsOfLongTermDebt", S, E, 80.0)])
    assert e["dltis_ttm"] == 120.0 and e["dltr_ttm"] == 80.0
    e = ev(pre=PRE_BASE)
    assert e["dltis_ttm"] == 0.0 and e["aqc_ttm"] == 0.0 and e["stkco_ttm"] == 0.0
    assert e["ivncf_ttm"] is None                                            # totals have no zero rule


def test_derived_items():
    extra = [row("PreferredStockValue", None, E, 40.0), row("MinorityInterest", None, E, 10.0),
             row("Goodwill", None, E, 70.0), row("IntangibleAssetsNetExcludingGoodwill", None, E, 30.0),
             row("OperatingIncomeLoss", S, E, 120.0), row("DepreciationDepletionAndAmortization", S, E, 20.0),
             row("IncomeTaxExpenseBenefit", S, E, 15.0), row("AssetsCurrent", None, E, 500.0),
             row("LiabilitiesCurrent", None, E, 300.0), row("LongTermDebtNoncurrent", None, E, 200.0),
             row("PaymentsOfDividendsCommonStock", S, E, 12.0),
             row("PaymentsOfDividendsPreferredStockAndPreferenceStock", S, E, 3.0)]
    e = ev(extra)
    assert e["ceq"] == 360.0 and e["teq"] == 410.0 and e["lse"] == 1000.0 and e["intano"] == 100.0
    assert e["wcap"] == 200.0 and e["xopr_ttm"] == 800.0 - 140.0
    assert e["dlc"] == 0.0 and e["dltt"] == 200.0 and e["txditc"] == 0.0
    assert e["txt_ttm"] == 15.0 and e["pi_ttm"] == 65.0                     # pretax = ni + txt fallback
    assert e["do_ttm"] == 0.0 and e["ib_ttm"] == 50.0                       # no discontinued line: ib = ni
    assert e["dv_ttm"] == 15.0                                               # dvc + preferred dividends paid


def test_per_share_and_average_shares():
    q = [(D(2023, 1, 1), D(2023, 3, 31)), (D(2023, 4, 1), D(2023, 6, 30)), (D(2023, 7, 1), D(2023, 9, 30)),
         (D(2023, 10, 1), D(2023, 12, 31))]
    extra = [row("EarningsPerShareBasic", S, E, 1.25, unit="USD/shares"),
             row("EarningsPerShareDiluted", S, E, 1.20, unit="USD/shares")]
    extra += [row("WeightedAverageNumberOfDilutedSharesOutstanding", s0, e0, v, unit="shares")
              for (s0, e0), v in zip(q, (100.0, 102.0, 104.0, 106.0))]
    extra += [row("WeightedAverageNumberOfDilutedSharesOutstanding", D(2023, 1, 1), D(2023, 6, 30), 101.0, unit="shares")]
    e = ev(extra)
    assert e["epspx_ttm"] == 1.25 and e["epsfx_ttm"] == 1.20
    assert e["epspi_ttm"] == 1.25                                            # no discontinued operations
    assert e["cshfd"] == pytest.approx(103.0)                                # mean of the four 3-month facts
    assert e["currency"] == "USD"                                            # per-share units never set the currency


def test_industry_items_applicability():
    extra = [row("ProvisionForLoanAndLeaseLosses", S, E, 9.0), row("Deposits", None, E, 700.0)]
    bank = ev(extra, sic=6022)
    assert bank["pcl_ttm"] == 9.0 and bank["dptc"] == 700.0
    tech = ev(extra, sic=3674)
    assert tech["pcl_ttm"] is None and tech["dptc"] is None
    assert fi.cat_structural("other", 3674, "pcl_ttm") and not fi.cat_structural("bank", 6022, "pcl_ttm")
    assert fi.cat_structural("bank", 6211, "dptc") and fi.cat_structural("bank", 6022, "wcap")


def test_label_fallback_tier():
    """A custom-tag 'Gross profit' line (pseudo concept lbl:GrossProfit) fills gp when no standard concept does;
    a standard GrossProfit fact wins over it."""
    e = ev([row("lbl:GrossProfit", S, E, 333.0)])
    assert e["gp_ttm"] == 333.0 and e["gp_src"] == "GrossProfit"
    e = ev([row("lbl:GrossProfit", S, E, 333.0), row("GrossProfit", S, E, 330.0)])
    assert e["gp_ttm"] == 330.0
    e = ev([row("lbl:CostOfRevenue", S, E, 500.0)])
    assert e["cogs_ttm"] == 500.0 and e["gp_ttm"] == 300.0 and e["gp_src"] == "sale_minus_cogs"
    e = ev([row("lbl:OperatingIncomeLoss", S, E, 111.0)])
    assert e["oi_ttm"] == 111.0
