"""Stage ``ratios`` (tier1-v4 task 4; gap analysis section 2 item 3): a WRDS Financial Ratios Suite (WFRS) analog,
point in time, built from ``compustat_std`` (``--std-stage``; task 5 rebuilds on the v11 stage) and the market stages
(``market/shares_daily``, ``prices``). No new source.

Outputs (rule ``wfrs-pit-v2``, schema ``atx.alpha-panel.ratios/v2``):

* ``ratios/fund_state.parquet`` -- issuer fundamental state vintages keyed (cik, available_at): one row per distinct
  ``available_at`` of the issuer's ``fundq`` / ``funda`` rows (every filing, amendment and restatement vintage). A
  row holds what ``fundq_asof(t)`` / ``funda_asof(t)`` return for any t just after its ``available_at`` (rows with
  ``available_at`` <= the row's, the latest vintage per (cik, datadate) by (available_at, accession)): trailing
  flows, balances, averages, annual history and every price-free ratio. Consumers take the latest row with
  ``available_at`` < their cutoff (strict, GC1).
* ``ratios/split_events.parquet`` -- issuer split / reverse-split events (vendor ``corporate_actions`` of the
  issuer's common lines) with the cumulative log ratio, used to put per-share EPS on one share basis.
* ``ratios/monthly/year=YYYY/ratios_monthly.parquet`` -- (session_date = month-end session m, security_id) for every
  common line (link_table_v3 ``linkprim`` P/J, point-in-time tiers strict / dated / name known by 22:00 UTC of m)
  with a price at m, from 2018-01 to the last complete calendar month. GC1 monthly: fundamentals with
  ``available_at`` < 22:00 UTC of m; prices and shares of m. ``available_at`` = 22:00 UTC of m.
* ``ratios_asof(d)`` (``macros_sql``) -- the daily as-of view: the same columns for session d from the state row with
  ``available_at`` < 22:00 UTC of session d-1 and the shares / raw close of d (GC1 daily).

Definitions (``RATIOS``, WFRS names; 66 original ratios + 3 honestly named extensions + 4 composite scores). Inputs per issuer state:

* Quarter lags: the visible fundq rows (latest vintage per datadate) are placed on quarter offsets j = round((D -
  datadate) / 91.3125 days) from the latest datadate D, kept when within 20 days of j quarters (closest wins), j = 0..8.
* TTM flows (``<x>_ttm``): when the lag-0 row is fiscal quarter 4 and an genuine annual-period funda vintage of D is visible,
  the fiscal-year value; else the sum of the four discrete quarters j = 0..3 (all four rows present and non-NULL);
  else (WFRS: annual data when quarterly data are unavailable) the latest fiscal-year value FY0 when its datadate is
  within 365 days of D (``ttm_fy0_items`` lists the items that fell back); else NULL -- a missing quarter is never
  zero-filled. YTD cash-flow items (oancfy capxy dvy xrdy sstky prstkcy): fqtr 4 = the YTD (the fiscal year);
  fqtr k < 4 = YTD(j0) + YTD(prior fiscal-year end, j = k, fqtr 4) - YTD(j = 4, same fqtr); else the FY0 fallback.
  ``ttm_src`` = ``annual`` / ``q4`` / NULL (the row structure).
* Balances: lag 0 (``<x>_q0``) and lag 4 (``<x>_q4``, the same quarter a year earlier); averages ("most recent two
  periods", WFRS) = (q0 + q4) / 2, NULL when either balance is missing; ``avg_fallback`` flags unavailable lag-4 assets.
* EPS: per-share TTM sums put every quarter on the share basis of the state's date by the issuer's split events
  between the quarter's filing date and that date (a split before a filing is already restated in it); ``csho_b`` =
  the lag-0 cshoq on the same basis. ``epsfi_ttm`` (incl. discontinued operations): annual EPSFI or a flagged FY0 fallback;
  fundq carries no quarterly total EPSFI, so no NI/IB inference is permitted.
* Book equity ``be`` = seq + txditc - pstk (the fundamentals rule; txditc from the latest annual row within 400 days,
  else NULL). Invested capital ``icapt`` = dltt + pstk + mib + ceq. EV debt = dltt + dlc + pstk + mib -
  che. Unreported ``mib`` remains NULL; every missing input makes the derived ratio NULL.
* Annual rows (funda, including later comparative restatements): FY0 = the latest visible fiscal year, FY-k by 365.25-day offsets (+-30 d).
  ``ptpm``, ``efftax``, ``adv_sale`` (no quarterly pretax income / advertising in fundq), ``capei``, the EPS growth
  of ``peg_trailing`` and the composite scores use annual values.
* Market equity ``me`` = sum over the issuer's common lines (linkprim P/J, modal FINRA type not preferred / warrant /
  unit / right / note / ETF / ETN / fund -- the market_shares rule) of ``shares_daily.shrout`` x raw close of the
  session (NULL when a priced common line has no shrout); ``prc`` = the line's raw close. P/E uses issuer aggregate ME / NI or IB in USD. It is an explicitly qualified cap/earnings analog;
  diluted EPS, cover-date shares and ordinary/ADR per-share conventions do not enter its denominator; ``eps_adj`` reports the issuer splits between the state's date and
  the session.
* Staleness: quarterly-based values are NULL when session - D > 400 days (550 when the lag-0 form is 20-F / 40-F,
  annual-only filers); annual-based values when session - FY0 datadate > 550 days.

Commands (every one under ``run_memory_guarded.py``)::

    python -m atx_db.alpha_panel.ratios build    [--std-stage compustat_std] [--stage ratios]
    python -m atx_db.alpha_panel.ratios validate [--stage ratios]        # coverage, spearman, ranges
    python -m atx_db.alpha_panel.ratios handcheck [--n 20]               # SEC 10-K inline XBRL (network)
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import os
import random
import re
import sys
import time
from pathlib import Path
import hashlib
from typing import Any

from . import common as C

STAGE = "ratios"
SCHEMA = "atx.alpha-panel.ratios/v2"
RULE = "wfrs-pit-v2"
MODULES = ("ratios", "common")
DEFAULT_STD_STAGE = "compustat_std"
N_BUCKETS = 8
MEM = os.environ.get("ATX_RATIOS_DUCKDB_MEM", "450MB")
Q_DAYS, Q_TOL, Q_LAGS = 91.3125, 20, 8
Y_DAYS, Y_TOL, Y_LAGS = 365.25, 30, 5
STALE_DAYS, STALE_ANNUAL_ONLY_DAYS, FY_STALE_DAYS, TXDITC_DAYS = 400, 550, 550, 400
FY0_FALLBACK_DAYS = 365                  # TTM falls back to FY0 when the quarterly path is NULL
ROWS_FROM = dt.date(2008, 1, 1)          # fundq / funda datadate lookback (capei: five fiscal years before 2015)
EVENTS_FROM = dt.datetime(2015, 1, 1)    # state vintages from (GC3: fundamentals from 2015)
MONTHLY_FROM = dt.date(2018, 1, 1)       # shares_daily starts 2018-01-02
ANNUAL_FORMS = ("10-K", "10-K/A", "10-KT", "10-KT/A", "20-F", "20-F/A", "40-F", "40-F/A")
ANNUAL_ONLY_FORMS = ("20-F", "20-F/A", "40-F", "40-F/A")
COMMON_LINKPRIM = ("P", "J")
PIT_LINK_TIERS = ("strict", "dated", "name")
NON_EQUITY_TYPES = ("preferred", "warrant", "unit", "right", "note", "ETF", "ETN", "fund")   # market_shares rule
GATE_YEARS = tuple(range(2020, 2026))
COVERAGE_GATE, SPEARMAN_GATE = 0.90, 0.99

# fundq columns by role (state name -> fundq column)
FLOW_Q = {"sale": "saleq", "cogs": "cogsq", "xsga": "xsgaq", "oiadp": "oiadpq", "oibdp": "oibdpq", "dp": "dpq",
          "xint": "xintq", "txt": "txtq", "ni": "niq", "ib": "ibq"}
EPS_Q = {"epsfx": "epsfxq", "epspx": "epspxq"}
YTD_Q = {"oancf": "oancfy", "capx": "capxy", "dvc": "dvy", "xrd": "xrdy", "sstk": "sstky", "prstkc": "prstkcy"}
BAL_Q = {"at": "atq", "lt": "ltq", "ceq": "ceqq", "seq": "seqq", "che": "cheq", "act": "actq", "lct": "lctq",
         "invt": "invtq", "rect": "rectq", "ap": "apq", "ppent": "ppentq", "dlc": "dlcq", "dltt": "dlttq",
         "intan": "intanq", "pstk": "pstkq", "mib": "mibq", "csho": "cshoq"}
# funda items per fiscal-year lag k (Compustat names; epspx/epsfx = excl. discontinued operations, epsfi = incl.)
ANN_LAGS: dict[int, tuple[str, ...]] = {
    0: ("sale", "cogs", "xsga", "oiadp", "oibdp", "dp", "xint", "ni", "ib", "pi", "txt", "xad", "at", "lt", "act",
        "lct", "rect", "ppent", "dltt", "re", "oancf", "capx", "dvc", "xrd", "sstk", "prstkc", "txditc", "epsfx",
        "epspx", "epsfi"),
    1: ("sale", "cogs", "xsga", "dp", "ni", "ib", "at", "act", "lct", "rect", "ppent", "dltt", "txditc"),
    2: ("at", "ni"),
    3: ("ni", "epsfx"),
    4: ("ni",),
}
ANNUAL_ONLY_ITEMS = ("pi", "xad", "re", "txditc")


def _q(p: Path | str) -> str:
    return Path(p).as_posix()


def _d(num: str, den: str) -> str:
    return f"CASE WHEN ({den}) <> 0 THEN ({num}) / ({den}) END"


def _ind(cond: str) -> str:
    return f"CASE WHEN ({cond}) IS NULL THEN NULL WHEN ({cond}) THEN 1 ELSE 0 END"


# ---------------------------------------------------------------------------------------------------------------
# ratio definitions: name, family, freq (q quarterly/TTM state, a annual FY0, qa both), price-dependent, numerator,
# denominator (NULL / 0 -> NULL), structural inputs (items whose absence makes the ratio undefined), WFRS definition
# ---------------------------------------------------------------------------------------------------------------

# history-limited: the issuer's first fundq datadate in the lake is later than the earliest fiscal year the measure
# needs (hist_days = days from first_datadate to FY0, else to the lag-0 datadate); annual measures without any annual
# row are history-limited while the issuer has filed for less than 300 days (no 10-K yet)
NO_FY0 = "(fy0_datadate IS NULL AND date_diff('day', first_datadate, datadate) < 300)"


def _hist(days: int) -> str:
    return f"({NO_FY0} OR hist_days < {days})"


def _r(name, family, freq, price, num, den, inputs, definition, expr=None, undef=None):
    if undef is None and freq in ("a", "qa"):
        undef = NO_FY0
    return {"name": name, "family": family, "freq": freq, "price": price, "num": num, "den": den,
            "inputs": tuple(inputs), "definition": definition, "expr": expr or _d(num, den), "undef": undef}


RATIOS: tuple[dict[str, Any], ...] = (
    # valuation
    _r("capei", "valuation", "a", True, "me", "ni_avg5", ["ni"],
       "Shiller's cyclically adjusted P/E: market equity / mean net income of the last five fiscal years (nominal)",
       undef=_hist(1431)),
    _r("bm", "valuation", "q", True, "be_q0", "me", ["seq"], "Book/Market: book equity / market equity"),
    _r("evm", "valuation", "q", True, "me + ev_debt", "oibdp_ttm", ["dltt", "dlc", "che", "oibdp"],
       "Enterprise value multiple: (me + dltt + dlc + pstk + mib - che) / EBITDA (oibdp) TTM"),
    _r("pe_issuer_ib", "valuation", "q", True, "me", "ib_ttm", ["ib"],
       "Issuer aggregate common market equity / income before extraordinary items TTM (USD); cap/earnings analog, not diluted EPS P/E"),
    _r("pe_issuer_ni", "valuation", "q", True, "me", "ni_ttm", ["ni"],
       "Issuer aggregate common market equity / parent net income TTM (USD); cap/earnings analog, not diluted EPS P/E"),
    _r("pe_exi", "valuation", "q", True, "prc", "epsfx_ttm * eps_adj", ["epsfx"],
       "WFRS price / diluted EPS excluding EI; NULL until certified security-class/ADR/EPS share-basis alignment is available", expr="CAST(NULL AS DOUBLE)"),
    _r("pe_inc", "valuation", "q", True, "prc", "epsfi_ttm * eps_adj", ["epsfi"],
       "WFRS price / diluted EPS including EI; NULL until certified security-class/ADR/EPS share-basis alignment is available", expr="CAST(NULL AS DOUBLE)"),
    _r("ps", "valuation", "q", True, "me", "sale_ttm", ["sale"], "Price/Sales: market equity / sales TTM"),
    _r("pcf", "valuation", "q", True, "me", "oancf_ttm", ["oancf"],
       "Price/Cash flow: market equity / operating cash flow TTM"),
    _r("dpr", "valuation", "q", False, "dvc_ttm", "ib_ttm", ["dvc", "ib"],
       "Dividend payout ratio: common cash dividends TTM / income before extraordinary items TTM"),
    _r("peg_trailing", "valuation", "qa", True, _d("prc", "epsfx_ttm * eps_adj"), "eps_g3 * 100", ["epsfx"],
       "Trailing PEG: pe_exi / 3-year compound annual growth (percent) of annual diluted EPS excl. EI (both > 0)",
       expr="CAST(NULL AS DOUBLE)", undef=f"({_hist(1066)} OR epsfx_a0 <= 0 OR epsfx_a3 <= 0)"),
    _r("divyield_paid", "valuation", "q", True, "dvc_ttm", "me", ["dvc"],
       "Dividend yield: common cash dividends TTM / market equity"),
    _r("ptb", "valuation", "q", True, "me", "ceq_q0", ["ceq"], "Price/Book: market equity / common equity"),
    _r("divyield", "valuation", "q", True, "CAST(NULL AS DOUBLE)", "prc", ["indicated_dividend_rate"],
       "Indicated dividend rate / price (WFRS); NULL: no source-backed indicated rate primitive", expr="CAST(NULL AS DOUBLE)"),
    # profitability
    _r("npm", "profitability", "q", False, "ni_ttm", "sale_ttm", ["ni", "sale"],
       "Net profit margin: net income TTM / sales TTM"),
    _r("opmbd", "profitability", "q", False, "oibdp_ttm", "sale_ttm", ["oibdp", "sale"],
       "Operating profit margin before depreciation: oibdp TTM / sales TTM"),
    _r("opmad", "profitability", "q", False, "oiadp_ttm", "sale_ttm", ["oiadp", "sale"],
       "Operating profit margin after depreciation: oiadp TTM / sales TTM"),
    _r("gpm", "profitability", "q", False, "sale_ttm - cogs_ttm", "sale_ttm", ["sale", "cogs"],
       "Gross profit margin: (sales - cogs) TTM / sales TTM"),
    _r("ptpm", "profitability", "a", False, "pi_a0", "sale_a0", ["pi", "sale"],
       "Pre-tax profit margin: pretax income / sales (latest fiscal year)"),
    _r("cfm", "profitability", "q", False, "ib_ttm + dp_ttm", "sale_ttm", ["ib", "dp", "sale"],
       "Cash flow margin: (income before extraordinary items + depreciation) TTM / sales TTM"),
    _r("roa", "profitability", "q", False, "oibdp_ttm", "avg_at", ["oibdp", "at"],
       "Return on assets: oibdp TTM / average total assets"),
    _r("roe", "profitability", "q", False, "ni_ttm", "avg_wrds_be", ["ni", "seq", "txditc"],
       "Return on equity: NI TTM / average (parent stockholder equity + deferred taxes and investment tax credit)"),
    _r("roce", "profitability", "q", False, "ni_ttm + txt_ttm + xint_ttm", "avg_capemp", ["ni", "txt", "xint", "dltt", "dlc", "ceq"],
       "Return on capital employed: EBIT (NI + tax + interest) TTM / average (dltt + dlc + ceq)"),
    _r("efftax", "profitability", "a", False, "txt_a0", "pi_a0", ["txt", "pi"],
       "Effective tax rate: income taxes / pretax income (latest fiscal year)"),
    _r("aftret_eq", "profitability", "q", False, "ni_ttm", "avg_ceq", ["ni", "ceq"],
       "After-tax return on average common equity: NI TTM / average ceq"),
    _r("aftret_invcapx", "profitability", "q", False, "ni_ttm + xint_ttm", "icapt_q0", ["ni", "xint", "dltt", "ceq"],
       "After-tax return on invested capital: (NI + interest) TTM / invested capital"),
    _r("aftret_equity", "profitability", "q", False, "ni_ttm", "avg_seq", ["ni", "seq"],
       "After-tax return on total stockholders' equity: NI TTM / average seq"),
    _r("pretret_noa", "profitability", "q", False, "oiadp_ttm", "avg_noa", ["oiadp", "ppent", "act", "lct"],
       "Pre-tax return on net operating assets: oiadp TTM / average (ppent + act - lct)"),
    _r("pretret_earnat", "profitability", "q", False, "oiadp_ttm", "avg_tea", ["oiadp", "ppent", "act"],
       "Pre-tax return on total earning assets: oiadp TTM / average (ppent + act)"),
    _r("gprof", "profitability", "q", False, "sale_ttm - cogs_ttm", "at_q0", ["sale", "cogs", "at"],
       "Gross profit / total assets (WFRS GProf)"),
    # capitalization
    _r("equity_invcap", "capitalization", "q", False, "ceq_q0", "icapt_q0", ["ceq", "dltt"],
       "Common equity / invested capital"),
    _r("debt_invcap", "capitalization", "q", False, "dltt_q0", "icapt_q0", ["dltt", "ceq"],
       "Long-term debt / invested capital"),
    _r("totdebt_invcap", "capitalization", "q", False, "dltt_q0 + dlc_q0", "icapt_q0", ["dltt", "dlc", "ceq"],
       "Total debt (dltt + dlc) / invested capital"),
    _r("capital_ratio", "capitalization", "q", False, "dltt_q0", "dltt_q0 + ceq_q0 + pstk_q0", ["dltt", "ceq"],
       "Capitalization ratio: dltt / (dltt + ceq + pstk)"),
    # financial soundness
    _r("int_debt", "solvency_fin", "q", False, "xint_ttm", "avg_dltt", ["xint", "dltt"],
       "Interest TTM / average long-term debt"),
    _r("int_totdebt", "solvency_fin", "q", False, "xint_ttm", "avg_totdebt", ["xint", "dltt", "dlc"],
       "Interest TTM / average total debt"),
    _r("cash_lt", "solvency_fin", "q", False, "che_q0", "lt_q0", ["che", "lt"],
       "Cash and short-term investments / total liabilities"),
    _r("invt_act", "solvency_fin", "q", False, "invt_q0", "act_q0", ["invt", "act"], "Inventory / current assets"),
    _r("rect_act", "solvency_fin", "q", False, "rect_q0", "act_q0", ["rect", "act"], "Receivables / current assets"),
    _r("debt_at", "solvency_fin", "q", False, "lt_q0", "at_q0", ["lt", "at"],
       "Total liabilities / total assets (WFRS debt_at)"),
    _r("debt_ebitda", "solvency_fin", "q", False, "dltt_q0 + dlc_q0", "oibdp_ttm", ["dltt", "dlc", "oibdp"],
       "Total debt / EBITDA TTM"),
    _r("short_debt", "solvency_fin", "q", False, "dlc_q0", "dltt_q0 + dlc_q0", ["dltt", "dlc"],
       "Short-term debt / total debt"),
    _r("curr_debt", "solvency_fin", "q", False, "lct_q0", "lt_q0", ["lct", "lt"],
       "Current liabilities / total liabilities"),
    _r("lt_debt", "solvency_fin", "q", False, "dltt_q0", "lt_q0", ["dltt", "lt"],
       "Long-term debt / total liabilities"),
    _r("profit_lct", "solvency_fin", "q", False, "oibdp_ttm", "lct_q0", ["oibdp", "lct"],
       "Operating income before depreciation TTM / current liabilities"),
    _r("ocf_lct", "solvency_fin", "q", False, "oancf_ttm", "lct_q0", ["oancf", "lct"],
       "Operating cash flow TTM / current liabilities"),
    _r("cash_debt", "solvency_fin", "q", False, "oancf_ttm", "dltt_q0 + dlc_q0", ["oancf", "dltt", "dlc"],
       "Operating cash flow TTM / total debt"),
    _r("fcf_ocf", "solvency_fin", "q", False, "oancf_ttm - capx_ttm", "oancf_ttm", ["oancf", "capx"],
       "Free cash flow / operating cash flow"),
    _r("lt_ppent", "solvency_fin", "q", False, "lt_q0", "at_q0 - intan_q0", ["lt", "at", "intan"],
       "Total liabilities / total tangible assets (AT - INTAN)"),
    _r("dltt_be", "solvency_fin", "q", False, "dltt_q0", "be_q0", ["dltt", "seq"], "Long-term debt / book equity"),
    # solvency
    _r("de_ratio", "solvency", "q", False, "lt_q0", "ceq_q0 + pstk_q0", ["lt", "ceq"],
       "Total debt/equity: total liabilities / shareholders' equity (common and preferred)"),
    _r("debt_assets", "solvency", "q", False, "dltt_q0 + dlc_q0", "at_q0", ["dltt", "dlc", "at"],
       "Financial debt / total assets"),
    _r("debt_capital", "solvency", "q", False, "ap_q0 + dlc_q0 + dltt_q0", "ap_q0 + dlc_q0 + dltt_q0 + ceq_q0 + pstk_q0",
       ["ap", "dlc", "dltt", "ceq"], "Total debt/capital: (ap + dlc + dltt) / (ap + dlc + dltt + ceq + pstk)"),
    _r("intcov", "solvency", "q", False, "xint_ttm + ib_ttm", "xint_ttm", ["xint", "ib"],
       "After-tax interest coverage: (xint + ib) TTM / xint TTM"),
    _r("intcov_ratio", "solvency", "q", False, "ni_ttm + txt_ttm + xint_ttm", "xint_ttm", ["ni", "txt", "xint"],
       "Interest coverage ratio: EBIT (NI + tax + interest) TTM / xint TTM"),
    # liquidity
    _r("cash_ratio", "liquidity", "q", False, "che_q0", "lct_q0", ["che", "lct"],
       "Cash and short-term investments / current liabilities"),
    _r("quick_ratio", "liquidity", "q", False, "act_q0 - invt_z0", "lct_q0", ["act", "invt", "lct"],
       "Quick ratio (acid test): (current assets - inventories) / current liabilities"),
    _r("curr_ratio", "liquidity", "q", False, "act_q0", "lct_q0", ["act", "lct"],
       "Current ratio: current assets / current liabilities"),
    _r("cash_conversion", "liquidity", "q", False, None, "cogs_ttm * sale_ttm", ["invt", "rect", "ap", "cogs", "sale"],
       "Cash conversion cycle (days): 365 x (inventories / cogs + receivables / sales - "
       "payables / cogs), current balances and TTM flows",
       expr="CASE WHEN cogs_ttm <> 0 AND sale_ttm <> 0 THEN 365.0 * (invt_q0 / cogs_ttm + rect_q0 / sale_ttm"
            " - ap_q0 / cogs_ttm) END"),
    # efficiency
    _r("at_turn", "efficiency", "q", False, "sale_ttm", "avg_at", ["sale", "at"],
       "Asset turnover: sales TTM / average total assets"),
    _r("inv_turn", "efficiency", "q", False, "cogs_ttm", "avg_invt", ["cogs", "invt"],
       "Inventory turnover: cogs TTM / average inventories"),
    _r("pay_turn", "efficiency", "q", False, "cogs_ttm + (invt_z0 - invt_z4)", "avg_ap", ["cogs", "invt", "ap"],
       "Payables turnover: (cogs TTM + change in inventories over the year) / average accounts payable"),
    _r("rect_turn", "efficiency", "q", False, "sale_ttm", "avg_rect", ["sale", "rect"],
       "Receivables turnover: sales TTM / average receivables"),
    _r("sale_equity", "efficiency", "q", False, "sale_ttm", "seq_q0", ["sale", "seq"],
       "Sales TTM / stockholders' equity"),
    _r("sale_invcap", "efficiency", "q", False, "sale_ttm", "icapt_q0", ["sale", "dltt", "ceq"],
       "Sales TTM / invested capital"),
    _r("sale_nwc", "efficiency", "q", False, "sale_ttm", "act_q0 - lct_q0", ["sale", "act", "lct"],
       "Sales TTM / working capital (act - lct)"),
    # other
    _r("accrual", "other", "q", False, "ib_ttm - oancf_ttm", "avg_at", ["ib", "oancf", "at"],
       "Accruals / average total assets: (ib - oancf) TTM / average at (Sloan sign)"),
    _r("rd_sale", "other", "q", False, "xrd_ttm", "sale_ttm", ["xrd", "sale"], "R&D expense TTM / sales TTM"),
    _r("adv_sale", "other", "a", False, "xad_a0", "sale_a0", ["xad", "sale"],
       "Advertising expense / sales (latest fiscal year)"),
)

# composite scores (annual FY0 / FY-1 / FY-2; altman_z also uses me): name -> (expr, inputs, definition)
_PIO = {
    "f_roa": _ind("pio_roa0 > 0"), "f_cfo": _ind("pio_cfo0 > 0"), "f_droa": _ind("pio_roa0 - pio_roa1 > 0"),
    "f_accrual": _ind("pio_cfo0 > pio_roa0"), "f_dlever": _ind("pio_lev0 - pio_lev1 < 0"),
    "f_dliquid": _ind("pio_liq0 - pio_liq1 > 0"), "f_eqoff": _ind("sstk_a0 <= 0"),
    "f_dmargin": _ind("pio_gm0 - pio_gm1 > 0"), "f_dturn": _ind("pio_turn0 - pio_turn1 > 0"),
}
_BEN = {
    "m_dsri": _d(_d("rect_a0", "sale_a0"), _d("rect_a1", "sale_a1")),
    "m_gmi": _d(_d("sale_a1 - cogs_a1", "sale_a1"), _d("sale_a0 - cogs_a0", "sale_a0")),
    "m_aqi": _d("1 - " + _d("act_a0 + ppent_a0", "at_a0"), "1 - " + _d("act_a1 + ppent_a1", "at_a1")),
    "m_sgi": _d("sale_a0", "sale_a1"),
    "m_depi": _d(_d("dp_a1", "dp_a1 + ppent_a1"), _d("dp_a0", "dp_a0 + ppent_a0")),
    "m_sgai": _d(_d("xsga_a0", "sale_a0"), _d("xsga_a1", "sale_a1")),
    "m_lvgi": _d(_d("lct_a0 + dltt_a0", "at_a0"), _d("lct_a1 + dltt_a1", "at_a1")),
    "m_tata": _d("ib_a0 - oancf_a0", "at_a0"),
}
COMPOSITES: tuple[dict[str, Any], ...] = (
    {"name": "piotroski_f", "family": "composite", "freq": "a", "price": False, "den": None, "undef": _hist(700),
     "expr": " + ".join(_PIO), "inputs": ("ib", "at", "oancf", "dltt", "act", "lct", "sstk", "sale", "cogs"),
     "definition": "Piotroski (2000) F-score: sum of nine indicators on annual data (f_roa ib/at_t-1 > 0; f_cfo "
                   "oancf/at_t-1 > 0; f_droa change in roa > 0; f_accrual cfo > roa; f_dlever change in dltt / "
                   "average at < 0; f_dliquid change in current ratio > 0; f_eqoff sstk = 0; f_dmargin change in "
                   "gross margin > 0; f_dturn change in sale/at_t-1 > 0); NULL unless all nine are defined"},
    {"name": "altman_z", "family": "composite", "freq": "a", "price": True, "den": None, "undef": NO_FY0,
     "expr": "CASE WHEN altman_x IS NOT NULL AND lt_a0 <> 0 AND me IS NOT NULL THEN altman_x + 0.6 * me / lt_a0 END",
     "inputs": ("act", "lct", "at", "re", "oiadp", "lt", "sale"),
     "definition": "Altman (1968) Z = 1.2 (act - lct)/at + 1.4 re/at + 3.3 oiadp/at + 0.6 me/lt + 1.0 sale/at "
                   "(latest fiscal year; me of the session)"},
    {"name": "ohlson_o", "family": "composite", "freq": "a", "price": False, "expr": "ohlson_o", "den": None,
     "undef": _hist(335),
     "inputs": ("at", "lt", "act", "lct", "ni", "oancf"),
     "definition": "Ohlson (1980) model 1 O = -1.32 - 0.407 ln(at, USD millions, undeflated) + 6.03 lt/at - 1.43 "
                   "(act - lct)/at + 0.0757 lct/act - 1.72 [lt > at] - 2.37 ni/at - 1.83 oancf/lt + 0.285 "
                   "[ni < 0 both years] - 0.521 (ni_t - ni_t-1)/(|ni_t| + |ni_t-1|) (latest fiscal year)"},
    {"name": "beneish_m", "family": "composite", "freq": "a", "price": False, "expr": "beneish_m", "den": None,
     "undef": _hist(335),
     "inputs": ("rect", "sale", "cogs", "act", "ppent", "at", "dp", "xsga", "lct", "dltt", "ib", "oancf"),
     "definition": "Beneish (1999) 8-variable M = -4.84 + 0.920 DSRI + 0.528 GMI + 0.404 AQI + 0.892 SGI + 0.115 "
                   "DEPI - 0.172 SGAI + 4.679 TATA - 0.327 LVGI (fiscal years t and t-1; TATA = (ib - oancf)/at)"},
)
COMPONENTS = tuple(_PIO) + tuple(_BEN)


def ratio_names() -> list[str]:
    return [r["name"] for r in RATIOS] + [c["name"] for c in COMPOSITES]


# ---------------------------------------------------------------------------------------------------------------
# split events (issuer level)
# ---------------------------------------------------------------------------------------------------------------

def build_splits(con, root: Path) -> None:
    """``spl (cik, ex_date, split_ratio, lr, cum_lr, security_id)``: vendor split / reverse-split events of lines linked
    (point-in-time tiers, linkprim P/J) to the CIK on the ex-date; one event per (cik, ex_date), the issuer-primary
    (P) line first. ``cum_lr`` = cumulative ln(ratio) through ex_date."""
    ca = _q(root / "corporate_actions" / "vendor_events.parquet")
    lt = _q(root / "identity" / "link_table_v3.parquet")
    tiers = ", ".join(f"'{t}'" for t in PIT_LINK_TIERS)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE spl AS
        SELECT cik, ex_date, split_ratio, lr,
               sum(lr) OVER (PARTITION BY cik ORDER BY ex_date ROWS UNBOUNDED PRECEDING) AS cum_lr, security_id
        FROM (SELECT l.cik, e.ex_date, e.split_ratio, ln(e.split_ratio) AS lr, e.security_id
              FROM read_parquet('{ca}') e
              JOIN read_parquet('{lt}') l ON l.security_id = e.security_id
               AND e.ex_date BETWEEN l.valid_from AND coalesce(l.valid_to, DATE '2100-01-01')
               AND l.link_tier IN ({tiers}) AND l.linkprim IN ('P', 'J') AND l.cik IS NOT NULL
              WHERE e.kind IN ('split', 'reverse_split') AND e.split_ratio > 0
              QUALIFY row_number() OVER (PARTITION BY l.cik, e.ex_date
                                         ORDER BY (l.linkprim = 'P') DESC, e.security_id) = 1)""")


# ---------------------------------------------------------------------------------------------------------------
# issuer fundamental state vintages
# ---------------------------------------------------------------------------------------------------------------

def _load(con, std: Path, where: str) -> None:
    fq, fa = _q(std / "fundq.parquet"), _q(std / "funda.parquet")
    qcols = sorted(set(FLOW_Q.values()) | set(EPS_Q.values()) | set(YTD_Q.values()) | set(BAL_Q.values()))
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE q AS
        SELECT row_number() OVER () AS rid, cik, datadate, fqtr, fyr, accession, form, filed, available_at,
               fin_template, sic, curcd, fx_converted, {', '.join(f'"{c}"' for c in qcols)}
        FROM read_parquet('{fq}') WHERE ({where}) AND datadate >= DATE '{ROWS_FROM}'""")
    acols = sorted({c for v in ANN_LAGS.values() for c in v})
    forms = ", ".join(f"'{f}'" for f in ANNUAL_FORMS)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE a AS
        SELECT row_number() OVER () AS arid, cik, datadate, accession, form, filed, available_at,
               {', '.join(f'"{c}"' for c in acols)}
        FROM read_parquet('{fa}') WHERE ({where}) AND datadate >= DATE '{ROWS_FROM}'""")


def _state_sql() -> str:
    """Final SELECT of the state (over the temp tables ``qp`` / ``ap`` / split joins)."""
    ann_d = "(p.fqtr_q0 = 4 AND ap.datadate_a0 = p.datadate)"
    q4ok = "(p.has_q0 AND p.has_q1 AND p.has_q2 AND p.has_q3)"
    fy0_ok = f"(ap.datadate_a0 IS NOT NULL AND date_diff('day', ap.datadate_a0, p.datadate) <= {FY0_FALLBACK_DAYS})"
    h: dict[str, str] = {}
    fallback: list[str] = []

    def ttm(x: str, primary: str, a0: str) -> str:
        """coalesce(fiscal year at a 10-K, the quarterly path, FY0 within a year); records the FY0 fallback."""
        fallback.append(f"CASE WHEN NOT coalesce({ann_d} AND {a0} IS NOT NULL, false) AND ({primary}) IS NULL "
                        f"AND {fy0_ok} AND {a0} IS NOT NULL THEN '{x}' END")
        return (f"coalesce(CASE WHEN {ann_d} THEN {a0} END, {primary}, CASE WHEN {fy0_ok} THEN {a0} END)")
    for x in FLOW_Q:
        h[f"{x}_ttm"] = ttm(x, f"CASE WHEN {q4ok} THEN p.{x}_q0 + p.{x}_q1 + p.{x}_q2 + p.{x}_q3 END", f"ap.{x}_a0")
    eps_q: dict[str, str] = {}
    for x in EPS_Q:
        eps_q[x] = "CASE WHEN {q} THEN {s} END".format(
            q=q4ok, s=" + ".join(f"p.{x}_q{j} * exp(-coalesce(sl_q{j}, 0))" for j in range(4)))
        h[f"{x}_ttm"] = ttm(x, eps_q[x], f"(ap.{x}_a0 * exp(-coalesce(sl_a0, 0)))")
    for x in YTD_Q:
        ya = f"CASE p.fqtr_q0 WHEN 1 THEN p.{x}_q1 WHEN 2 THEN p.{x}_q2 WHEN 3 THEN p.{x}_q3 END"
        fa = "CASE p.fqtr_q0 WHEN 1 THEN p.fqtr_q1 WHEN 2 THEN p.fqtr_q2 WHEN 3 THEN p.fqtr_q3 END"
        primary = (f"CASE WHEN p.fqtr_q0 = 4 THEN p.{x}_q0 "
                   f"WHEN p.fqtr_q0 IN (1, 2, 3) AND {fa} = 4 AND p.fqtr_q4 = p.fqtr_q0 "
                   f"THEN p.{x}_q0 + {ya} - p.{x}_q4 END")
        h[f"{x}_ttm"] = ttm(x, primary, f"ap.{x}_a0")
    # EPS incl. discontinued operations: annual EPSFI at a 10-K, else the no quarterly total-EPS inference,
    # else FY0 EPSFI
    scaled = "CAST(NULL AS DOUBLE)"  # Total diluted EPS cannot be inferred from NI/IB.
    h["epsfi_ttm"] = ttm("epsfi", scaled, "(ap.epsfi_a0 * exp(-coalesce(sl_a0, 0)))")
    h["ttm_src"] = f"CASE WHEN {ann_d} THEN 'annual' WHEN {q4ok} THEN 'q4' END"
    h["ttm_fy0_items"] = f"nullif(concat_ws(',', {', '.join(fallback)}), '')"
    h["first_datadate"] = "p.first_datadate"
    h["hist_days"] = "date_diff('day', p.first_datadate, coalesce(ap.datadate_a0, p.datadate))"
    for x in BAL_Q:
        h[f"{x}_q0"] = f"p.{x}_q0"
        h[f"{x}_q4"] = f"p.{x}_q4"
    # txditc of the latest annual row at or before the balance date, within TXDITC_DAYS; unavailable history remains NULL
    for j in (0, 4):
        dd = f"p.datadate_q{j}"
        tx = (f"CASE WHEN ap.datadate_a0 <= {dd} AND date_diff('day', ap.datadate_a0, {dd}) <= {TXDITC_DAYS} "
              f"THEN ap.txditc_a0 WHEN ap.datadate_a1 <= {dd} "
              f"AND date_diff('day', ap.datadate_a1, {dd}) <= {TXDITC_DAYS} THEN ap.txditc_a1 END")
        h[f"wrds_be_q{j}"] = f"p.seq_q{j} + ({tx})"
        h[f"be_q{j}"] = f"p.seq_q{j} + ({tx}) - p.pstk_q{j}"
        h[f"icapt_q{j}"] = f"p.dltt_q{j} + p.pstk_q{j} + p.mib_q{j} + p.ceq_q{j}"
        h[f"invt_z{j}"] = f"p.invt_q{j}"
    h["ev_debt"] = "p.dltt_q0 + p.dlc_q0 + p.pstk_q0 + p.mib_q0 - p.che_q0"
    h["csho_b"] = "p.csho_q0 * exp(coalesce(sl_q0, 0))"   # lag-0 shares on the state's split basis
    for k, items in ANN_LAGS.items():
        for x in items:
            h[f"{x}_a{k}"] = f"ap.{x}_a{k}"
    h["ni_avg5"] = "(ap.ni_a0 + ap.ni_a1 + ap.ni_a2 + ap.ni_a3 + ap.ni_a4) / 5.0"
    h["eps_g3"] = ("CASE WHEN ap.epsfx_a0 > 0 AND ap.epsfx_a3 > 0 THEN pow((ap.epsfx_a0 * exp(-coalesce(sl_a0, 0))) "
                   "/ (ap.epsfx_a3 * exp(-coalesce(sl_a3, 0))), 1.0 / 3) - 1 END")
    return ",\n".join(f"({v}) AS {k}" for k, v in h.items())


def _avg_sql() -> dict[str, str]:
    def avg(a0: str, a4: str) -> str:
        return f"(({a0}) + ({a4})) / 2.0"
    out = {"avg_" + x: avg(f"{x}_q0", f"{x}_q4") for x in ("at", "be", "ceq", "seq", "icapt", "dltt", "rect", "ap")}
    out["avg_wrds_be"] = avg("wrds_be_q0", "wrds_be_q4")
    out["avg_invt"] = avg("invt_q0", "invt_q4")
    out["avg_invt_z"] = avg("invt_z0", "invt_z4")
    out["avg_totdebt"] = avg("dltt_q0 + dlc_q0", "dltt_q4 + dlc_q4")
    out["avg_capemp"] = avg("dltt_q0 + dlc_q0 + ceq_q0", "dltt_q4 + dlc_q4 + ceq_q4")
    out["avg_noa"] = avg("ppent_q0 + act_q0 - lct_q0", "ppent_q4 + act_q4 - lct_q4")
    out["avg_tea"] = avg("ppent_q0 + act_q0", "ppent_q4 + act_q4")
    out["avg_fallback"] = "at_q4 IS NULL AND at_q0 IS NOT NULL"
    pio = {"pio_roa0": _d("ib_a0", "at_a1"), "pio_roa1": _d("ib_a1", "at_a2"), "pio_cfo0": _d("oancf_a0", "at_a1"),
           "pio_lev0": _d("dltt_a0", "(at_a0 + at_a1) / 2.0"), "pio_lev1": _d("dltt_a1", "(at_a1 + at_a2) / 2.0"),
           "pio_liq0": _d("act_a0", "lct_a0"), "pio_liq1": _d("act_a1", "lct_a1"),
           "pio_gm0": _d("sale_a0 - cogs_a0", "sale_a0"), "pio_gm1": _d("sale_a1 - cogs_a1", "sale_a1"),
           "pio_turn0": _d("sale_a0", "at_a1"), "pio_turn1": _d("sale_a1", "at_a2")}
    out.update(pio)
    out["altman_x"] = (f"1.2 * {_d('act_a0 - lct_a0', 'at_a0')} + 1.4 * {_d('re_a0', 'at_a0')} + "
                       f"3.3 * {_d('oiadp_a0', 'at_a0')} + 1.0 * {_d('sale_a0', 'at_a0')}")
    chin = _d("ni_a0 - ni_a1", "abs(ni_a0) + abs(ni_a1)")
    out["ohlson_o"] = (f"CASE WHEN at_a0 > 0 THEN -1.32 - 0.407 * ln(at_a0 / 1e6) + 6.03 * {_d('lt_a0', 'at_a0')} "
                       f"- 1.43 * {_d('act_a0 - lct_a0', 'at_a0')} + 0.0757 * {_d('lct_a0', 'act_a0')} "
                       f"- 1.72 * {_ind('lt_a0 > at_a0')} - 2.37 * {_d('ni_a0', 'at_a0')} "
                       f"- 1.83 * {_d('oancf_a0', 'lt_a0')} + 0.285 * {_ind('ni_a0 < 0 AND ni_a1 < 0')} "
                       f"- 0.521 * {chin} END")
    return out


def _absent_sql() -> str:
    """Comma list of input items structurally absent for the issuer: never non-NULL on the eight quarterly rows j =
    0..7 (quarterly items) or on FY0 / FY-1 (annual-only items)."""
    parts = [f"CASE WHEN p.n_{x} = 0 THEN '{x}' END" for x in sorted({*FLOW_Q, *EPS_Q, *YTD_Q, *BAL_Q})]
    parts += [f"CASE WHEN coalesce(ap.na_{x}, 0) = 0 THEN '{x}' END" for x in ANNUAL_ONLY_ITEMS]
    return f"nullif(concat_ws(',', {', '.join(parts)}), '')"


def build_state_bucket(con, std: Path, where: str, dest: Path) -> int:
    """Issuer state vintages for the CIKs selected by ``where`` written to ``dest``."""
    _load(con, std, where)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE ev AS
        SELECT DISTINCT cik, available_at AS tau FROM (SELECT cik, available_at FROM q UNION ALL
                                                       SELECT cik, available_at FROM a)
        WHERE available_at >= TIMESTAMP '{EVENTS_FROM}'""")
    span = 3100
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE vis AS
        SELECT e.cik, e.tau, q.rid, q.datadate
        FROM ev e JOIN q ON q.cik = e.cik AND q.available_at <= e.tau
                        AND q.datadate > CAST(e.tau AS DATE) - INTERVAL {span} DAY
        QUALIFY row_number() OVER (PARTITION BY e.cik, e.tau, q.datadate
                                   ORDER BY q.available_at DESC, q.accession DESC) = 1""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE qlag AS
        WITH h AS (SELECT cik, tau, max(datadate) AS d0 FROM vis GROUP BY 1, 2),
        x AS (SELECT v.cik, v.tau, v.rid, date_diff('day', v.datadate, h.d0) AS dd FROM vis v JOIN h USING (cik, tau)),
        y AS (SELECT *, CAST(round(dd / {Q_DAYS}) AS INTEGER) AS j FROM x)
        SELECT cik, tau, rid, j FROM y WHERE j BETWEEN 0 AND {Q_LAGS} AND abs(dd - j * {Q_DAYS}) <= {Q_TOL}
        QUALIFY row_number() OVER (PARTITION BY cik, tau, j ORDER BY abs(dd - j * {Q_DAYS}), dd) = 1""")
    agg = ["max(CASE WHEN l.j = 0 THEN q.datadate END) AS datadate", "any_value(f.first_datadate) AS first_datadate",
           "max(CASE WHEN l.j = 0 THEN q.fyr END) AS fyr",
           "max(CASE WHEN l.j = 0 THEN q.accession END) AS accession0",
           "max(CASE WHEN l.j = 0 THEN q.form END) AS form0",
           "max(CASE WHEN l.j = 0 THEN q.fin_template END) AS fin_template",
           "max(CASE WHEN l.j = 0 THEN q.sic END) AS sic",
           "max(CASE WHEN l.j = 0 THEN q.curcd END) AS curcd",
           "bool_or(CASE WHEN l.j = 0 THEN q.fx_converted END) AS fx_converted"]
    for j in range(Q_LAGS + 1):
        agg.append(f"bool_or(l.j = {j}) AS has_q{j}")
        agg.append(f"max(CASE WHEN l.j = {j} THEN q.fqtr END) AS fqtr_q{j}")
    for j in (0, 4):
        agg.append(f"max(CASE WHEN l.j = {j} THEN q.datadate END) AS datadate_q{j}")
    for x, c in FLOW_Q.items():
        agg += [f'max(CASE WHEN l.j = {j} THEN q."{c}" END) AS {x}_q{j}' for j in range(4)]
    for x, c in EPS_Q.items():
        agg += [f'max(CASE WHEN l.j = {j} THEN q."{c}" END) AS {x}_q{j}' for j in range(4)]
    agg += [f"max(CASE WHEN l.j = {j} THEN q.filed END) AS filed_q{j}" for j in range(4)]
    for x, c in YTD_Q.items():
        agg += [f'max(CASE WHEN l.j = {j} THEN q."{c}" END) AS {x}_q{j}' for j in range(5)]
    for x, c in BAL_Q.items():
        agg += [f'max(CASE WHEN l.j = {j} THEN q."{c}" END) AS {x}_q{j}' for j in (0, 4)]
    for x, c in {**FLOW_Q, **EPS_Q, **YTD_Q, **BAL_Q}.items():
        agg.append(f'count(CASE WHEN l.j <= 7 THEN q."{c}" END) AS n_{x}')
    con.execute(f"CREATE OR REPLACE TEMP TABLE qp AS SELECT l.cik, l.tau, {', '.join(agg)} "
                f"FROM qlag l JOIN q USING (rid) "
                f"JOIN (SELECT cik, tau, min(datadate) AS first_datadate FROM vis GROUP BY 1, 2) f "
                f"ON f.cik = l.cik AND f.tau = l.tau GROUP BY 1, 2")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE visa AS
        SELECT e.cik, e.tau, a.arid, a.datadate
        FROM ev e JOIN a ON a.cik = e.cik AND a.available_at <= e.tau
                        AND a.datadate > CAST(e.tau AS DATE) - INTERVAL {span} DAY
        QUALIFY row_number() OVER (PARTITION BY e.cik, e.tau, a.datadate
                                   ORDER BY a.available_at DESC, a.accession DESC) = 1""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE alag AS
        WITH h AS (SELECT cik, tau, max(datadate) AS d0 FROM visa GROUP BY 1, 2),
        x AS (SELECT v.cik, v.tau, v.arid, date_diff('day', v.datadate, h.d0) AS dd FROM visa v JOIN h USING (cik, tau)),
        y AS (SELECT *, CAST(round(dd / {Y_DAYS}) AS INTEGER) AS k FROM x)
        SELECT cik, tau, arid, k FROM y WHERE k BETWEEN 0 AND {Y_LAGS} AND abs(dd - k * {Y_DAYS}) <= {Y_TOL}
        QUALIFY row_number() OVER (PARTITION BY cik, tau, k ORDER BY abs(dd - k * {Y_DAYS}), dd) = 1""")
    aagg = [f"max(CASE WHEN l.k = {k} THEN a.datadate END) AS datadate_a{k}" for k in range(Y_LAGS + 1)]
    aagg += ["max(CASE WHEN l.k = 0 THEN a.form END) AS form_a0", "max(CASE WHEN l.k = 0 THEN a.filed END) AS filed_a0",
             "max(CASE WHEN l.k = 3 THEN a.filed END) AS filed_a3"]
    for k, items in ANN_LAGS.items():
        aagg += [f'max(CASE WHEN l.k = {k} THEN a."{x}" END) AS {x}_a{k}' for x in items]
    aagg += [f'count(CASE WHEN l.k <= 1 THEN a."{x}" END) AS na_{x}' for x in ANNUAL_ONLY_ITEMS]
    con.execute(f"CREATE OR REPLACE TEMP TABLE ap AS SELECT l.cik, l.tau, {', '.join(aagg)} "
                f"FROM alag l JOIN a USING (arid) GROUP BY 1, 2")
    # split log factor between each per-share value's filing date and the state's date (issuer level)
    con.execute("""
        CREATE OR REPLACE TEMP TABLE sb AS
        SELECT p.cik, p.tau, CAST(p.tau AS DATE) AS basis_date, p.filed_q0, p.filed_q1, p.filed_q2, p.filed_q3,
               ap.filed_a0, ap.filed_a3
        FROM qp p LEFT JOIN ap USING (cik, tau)""")
    joins, cols = [], []
    for name in ("basis_date", "filed_q0", "filed_q1", "filed_q2", "filed_q3", "filed_a0", "filed_a3"):
        al = f"s_{name}"
        joins.append(f"ASOF LEFT JOIN spl {al} ON {al}.cik = sb.cik AND sb.{name} >= {al}.ex_date")
        cols.append(f"CASE WHEN sb.{name} IS NULL THEN NULL ELSE coalesce({al}.cum_lr, 0) END AS l_{name}")
    con.execute(f"CREATE OR REPLACE TEMP TABLE sl AS SELECT sb.cik, sb.tau, {', '.join(cols)} FROM sb {' '.join(joins)}")
    sl = ", ".join([f"l_basis_date - l_filed_q{j} AS sl_q{j}" for j in range(4)] +
                   ["l_basis_date - l_filed_a0 AS sl_a0", "l_basis_date - l_filed_a3 AS sl_a3"])
    avg = _avg_sql()
    pf = [r for r in RATIOS if not r["price"]]
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE st1 AS
        SELECT p.cik, p.tau AS available_at, CAST(p.tau AS DATE) AS basis_date, p.datadate, p.fqtr_q0 AS fqtr, p.fyr,
               p.accession0, p.form0, p.fin_template, p.sic, p.curcd, p.fx_converted,
               ap.datadate_a0 AS fy0_datadate, ap.form_a0 AS fy0_form, {_absent_sql()} AS absent_items,
               {_state_sql()}
        FROM qp p LEFT JOIN ap USING (cik, tau)
        LEFT JOIN (SELECT cik, tau, {sl} FROM sl) s USING (cik, tau)""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE st2 AS
        SELECT *, {', '.join(f'({v}) AS {k}' for k, v in avg.items())} FROM st1""")
    ben = ", ".join(f"({v}) AS {k}" for k, v in _BEN.items())
    pio = ", ".join(f"({v}) AS {k}" for k, v in _PIO.items())
    con.execute(f"CREATE OR REPLACE TEMP TABLE st3 AS SELECT *, {ben}, {pio} FROM st2")
    m = ("-4.84 + 0.920 * m_dsri + 0.528 * m_gmi + 0.404 * m_aqi + 0.892 * m_sgi + 0.115 * m_depi - 0.172 * m_sgai "
         "+ 4.679 * m_tata - 0.327 * m_lvgi")
    sel = ", ".join(f"({r['expr']}) AS {r['name']}" for r in pf)
    return C.copy_to_parquet(con, f"""
        SELECT *, {sel}, ({' + '.join(_PIO)}) AS piotroski_f, ({m}) AS beneish_m
        FROM st3 ORDER BY cik, available_at""", dest, row_group_size=32768)


# ---------------------------------------------------------------------------------------------------------------
# consumption: monthly materialisation and the daily as-of macro share one SELECT
# ---------------------------------------------------------------------------------------------------------------

MONTHLY_ID_COLS = ("session_date", "security_id", "cik", "link_tier", "linkprim", "prc", "shrout", "me_line", "me",
                   "me_lines", "eps_adj", "fund_available_at", "datadate", "fqtr", "form0", "ttm_src", "ttm_fy0_items",
                   "fy0_datadate", "first_datadate", "hist_days",
                   "fin_template", "sic", "curcd", "fx_converted", "absent_items", "be_q0", "ev", "staleness_days",
                   "fresh_q", "fresh_a")


def consume_sql(cells: str, state: str, spl: str) -> str:
    """SELECT of the published columns over ``cells`` (session_date, security_id, cik, link_tier, linkprim, prc,
    shrout, me_line, me, me_lines, cutoff): the state row with available_at < cutoff (ASOF), the issuer's splits between
    the state's date and the session, staleness flags, every ratio and composite."""
    annual_only = ", ".join(f"'{f}'" for f in ANNUAL_ONLY_FORMS)
    fresh = {"q": "fresh_q", "a": "fresh_a", "qa": "(fresh_q AND fresh_a)"}
    ratio_cols = []
    for r in list(RATIOS) + list(COMPOSITES):
        expr = r["expr"] if r["price"] else r["name"]
        ratio_cols.append(f"CASE WHEN {fresh[r['freq']]} THEN {expr} END AS {r['name']}")
    comp_cols = [f"CASE WHEN fresh_a THEN {c} END AS {c}" for c in COMPONENTS]
    undefined = []
    for r in list(RATIOS) + list(COMPOSITES):
        conds = ([f"({r['den']}) = 0"] if r.get("den") else []) + ([f"coalesce({r['undef']}, false)"]
                                                                   if r.get("undef") else [])
        if conds:
            undefined.append(f"CASE WHEN {fresh[r['freq']]} AND ({' OR '.join(conds)}) THEN '{r['name']}' END")
    return f"""
    WITH c AS ({cells}),
    s AS (SELECT c.*, st.* EXCLUDE (cik, available_at), st.available_at AS fund_available_at
          FROM c ASOF LEFT JOIN {state} st ON st.cik = c.cik AND c.cutoff > st.available_at),
    k AS (SELECT s.*, exp(-(CASE WHEN s.basis_date IS NULL THEN 0
                                 ELSE coalesce(sd.cum_lr, 0) - coalesce(sbas.cum_lr, 0) END)) AS eps_adj
          FROM s ASOF LEFT JOIN {spl} sd ON sd.cik = s.cik AND s.session_date >= sd.ex_date
                 ASOF LEFT JOIN {spl} sbas ON sbas.cik = s.cik AND s.basis_date >= sbas.ex_date),
    x AS (SELECT *, date_diff('day', datadate, session_date) AS staleness_days,
                 coalesce(date_diff('day', datadate, session_date)
                          <= CASE WHEN form0 IN ({annual_only}) THEN {STALE_ANNUAL_ONLY_DAYS} ELSE {STALE_DAYS} END,
                          false) AS fresh_q,
                 coalesce(date_diff('day', fy0_datadate, session_date) <= {FY_STALE_DAYS}, false) AS fresh_a,
                 me + ev_debt AS ev
          FROM k)
    SELECT {', '.join(MONTHLY_ID_COLS)},
           {', '.join(ratio_cols)},
           {', '.join(comp_cols)},
           nullif(concat_ws(',', {', '.join(undefined)}), '') AS ratio_undefined,
           'pe_exi,pe_inc,peg_trailing:missing_certified_security_class_diluted_EPS_alignment;divyield:missing_indicated_dividend_rate' AS ratio_missing_primitives,
           cutoff_clock AS available_at
    FROM x"""


def cells_sql(root: Path, sessions: str, cutoff: str, year_glob: str = "*") -> str:
    """Line cells for the sessions selected by the SQL ``sessions`` (one column ``session_date``): common lines
    (linkprim P/J) linked at the session (point-in-time tiers, the link known by 22:00 UTC of the session -- the
    panel rule) with a raw close, the line's shrout, the issuer's market equity, and ``cutoff`` (SQL over
    session_date: the fundamentals cutoff) / ``cutoff_clock`` (22:00 UTC of the session: the row's clock)."""
    lt = _q(root / "identity" / "link_table_v3.parquet")
    px = _q(root / "prices" / f"year={year_glob}" / "prices.parquet")
    sh = _q(root / "market" / "shares_daily" / f"year={year_glob}" / "shares_daily.parquet")
    fn = root / "security_master" / "finra_names.parquet"
    ltype = (f"SELECT security_id, mode(finra_type) AS line_type FROM read_parquet('{_q(fn)}') GROUP BY 1"
             if fn.exists() else "SELECT CAST(NULL AS BIGINT) AS security_id, CAST(NULL AS VARCHAR) AS line_type "
                                 "WHERE false")
    tiers = ", ".join(f"'{t}'" for t in PIT_LINK_TIERS)
    prim = ", ".join(f"'{p}'" for p in COMMON_LINKPRIM)
    bad = ", ".join(f"'{t}'" for t in NON_EQUITY_TYPES)
    return f"""
        WITH ms AS ({sessions}),
        ltype AS ({ltype}),
        lk AS (SELECT m.session_date, l.security_id, l.cik, l.link_tier, l.linkprim
               FROM ms m JOIN read_parquet('{lt}') l
                 ON m.session_date BETWEEN l.valid_from AND coalesce(l.valid_to, DATE '2100-01-01')
               LEFT JOIN ltype t ON t.security_id = l.security_id
               WHERE l.link_tier IN ({tiers}) AND l.linkprim IN ({prim}) AND l.cik IS NOT NULL
                 AND l.available_at <= CAST(m.session_date AS TIMESTAMP) + INTERVAL 22 HOUR
                 AND coalesce(t.line_type NOT IN ({bad}), true)),
        px AS (SELECT p.session_date, p.security_id, p.close AS prc FROM read_parquet('{px}', union_by_name = true) p
               WHERE p.session_date IN (SELECT session_date FROM ms) AND p.close > 0),
        sh AS (SELECT s.session_date, s.security_id, s.shrout FROM read_parquet('{sh}', union_by_name = true) s
               WHERE s.session_date IN (SELECT session_date FROM ms)),
        c0 AS (SELECT lk.*, px.prc, sh.shrout, sh.shrout * px.prc AS me_line
               FROM lk JOIN px USING (session_date, security_id) LEFT JOIN sh USING (session_date, security_id))
        SELECT c0.*,
               CASE WHEN count(*) OVER w = count(me_line) OVER w THEN sum(me_line) OVER w END AS me,
               count(*) OVER w AS me_lines,
               {cutoff} AS cutoff,
               CAST(session_date AS TIMESTAMP) + INTERVAL 22 HOUR AS cutoff_clock
        FROM c0 WINDOW w AS (PARTITION BY session_date, cik)"""


def month_end_sessions(con, root: Path) -> list[dt.date]:
    """Last calendar session of every month from MONTHLY_FROM, excluding the calendar's final (incomplete) month."""
    cal = _q(root / "calendar.parquet")
    rows = con.execute(f"""
        WITH c AS (SELECT session_date FROM read_parquet('{cal}')),
        m AS (SELECT max(session_date) AS s FROM c GROUP BY date_trunc('month', session_date))
        SELECT s FROM m WHERE s >= DATE '{MONTHLY_FROM}' AND s < (SELECT max(session_date) FROM c)
          AND date_trunc('month', s) < date_trunc('month', (SELECT max(session_date) FROM c)) ORDER BY 1""").fetchall()
    return [r[0] for r in rows]


def macros_sql(root: Path | None = None, stage: str = STAGE) -> str:
    """``ratios_asof(d)``: the daily as-of view for session ``d`` (a DATE): fundamentals with available_at < 22:00 UTC
    of the previous calendar session, the shares and raw close of d. ``<root>`` = the alpha-panel build root."""
    root = Path(root) if root else C.build_root()
    st = _q(root / stage / "fund_state.parquet")
    sp = _q(root / stage / "split_events.parquet")
    cal = _q(root / "calendar.parquet")
    sessions = "SELECT CAST(d AS DATE) AS session_date"
    cutoff = (f"(SELECT CAST(max(session_date) AS TIMESTAMP) + INTERVAL 22 HOUR FROM read_parquet('{cal}') "
              f"WHERE session_date < CAST(d AS DATE))")
    body = consume_sql(cells_sql(root, sessions, cutoff), f"read_parquet('{st}')", f"read_parquet('{sp}')")
    return f"CREATE OR REPLACE MACRO ratios_asof(d) AS TABLE {body};"


# ---------------------------------------------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------------------------------------------

def _manifest_sha(path: Path) -> str | None:
    return C.sha256_file(path) if path.exists() else None


def build(std_stage: str = DEFAULT_STD_STAGE, stage: str = STAGE, buckets: int = N_BUCKETS,
          years: tuple[int, ...] | None = None) -> dict[str, Any]:
    root = C.build_root()
    std = root / std_stage
    out = C.stage_dir(stage)
    tmp = root / "_tmp" / "ratios"
    tmp.mkdir(parents=True, exist_ok=True)
    t0 = time.perf_counter()
    receipt: dict[str, Any] = {"rule": RULE, "std_stage": std_stage}
    con = C.connect(memory=MEM, threads=2, db_file="ratios_build.duckdb")
    try:
        with C.timed(receipt, "splits"):
            build_splits(con, root)
            receipt["split_events"] = C.copy_to_parquet(con, "SELECT * FROM spl ORDER BY cik, ex_date",
                                                        out / "split_events.parquet")
        parts = []
        with C.timed(receipt, "state"):
            for b in range(buckets):
                dest = tmp / f"state-{b}.parquet"
                n = build_state_bucket(con, std, f"cik % {buckets} = {b}", dest)
                parts.append(dest)
                for t in ("q", "a", "ev", "vis", "qlag", "qp", "visa", "alag", "ap", "sb", "sl", "st1", "st2", "st3"):
                    con.execute(f"DROP TABLE IF EXISTS {t}")
                print(f"state bucket {b}: {n} rows", flush=True)
            glob = _q(tmp / "state-*.parquet")
            receipt["state_rows"] = C.copy_to_parquet(
                con, f"SELECT * FROM read_parquet('{glob}', union_by_name = true) ORDER BY cik, available_at",
                out / "fund_state.parquet", row_group_size=32768)
        sessions = month_end_sessions(con, root)
        by_year: dict[int, list[dt.date]] = {}
        for s in sessions:
            by_year.setdefault(s.year, []).append(s)
        st = f"read_parquet('{_q(out / 'fund_state.parquet')}')"
        sp = f"read_parquet('{_q(out / 'split_events.parquet')}')"
        receipt["monthly"] = {}
        with C.timed(receipt, "monthly"):
            for y, ss in sorted(by_year.items()):
                if years and y not in years:
                    continue
                lst = ", ".join(f"DATE '{s}'" for s in ss)
                sess = f"SELECT unnest([{lst}]) AS session_date"
                cells = cells_sql(root, sess, "CAST(session_date AS TIMESTAMP) + INTERVAL 22 HOUR", str(y))
                n = C.copy_to_parquet(con, consume_sql(cells, st, sp) + " ORDER BY session_date, security_id",
                                      out / "monthly" / f"year={y}" / "ratios_monthly.parquet", row_group_size=32768)
                receipt["monthly"][str(y)] = {"rows": n, "sessions": len(ss)}
                print(f"monthly {y}: {n} rows", flush=True)
        receipt["state_span"] = [str(v) for v in con.execute(
            f"SELECT min(available_at), max(available_at), count(DISTINCT cik) FROM {st}").fetchone()]
    finally:
        con.close()
        for p in (root / "_tmp" / "ratios_build.duckdb", root / "_tmp" / "ratios_build.duckdb.wal"):
            if p.exists():
                p.unlink()
    for p in tmp.glob("state-*.parquet"):
        p.unlink()
    receipt["seconds"] = round(time.perf_counter() - t0, 1)
    inputs = {std_stage: _manifest_sha(std / "manifest.json"),
              "market_shares": _manifest_sha(root / "market" / "market_shares_manifest.json"),
              "prices": _manifest_sha(root / "prices" / "manifest.json"),
              "corporate_actions": _manifest_sha(root / "corporate_actions" / "manifest.json"),
              "identity/link_table_v3_manifest.json": _manifest_sha(root / "identity" / "link_table_v3_manifest.json")}
    prior = C.read_json(out / "manifest.json") if (out / "manifest.json").exists() else {}
    C.write_stage_manifest(stage, SCHEMA, MODULES, {
        "rule": RULE, "receipt": receipt, "input_manifests_sha256": inputs,
        "ratios": [{k: r[k] for k in ("name", "family", "freq", "price", "inputs", "definition")}
                   for r in list(RATIOS) + list(COMPOSITES)],
        "components": list(COMPONENTS), "gate_status": prior.get("gate_status", {}),
        "published_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "macro_sql": macros_sql(root, stage)})
    return receipt


# ---------------------------------------------------------------------------------------------------------------
# validation: coverage, same-date Spearman vs the panel characteristics, p1/p99 vs literature envelopes
# ---------------------------------------------------------------------------------------------------------------

# p1 must be >= lo and p99 <= hi (pooled linked member month-end cells 2020-2025). Envelopes: definitional bounds
# (shares of a total, scores), else the outer range of Compustat-universe distributions reported in the literature
# (Fama-French BE/ME; Piotroski 2000 F in [0, 9]; Altman 1968; Ohlson 1980; Beneish 1999; WFRS ratio documentation).
# They catch unit, scale and sign errors, not distribution shape.
LIT_RANGES: dict[str, tuple[float, float, str]] = {
    "divyield_paid": (0, 0.3, "paid TTM dividend yield"),
    "capei": (-500, 1000, "P/E-type: near-zero earnings tails"), "bm": (-1, 10, "Fama-French BE/ME"),
    "pe_issuer_ni": (-1000, 1000, "issuer cap/NI"), "pe_issuer_ib": (-1000, 1000, "issuer cap/IB"),
    "evm": (-500, 500, "EV/EBITDA tails near zero EBITDA"), "pe_exi": (-1000, 1000, "P/E tails"),
    "pe_inc": (-1000, 1000, "P/E tails"), "ps": (0, 1000, "price/sales, pre-revenue tails"),
    "pcf": (-1000, 1000, "P/CF tails"), "dpr": (-10, 10, "payout ratio"), "peg_trailing": (-100, 100, "PEG"),
    "divyield": (0, 0.3, "dividend yield"), "ptb": (-100, 300, "price/book"),
    "npm": (-200, 2, "margins: pre-revenue tails"), "opmbd": (-200, 1.5, "margin"), "opmad": (-200, 1.5, "margin"),
    "gpm": (-50, 1.0, "gross margin <= 1 when cogs >= 0"), "ptpm": (-200, 2, "margin"), "cfm": (-200, 2, "margin"),
    "roa": (-2, 1, "oibdp/at"), "roe": (-10, 10, "roe"), "roce": (-10, 10, "roce"), "efftax": (-5, 5, "tax rate"),
    "aftret_eq": (-10, 10, "return"), "aftret_invcapx": (-10, 10, "return"), "aftret_equity": (-10, 10, "return"),
    "pretret_noa": (-20, 20, "return"), "pretret_earnat": (-5, 5, "return"), "gprof": (-1, 3, "Novy-Marx GP/A"),
    "equity_invcap": (-10, 2, "share of invested capital"), "debt_invcap": (-2, 10, "share of invested capital"),
    "totdebt_invcap": (-2, 10, "share of invested capital"), "capital_ratio": (-5, 10, "capitalization"),
    "int_debt": (0, 50, "interest / LTD (small-LTD tails)"), "int_totdebt": (0, 10, "interest / debt"),
    "cash_lt": (0, 100, "cash / liabilities"), "invt_act": (0, 1, "share of current assets"),
    "rect_act": (0, 1, "share of current assets"), "debt_at": (0, 1.5, "debt / assets"),
    "debt_ebitda": (-100, 200, "debt / EBITDA"), "short_debt": (0, 1, "share of total debt"),
    "curr_debt": (0, 1, "share of total liabilities"), "lt_debt": (0, 1, "share of total liabilities"),
    "profit_lct": (-50, 50, "coverage"), "ocf_lct": (-50, 50, "coverage"), "cash_debt": (-100, 1000, "coverage"),
    "fcf_ocf": (-100, 100, "fcf share"), "lt_ppent": (0, 10000, "liabilities / tangible assets"),
    "dltt_be": (-50, 50, "LTD / BE"), "de_ratio": (-100, 200, "liabilities / equity"),
    "debt_assets": (0, 5, "liabilities / assets"), "debt_capital": (-5, 5, "debt / capital"),
    "intcov": (-1000, 1000, "coverage"), "intcov_ratio": (-5000, 5000, "coverage"),
    "cash_ratio": (0, 100, "liquidity"), "quick_ratio": (0, 100, "liquidity"), "curr_ratio": (0, 100, "liquidity"),
    "cash_conversion": (-1000, 2000, "days"), "at_turn": (0, 5, "asset turnover"), "inv_turn": (0, 1000, "turnover"),
    "pay_turn": (-100, 1000, "turnover"), "rect_turn": (0, 1000, "turnover"), "sale_equity": (-100, 100, "turnover"),
    "sale_invcap": (-50, 100, "turnover"), "sale_nwc": (-1000, 1000, "turnover"), "accrual": (-2, 2, "Sloan accruals"),
    "rd_sale": (0, 100, "R&D intensity, pre-revenue tails"), "adv_sale": (0, 1, "advertising intensity"),
    "piotroski_f": (0, 9, "Piotroski (2000): integer 0..9"), "altman_z": (-50, 200, "Altman (1968)"),
    "ohlson_o": (-30, 30, "Ohlson (1980)"), "beneish_m": (-20, 20, "Beneish (1999)"),
}
# panel characteristic (char_registry formula on panel columns) vs the ratio-suite counterpart
SPEARMAN_PAIRS = {
    "bm": ("CASE WHEN p.me_company > 0 AND p.be > 0 THEN p.be / p.me_company END", "r.bm"),
    "ep": ("CASE WHEN p.me_company > 0 AND p.ni_ttm > 0 THEN p.ni_ttm / p.me_company END",
           "CASE WHEN r.pe_issuer_ni > 0 THEN 1.0 / r.pe_issuer_ni END"),
    "sp": ("CASE WHEN p.me_company > 0 AND p.sale_ttm > 0 THEN p.sale_ttm / p.me_company END",
           "CASE WHEN r.ps > 0 THEN 1.0 / r.ps END"),
    "gpa": ('CASE WHEN p."at" > 0 THEN p.gp_ttm / p."at" END', "r.gprof"),
}
TEMPLATE_STRUCTURAL_SHARE = 0.5
MASK_TEMPLATES = ("bank", "insurer", "reit", "utility")   # 'other' firms: issuer-level absence only


def structural_masks(con, std: Path) -> dict[str, list[str]]:
    """Explicit sector applicability; observed missingness never establishes structural absence."""
    return {"bank": ["cogs", "invt"], "insurer": ["cogs", "invt"], "reit": [], "utility": []}



def member_cells(con, root: Path, stage_out: Path) -> None:
    """Temp table ``cells``: monthly ratio rows on panel ``member_equity`` cells 2020-2025, plus ``mcells`` = every
    member_equity month-end cell (linked or not) with the ratio row when present."""
    panel = _q(root / "panel" / "year=*" / "*.parquet")
    mon = _q(stage_out / "monthly" / "year=*" / "ratios_monthly.parquet")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE mcells AS
        SELECT p.session_date, p.security_id, year(p.session_date) AS year, r.* EXCLUDE (session_date, security_id)
        FROM (SELECT DISTINCT session_date, security_id FROM read_parquet('{panel}')
              WHERE member_equity AND year(session_date) BETWEEN {GATE_YEARS[0]} AND {GATE_YEARS[-1]}
                AND session_date IN (SELECT DISTINCT session_date FROM read_parquet('{mon}'))) p
        LEFT JOIN read_parquet('{mon}') r USING (session_date, security_id)""")
    con.execute("CREATE OR REPLACE TEMP TABLE cells AS SELECT * FROM mcells WHERE cik IS NOT NULL")


def coverage_gate(con, masks: dict[str, list[str]], basis: str = "member_equity") -> dict[str, Any]:
    """Per ratio and year: share non-NULL of linked member cells whose inputs are non-structural (not in the
    explicit template structural set (observed absence remains missing)) and whose
    denominator is not 0; info: share of every linked member cell."""
    res: dict[str, Any] = {"basis": basis, "gate": COVERAGE_GATE, "masks": masks, "by_ratio": {}}
    def lst(v: list[str]) -> str:
        return "CAST([" + ", ".join(f"'{i}'" for i in v) + "] AS VARCHAR[])"
    mask_case = " ".join(f"WHEN fin_template = '{t}' THEN {lst(v)}" for t, v in masks.items())
    con.execute(f"""CREATE OR REPLACE TEMP TABLE cov AS
                    SELECT *, CASE {mask_case} ELSE {lst([])} END AS tmask,
                           string_split(coalesce(absent_items, ''), ',') AS absent,
                           string_split(coalesce(ratio_undefined, ''), ',') AS undef FROM cells""")
    fails = []
    for r in list(RATIOS) + list(COMPOSITES):
        n = r["name"]
        inputs = lst(list(r["inputs"]))
        elig = (f"NOT list_has_any(tmask, {inputs}) "
                f"AND NOT list_contains(undef, '{n}')")
        rows = con.execute(f"""
            SELECT year, count(*) AS cells, count({n}) AS nonnull,
                   count(*) FILTER (WHERE {elig}) AS eligible, count({n}) FILTER (WHERE {elig}) AS eligible_nonnull
            FROM cov GROUP BY 1 ORDER BY 1""").fetchall()
        by_year = {}
        for y, cells, nn, el, eln in rows:
            share = eln / el if el else None
            by_year[str(y)] = {"cells": cells, "info_share_all": round(nn / cells, 4) if cells else None,
                               "eligible": el, "share": None if share is None else round(share, 4)}
        mn = min((v["share"] for v in by_year.values() if v["share"] is not None), default=None)
        ok = mn is not None and mn >= COVERAGE_GATE and len(by_year) == len(GATE_YEARS)
        res["by_ratio"][n] = {"by_year": by_year, "min_share": mn, "pass": ok}
        if not ok:
            fails.append(n)
    res["fails"] = fails
    res["n_pass"] = len(res["by_ratio"]) - len(fails)
    res["pass"] = not fails
    extension_names = {"pe_issuer_ni", "pe_issuer_ib", "divyield_paid"}
    groups = {"primary": [r["name"] for r in RATIOS if r["name"] not in extension_names],
              "extensions": sorted(extension_names), "composites": [r["name"] for r in COMPOSITES]}
    res["by_role"] = {k: {"total": len(names), "n_pass": sum(res["by_ratio"][n]["pass"] for n in names),
                           "fails": [n for n in names if not res["by_ratio"][n]["pass"]]} for k,names in groups.items()}
    return res


def spearman_gate(con, root: Path) -> dict[str, Any]:
    """Same-date cross-sectional Spearman between the panel characteristics bm / ep / sp / gpa (char_registry formulas
    on the panel's columns) and the ratio-suite counterparts (bm, 1/pe_issuer_ni, 1/ps, gprof) on linked member month-end
    cells where both are defined; per month-end date, summarised per year (gate: every date >= 0.99)."""
    panel = _q(root / "panel" / "year=*" / "*.parquet")
    cols = ", ".join(f"{a} AS c_{k}, {b} AS r_{k}" for k, (a, b) in SPEARMAN_PAIRS.items())
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE sp AS
        SELECT r.session_date, r.year, {cols}
        FROM cells r JOIN read_parquet('{panel}') p USING (session_date, security_id)""")
    res: dict[str, Any] = {"gate": SPEARMAN_GATE, "by_char": {}}
    ok_all = True
    for k in SPEARMAN_PAIRS:
        rows = con.execute(f"""
            WITH z AS (SELECT session_date, year, rank() OVER (PARTITION BY session_date ORDER BY c_{k}) AS a,
                              rank() OVER (PARTITION BY session_date ORDER BY r_{k}) AS b
                       FROM sp WHERE c_{k} IS NOT NULL AND r_{k} IS NOT NULL),
            d AS (SELECT session_date, year, corr(a, b) AS rho, count(*) AS n FROM z GROUP BY 1, 2)
            SELECT year, min(rho), avg(rho), max(rho), avg(n), count(*) FROM d GROUP BY 1 ORDER BY 1""").fetchall()
        by_year = {str(y): {"min": round(a, 4), "mean": round(b, 4), "max": round(c, 4), "mean_n": round(n),
                            "dates": cnt} for y, a, b, c, n, cnt in rows}
        mn = min((v["min"] for v in by_year.values()), default=None)
        ok = mn is not None and mn >= SPEARMAN_GATE
        ok_all &= ok
        res["by_char"][k] = {"by_year": by_year, "min_date_rho": mn,
                             "min_year_mean": min((v["mean"] for v in by_year.values()), default=None), "pass": ok}
    res["pass"] = ok_all
    return res


def ranges_gate(con) -> dict[str, Any]:
    res: dict[str, Any] = {"by_ratio": {}}
    fails = []
    for r in list(RATIOS) + list(COMPOSITES):
        n = r["name"]
        lo, hi, src = LIT_RANGES[n]
        q = con.execute(f"SELECT quantile_cont({n}, [0.01, 0.5, 0.99]), count({n}) FROM cells").fetchone()
        if q[0] is None:
            res["by_ratio"][n] = {"n": 0, "pass": False}
            fails.append(n)
            continue
        p1, p50, p99 = q[0]
        ok = p1 >= lo and p99 <= hi
        res["by_ratio"][n] = {"p1": round(p1, 4), "p50": round(p50, 4), "p99": round(p99, 4), "n": q[1],
                              "range": [lo, hi], "source": src, "pass": ok}
        if not ok:
            fails.append(n)
    res["fails"] = fails
    res["pass"] = not fails
    return res


def universe_cells(con, root: Path, stage: str = STAGE, basis: str = "univ_all_common") -> bool:
    """Replace ``cells`` by univ_all_common cells when the universe stage has a manifest."""
    if not (root / "universe" / "manifest.json").exists():
        return False
    uni = _q(root / "universe" / "year=*" / "universe.parquet")
    mon = _q(root / stage / "monthly" / "year=*" / "ratios_monthly.parquet")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE cells AS
        SELECT u.session_date, u.security_id, year(u.session_date) AS year, r.* EXCLUDE (session_date, security_id)
        FROM (SELECT session_date, security_id FROM read_parquet('{uni}')
              WHERE {basis} AND year(session_date) BETWEEN {GATE_YEARS[0]} AND {GATE_YEARS[-1]}
                AND session_date IN (SELECT DISTINCT session_date FROM read_parquet('{mon}'))) u
        LEFT JOIN read_parquet('{mon}') r USING (session_date, security_id)""")
    return True


def validate(stage: str = STAGE, std_stage: str = DEFAULT_STD_STAGE,
             checks: tuple[str, ...] = ("coverage", "spearman", "ranges")) -> dict[str, Any]:
    root = C.build_root()
    out = root / stage
    con = C.connect(memory=MEM, threads=2)
    doc = C.read_json(out / "validation.json") if (out / "validation.json").exists() else {}
    try:
        member_cells(con, root, out)
        link = con.execute("SELECT year, count(*), count(cik), count(me) FROM mcells GROUP BY 1 ORDER BY 1").fetchall()
        doc["basis"] = {str(y): {"member_cells": n, "linked": l, "with_me": m} for y, n, l, m in link}
        if "coverage" in checks:
            masks = structural_masks(con, root / std_stage)
            doc["coverage"] = coverage_gate(con, masks)
        if "spearman" in checks:
            doc["spearman"] = spearman_gate(con, root)
        if "ranges" in checks:
            doc["ranges"] = ranges_gate(con)
        if "coverage" in checks:
            for basis in ("univ_all_common", "univ_r3000p"):
                if universe_cells(con, root, stage, basis):
                    doc["coverage_" + basis] = coverage_gate(con, masks, basis=basis)
            doc["universe_manifest_sha256"] = _manifest_sha(root / "universe" / "manifest.json")
    finally:
        con.close()
    doc["validated_at"] = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
    C.write_json_atomic(out / "validation.json", doc)
    publish_gate_status(stage)
    return doc


def publish_gate_status(stage: str = STAGE) -> dict[str, str]:
    out = C.build_root() / stage
    vpath, mpath = out / "validation.json", out / "manifest.json"
    if not vpath.exists() or not mpath.exists():
        return {}
    doc = C.read_json(vpath)
    status = {k: ("PASS" if doc[k].get("pass") else "FAIL") for k in ("coverage", "coverage_univ_all_common", "coverage_univ_r3000p", "spearman", "ranges", "handcheck")
              if isinstance(doc.get(k), dict) and "pass" in doc[k]}
    man = C.read_json(mpath)
    man["gate_status"] = status
    C.write_json_atomic(mpath, man)
    return status


# ---------------------------------------------------------------------------------------------------------------
# hand recompute from the 10-K (inline XBRL of the filing's primary document)
# ---------------------------------------------------------------------------------------------------------------

# item -> (kind: d = fiscal-year duration ending datadate, i = instant at datadate, p = instant at the prior fiscal-year
# end), concept chain (the fundamentals stage's documented chains, first present wins, non-dimensional contexts)
HAND_ITEMS: dict[str, tuple[str, tuple[str, ...]]] = {
    "sale": ("d", ("Revenues", "RevenueFromContractWithCustomerExcludingAssessedTax",
                   "RevenueFromContractWithCustomerIncludingAssessedTax", "SalesRevenueNet", "SalesRevenueGoodsNet",
                   "SalesRevenueServicesNet", "RevenuesNetOfInterestExpense")),
    "int_inc": ("d", ("InterestAndDividendIncomeOperating",)),
    "nonint_inc": ("d", ("NoninterestIncome",)),
    "cogs": ("d", ("CostOfGoodsAndServicesSold", "CostOfRevenue", "CostOfGoodsSold", "CostOfServices",
                   "CostOfGoodsAndServiceExcludingDepreciationDepletionAndAmortization",
                   "CostOfGoodsSoldExcludingDepreciationDepletionAndAmortization")),
    "oiadp": ("d", ("OperatingIncomeLoss",)),
    "ni": ("d", ("NetIncomeLoss", "ProfitLoss", "NetIncomeLossAvailableToCommonStockholdersBasic")),
    "ib_cont": ("d", ("IncomeLossFromContinuingOperations",
                      "IncomeLossFromContinuingOperationsIncludingPortionAttributableToNoncontrollingInterest")),
    "do": ("d", ("IncomeLossFromDiscontinuedOperationsNetOfTax",
                 "IncomeLossFromDiscontinuedOperationsNetOfTaxAttributableToReportingEntity",
                 "DiscontinuedOperationIncomeLossFromDiscontinuedOperationNetOfTax",
                 "IncomeLossFromDiscontinuedOperationsNetOfTaxIncludingPortionAttributableToNoncontrollingInterest")),
    "oancf": ("d", ("NetCashProvidedByUsedInOperatingActivities",
                    "NetCashProvidedByUsedInOperatingActivitiesContinuingOperations")),
    "capx": ("d", ("PaymentsToAcquirePropertyPlantAndEquipment", "PaymentsForCapitalImprovements",
                   "PaymentsToAcquireMachineryAndEquipment", "PaymentsToAcquireOtherPropertyPlantAndEquipment",
                   "PaymentsToAcquireProductiveAssets")),
    "dvc": ("d", ("PaymentsOfDividendsCommonStock", "PaymentsOfDividends")),
    "dp": ("d", ("DepreciationDepletionAndAmortization", "DepreciationDepletionAndAmortizationPropertyPlantAndEquipment")),
    "txt": ("d", ("IncomeTaxExpenseBenefit",)),
    "pi": ("d", ("IncomeLossFromContinuingOperationsBeforeIncomeTaxesExtraordinaryItemsNoncontrollingInterest", "IncomeLossFromContinuingOperationsBeforeIncomeTaxesMinorityInterestAndIncomeLossFromEquityMethodInvestments")),
    "xint": ("d", ("InterestExpense", "InterestAndDebtExpense")),
    "che": ("i", ("CashAndCashEquivalentsAtCarryingValue",)),
    "short_invest": ("i", ("ShortTermInvestments",)),
    "invt": ("i", ("InventoryNet",)),
    "rect": ("i", ("AccountsReceivableNetCurrent",)),
    "xrd": ("d", ("ResearchAndDevelopmentExpense", "ResearchAndDevelopmentExpenseExcludingAcquiredInProcessCost",
                  "ResearchAndDevelopmentExpenseSoftwareExcludingAcquiredInProcessCost")),
    "epsfi": ("d", ("EarningsPerShareDiluted", "EarningsPerShareBasicAndDiluted")),
    "epsfx_cont": ("d", ("IncomeLossFromContinuingOperationsPerDilutedShare",
                         "IncomeLossFromContinuingOperationsPerBasicAndDilutedShare")),
    "csho": ("i", ("CommonStockSharesOutstanding",)),
    "at": ("i", ("Assets",)), "at_p": ("p", ("Assets",)),
    "liab": ("i", ("Liabilities",)),
    "seq": ("i", ("StockholdersEquity",)), "seq_p": ("p", ("StockholdersEquity",)),
    "seq_nci": ("i", ("StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest",)),
    "mib": ("i", ("MinorityInterest",)),
    "act": ("i", ("AssetsCurrent",)), "lct": ("i", ("LiabilitiesCurrent",)),
    "pstk": ("i", ("PreferredStockRedemptionAmount", "PreferredStockLiquidationPreferenceValue", "PreferredStockValue",
                   "PreferredStockValueOutstanding")),
    "pstk_p": ("p", ("PreferredStockRedemptionAmount", "PreferredStockLiquidationPreferenceValue",
                     "PreferredStockValue", "PreferredStockValueOutstanding")),
    "txditc": ("i", ("DeferredIncomeTaxLiabilitiesNet", "DeferredTaxLiabilitiesNoncurrent")),
    "txditc_p": ("p", ("DeferredIncomeTaxLiabilitiesNet", "DeferredTaxLiabilitiesNoncurrent")),
}
HAND_N, HAND_SEED = 20, 20261003


def _hdiv(a, b):
    return None if a is None or b is None or b == 0 else a / b


def _hadd(a, b):
    return None if a is None or b is None else a+b


def _hsub(a, b):
    return None if a is None or b is None else a-b


def _havg(a, b):
    return None if a is None or b is None else (a + b) / 2


def hand_items(v: dict[str, Any]) -> dict[str, Any]:
    """Filing values -> the ratio inputs under the items' documented rules: sale (banks: interest and dividend income +
    noninterest income), ib (continuing operations, else ni - do, else ni), epsfx (continuing-operations diluted EPS,
    else total diluted EPS when no discontinued operations), lt (reported Liabilities only), pstk
    and txditc 0 when not reported, be = seq + txditc - pstk."""
    h = dict(v)
    if h.get("che") is not None and h.get("short_invest") is not None:
        h["che"] += h["short_invest"]
    if h.get("sale") is None and h.get("int_inc") is not None and h.get("nonint_inc") is not None:
        h["sale"] = h["int_inc"] + h["nonint_inc"]
    h["ib"] = h.get("ib_cont")
    if h["ib"] is None and h.get("ni") is not None:
        h["ib"] = h["ni"] - (h.get("do") or 0.0)
    h["epsfx"] = h.get("epsfx_cont")
    if h["epsfx"] is None and not h.get("do"):
        h["epsfx"] = h.get("epsfi")
    h["lt"] = h.get("liab")
    h["be"] = None if any(h.get(k) is None for k in ("seq", "txditc", "pstk")) else h["seq"] + h["txditc"] - h["pstk"]
    h["be_p"] = None if any(h.get(k) is None for k in ("seq_p", "txditc_p", "pstk_p")) else h["seq_p"] + h["txditc_p"] - h["pstk_p"]
    return h


HAND_RATIOS: dict[str, Any] = {
    "ps": lambda h, m: _hdiv(m["me"], h.get("sale")),
    "pcf": lambda h, m: _hdiv(m["me"], h.get("oancf")),
    "pe_issuer_ni": lambda h, m: _hdiv(m["me"], h.get("ni")),
    "pe_issuer_ib": lambda h, m: _hdiv(m["me"], h.get("ib")),
    "bm": lambda h, m: _hdiv(h.get("be"), m["me"]),
    "ptb": lambda h, m: _hdiv(m["me"], None if h.get("seq") is None or h.get("pstk") is None else h["seq"] - h["pstk"]),
    "divyield_paid": lambda h, m: _hdiv(h.get("dvc"), m["me"]),
    "dpr": lambda h, m: _hdiv(h.get("dvc"), h.get("ib")),
    "npm": lambda h, m: _hdiv(h.get("ni"), h.get("sale")),
    "opmad": lambda h, m: _hdiv(h.get("oiadp"), h.get("sale")),
    "gpm": lambda h, m: _hdiv(None if h.get("sale") is None or h.get("cogs") is None else h["sale"] - h["cogs"],
                              h.get("sale")),
    "roe": lambda h, m: _hdiv(h.get("ni"), _havg(None if h.get("seq") is None or h.get("txditc") is None else h["seq"]+h["txditc"], None if h.get("seq_p") is None or h.get("txditc_p") is None else h["seq_p"]+h["txditc_p"])),
    "at_turn": lambda h, m: _hdiv(h.get("sale"), _havg(h.get("at"), h.get("at_p"))),
    "accrual": lambda h, m: _hdiv(None if h.get("ib") is None or h.get("oancf") is None else h["ib"] - h["oancf"],
                                  _havg(h.get("at"), h.get("at_p"))),
    "debt_at": lambda h, m: _hdiv(h.get("lt"), h.get("at")),
    "curr_ratio": lambda h, m: _hdiv(h.get("act"), h.get("lct")),
    "de_ratio": lambda h, m: _hdiv(h.get("lt"), h.get("seq")),
    "fcf_ocf": lambda h, m: _hdiv(None if h.get("oancf") is None or h.get("capx") is None else h["oancf"] - h["capx"],
                                  h.get("oancf")),
    "rd_sale": lambda h, m: _hdiv(h.get("xrd"), h.get("sale")),
}

HAND_RATIOS.update({
    "gprof": lambda h,m: _hdiv(_hsub(h.get("sale"),h.get("cogs")),h.get("at")),
    "sale_equity": lambda h,m: _hdiv(h.get("sale"),h.get("seq")),
    "sale_nwc": lambda h,m: _hdiv(h.get("sale"),_hsub(h.get("act"),h.get("lct"))),
    "curr_debt": lambda h,m: _hdiv(h.get("lct"),h.get("lt")),
    "invt_act": lambda h,m: _hdiv(h.get("invt"),h.get("act")),
    "rect_act": lambda h,m: _hdiv(h.get("rect"),h.get("act")),
    "cash_lt": lambda h,m: _hdiv(h.get("che"),h.get("lt")),
    "cash_ratio": lambda h,m: _hdiv(h.get("che"),h.get("lct")),
    "quick_ratio": lambda h,m: _hdiv(_hsub(h.get("act"),h.get("invt")),h.get("lct")),
    "ocf_lct": lambda h,m: _hdiv(h.get("oancf"),h.get("lct")),
    "ptpm": lambda h,m: _hdiv(h.get("pi"),h.get("sale")),
    "efftax": lambda h,m: _hdiv(h.get("txt"),h.get("pi")),
    "cfm": lambda h,m: _hdiv(_hadd(h.get("ib"),h.get("dp")),h.get("sale")),
    "opmbd": lambda h,m: _hdiv(_hadd(h.get("oiadp"),h.get("dp")),h.get("sale")),
    "roa": lambda h,m: _hdiv(_hadd(h.get("oiadp"),h.get("dp")),_havg(h.get("at"),h.get("at_p"))),
    "intcov": lambda h,m: _hdiv(_hadd(h.get("ib"),h.get("xint")),h.get("xint")),
    "intcov_ratio": lambda h,m: _hdiv(_hadd(_hadd(h.get("ni"),h.get("txt")),h.get("xint")),h.get("xint")),
})


def _hand_values(facts: list[dict[str, Any]], datadate: dt.date) -> dict[str, Any]:
    """Filing values per ``HAND_ITEMS``: fiscal-year durations (300-380 days) ending at datadate, instants at datadate
    and at the prior fiscal-year end (the latest Assets instant 340-390 days before datadate)."""
    end = datadate.isoformat()
    prior_ends = sorted({f["end"] for f in facts if f["instant"] and not f["dim"] and f["concept"] == "Assets"
                         and 340 <= (datadate - dt.date.fromisoformat(f["end"])).days <= 390})
    prior = prior_ends[-1] if prior_ends else None
    out: dict[str, Any] = {}
    for item, (kind, concepts) in HAND_ITEMS.items():
        val, used = None, None
        for c in concepts:
            vals = []
            for f in facts:
                if f["concept"] != c or f["dim"]:
                    continue
                if kind == "i" and f["instant"] and f["end"] == end:
                    vals.append(f["value"])
                elif kind == "p" and f["instant"] and prior and f["end"] == prior:
                    vals.append(f["value"])
                elif kind == "d" and not f["instant"] and f["end"] == end and f["start"]:
                    if 300 <= (datadate - dt.date.fromisoformat(f["start"])).days <= 380:
                        vals.append(f["value"])
            if vals:
                val, used = vals[0], c
                break
        out[item] = val
        if used:
            out[item + "_concept"] = used
    # Match the standardized source contract: DEI cover share count at end-15d..end+120d,
    # then period-end balance shares. Never substitute a different share basis to force a match.
    cover = [f for f in facts if f["concept"] == "EntityCommonStockSharesOutstanding"
             and f["instant"] and not f["dim"]
             and -15 <= (dt.date.fromisoformat(f["end"]) - datadate).days <= 120]
    if cover:
        f = max(cover, key=lambda x: x["end"])
        out["csho"], out["csho_concept"] = f["value"], f["concept"]
    return out


def handcheck(stage: str = STAGE, n: int = HAND_N, seed: int = HAND_SEED) -> dict[str, Any]:
    """``n`` seeded linked member issuers (USD 10-K filers, issuer-primary line, no split between the 10-K and the
    date), two month-end dates each: the first month-end on which each of the issuer's earliest and latest 10-Ks
    2021-2025 is the state's lag-0 row (fiscal-year TTM path). For each, fetch the 10-K's primary document
    (``sec_http``, <= 5 req/s), read its inline XBRL facts and recompute ``HAND_RATIOS`` from the filing's values
    (``hand_items``) and the row's market inputs (me, prc, eps_adj); match = relative difference <= 1e-6."""
    from .. import sec_http
    from .compustat_std import parse_ixbrl

    root = C.build_root()
    out = root / stage
    mon = _q(out / "monthly" / "year=*" / "ratios_monthly.parquet")
    panel = _q(root / "panel" / "year=*" / "*.parquet")
    st = _q(out / "fund_state.parquet")
    names = ", ".join(f"r.{k}" for k in HAND_RATIOS)
    con = C.connect(memory="300MB", threads=1)
    rows = con.execute(f"""
        WITH r AS (SELECT r.session_date, r.security_id, r.cik, r.datadate, r.me, r.prc, r.eps_adj, {names},
                          s.accession0 AS accession_10k
                   FROM read_parquet('{mon}') r
                   JOIN read_parquet('{st}') s ON s.cik = r.cik AND s.available_at = r.fund_available_at
                   WHERE r.form0 = '10-K' AND r.ttm_src = 'annual' AND r.curcd = 'USD' AND r.fresh_q
                     AND r.linkprim = 'P' AND abs(r.eps_adj - 1) < 1e-12 AND r.me IS NOT NULL
                     AND year(r.session_date) BETWEEN 2021 AND 2025),
        m AS (SELECT r.* FROM r JOIN (SELECT DISTINCT session_date, security_id FROM read_parquet('{panel}')
                                      WHERE member_equity) p USING (session_date, security_id))
        SELECT * FROM m QUALIFY row_number() OVER (PARTITION BY cik, accession_10k ORDER BY session_date) = 1
        ORDER BY cik, session_date""").fetch_df()
    con.close()
    by_cik: dict[int, list[dict[str, Any]]] = {}
    for rec in rows.to_dict("records"):
        by_cik.setdefault(int(rec["cik"]), []).append(rec)
    eligible = sorted(c for c, v in by_cik.items() if len(v) >= 2)
    picks = random.Random(seed).sample(eligible, min(len(eligible), n * 3))
    lim = sec_http.default_sec_limiter()
    checks: list[dict[str, Any]] = []
    done = 0
    for cik in picks:
        if done >= n:
            break
        recs = by_cik[cik]
        issuer_rows, ok = [], True
        for rec in (recs[0], recs[-1]):
            acc = rec["accession_10k"]
            base = f"https://www.sec.gov/Archives/edgar/data/{cik}/{acc.replace('-', '')}"
            try:
                hdr = sec_http.sec_get(f"{base}/{acc}-index-headers.html", limiter=lim).decode("utf-8", "replace")
                mm = re.search(r"&lt;FILENAME&gt;([^\s&<]+)|<FILENAME>([^\s<]+)", hdr)
                primary = (mm.group(1) or mm.group(2)) if mm else None
                if primary is None:
                    idx = json.loads(sec_http.sec_get(f"{base}/index.json", limiter=lim))
                    primary = next(i["name"] for i in idx["directory"]["item"] if i["name"].endswith(".htm"))
                url = f"{base}/{primary}"
                raw = sec_http.sec_get(url, limiter=lim)
                retained = root.parents[1] / "raw" / "sec_ratios_handcheck"
                retained.mkdir(parents=True, exist_ok=True)
                digest = hashlib.sha256(raw).hexdigest()
                (retained / f"{acc}-{digest[:16]}.htm").write_bytes(raw)
                with (retained / "receipts.jsonl").open("a", encoding="utf-8") as f:
                    f.write(json.dumps({"url": url, "bytes": len(raw), "sha256": digest,
                                        "fetched_at": dt.datetime.now(dt.timezone.utc).isoformat(), "http_status": 200}) + "\n")
                html = raw.decode("utf-8", "replace")
            except Exception as exc:  # noqa: BLE001 - recorded; the issuer is replaced by the next pick
                checks.append({"cik": cik, "accession": acc, "error": str(exc)[:200]})
                ok = False
                break
            facts = parse_ixbrl(html)
            if not facts:
                checks.append({"cik": cik, "accession": acc, "skipped": "no inline XBRL facts"})
                ok = False
                break
            dd = rec["datadate"]
            dd = dd.date() if isinstance(dd, dt.datetime) else (dd.to_pydatetime().date() if hasattr(dd, "to_pydatetime") else dd)
            vals = _hand_values(facts, dd)
            h = hand_items(vals)
            m = {"me": float(rec["me"]), "prc": float(rec["prc"]), "eps_adj": float(rec["eps_adj"])}
            items = []
            for name, fn in HAND_RATIOS.items():
                hv = fn(h, m)
                ov = rec[name]
                ov = None if ov is None or (isinstance(ov, float) and math.isnan(ov)) else float(ov)
                if hv is None and ov is None:
                    verdict = "both_absent"
                elif hv is None or ov is None:
                    verdict = "absent_in_filing" if hv is None else "absent_in_ratios"
                else:
                    rel = abs(hv - ov) / max(abs(hv), 1e-12)
                    verdict = "match" if rel <= 1e-6 else "MISMATCH"
                items.append({"ratio": name, "ours": ov, "hand": hv, "verdict": verdict})
            issuer_rows.append({"cik": cik, "accession": acc, "datadate": str(dd),
                                "session_date": str(rec["session_date"])[:10], "primary": primary,
                                "filing": {k: v for k, v in h.items() if not k.endswith("_concept")},
                                "concepts": {k: v for k, v in vals.items() if k.endswith("_concept")},
                                "market": m, "ratios": items})
        if ok:
            checks.extend(issuer_rows)
            done += 1
    summary: dict[str, int] = {}
    for c in checks:
        for it in c.get("ratios", []):
            summary[it["verdict"]] = summary.get(it["verdict"], 0) + 1
    bad = sum(v for k, v in summary.items() if k not in ("match", "both_absent"))
    res = {"rule": handcheck.__doc__.strip(), "issuers": done, "rows": sum(1 for c in checks if "ratios" in c),
           "summary": summary, "checks": checks, "pass": done >= n and bad == 0}
    vpath = out / "validation.json"
    doc = C.read_json(vpath) if vpath.exists() else {}
    doc["handcheck"] = res
    C.write_json_atomic(vpath, doc)
    publish_gate_status(stage)
    return res


# ---------------------------------------------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--std-stage", default=DEFAULT_STD_STAGE)
    b.add_argument("--buckets", type=int, default=N_BUCKETS)
    b.add_argument("--years", type=int, nargs="*")
    v = sub.add_parser("validate")
    v.add_argument("--std-stage", default=DEFAULT_STD_STAGE)
    v.add_argument("--checks", nargs="*", default=["coverage", "spearman", "ranges"])
    h = sub.add_parser("handcheck")
    h.add_argument("--n", type=int, default=HAND_N)
    g = sub.add_parser("gate-status")
    sub.add_parser("macro")
    for p in (b, v, h, g):
        p.add_argument("--stage", default=STAGE)
    args = ap.parse_args(argv)
    if args.cmd == "build":
        res = build(args.std_stage, args.stage, args.buckets, tuple(args.years) if args.years else None)
    elif args.cmd == "validate":
        doc = validate(args.stage, args.std_stage, tuple(args.checks))
        res = {k: doc[k].get("pass") for k in ("coverage", "spearman", "ranges") if k in doc}
        res["coverage_fails"] = doc.get("coverage", {}).get("fails")
    elif args.cmd == "handcheck":
        r = handcheck(args.stage, args.n)
        res = {k: r[k] for k in ("issuers", "rows", "summary", "pass")}
    elif args.cmd == "gate-status":
        res = publish_gate_status(args.stage)
    else:
        print(macros_sql())
        return 0
    print(json.dumps(res, indent=2, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
