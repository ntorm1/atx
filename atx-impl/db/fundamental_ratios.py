"""S9a: derived point-in-time financial ratios (Compustat/FactSet calculated items).

This module materializes ``fundamental_ratios``, a long-format fact table with one
row per (security, period, ratio_code). Ratios are computed from two already-loaded,
already-bitemporal warehouse surfaces:

* ``fundamental_ttm_points``        -- trailing-twelve-month *flow* metrics (revenue,
                                       net income, operating cash flow, capex, ...).
                                       TTM is the quant-standard, fiscal-calendar-robust
                                       flow basis and avoids the Q-vs-YTD ambiguity of
                                       raw statement flows.
* ``fundamental_statement_points``  -- *instant* balance metrics at the period close
                                       (assets, liabilities, equity, shares).

Point-in-time discipline: every ratio carries its **own** ``available_at`` equal to the
maximum availability of the specific inputs it consumes, so an as-of query returns a
ratio only once every input it depends on was knowable. ``as_of_date`` is the period
close (TTM window end). No network is touched — ratios are a pure transform of warehouse
tables, so the dataset is fully deterministic and test-safe.

The ratio math lives in :func:`compute_ratio_rows`, a pure DataFrame->DataFrame transform
that is unit-tested independently of DuckDB. The :class:`FundamentalRatiosDataset` /
:func:`refresh_fundamental_ratios` pair pivots the inputs into the wide frame the transform
expects, then writes the result.

v1 stores only the latest-revision vintage of the inputs (one ratio per period). Storing a
ratio row per restatement vintage is a planned, non-breaking refinement (the bitemporal
columns are already present).
"""
from __future__ import annotations

import datetime as dt
import hashlib
import math
from dataclasses import dataclass
from typing import Any, Callable

import pandas as pd

from .asof import fundamental_ratios_asof  # noqa: F401  (re-exported for callers)
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, json_dumps, quality_check


SOURCE_NAME = "Derived point-in-time financial ratios"
DEFAULT_SOURCE = "derived_fundamental_ratios_v1"
DEFAULT_BASIS = "ttm"

# Raw wide-frame input key -> canonical_metric in the source tables.
TTM_INPUTS = {
    "rev": "revenue",
    "ni": "net_income",
    "oi": "operating_income",
    "ocf": "operating_cash_flow",
    "capex": "capital_expenditures",
    "div": "dividends_paid",
    "repurch": "share_repurchases",
}
BALANCE_INPUTS = {
    "assets": "assets",
    "liabilities": "liabilities",
    "equity": "stockholders_equity",
    "shares": "shares_outstanding",
}

RATIO_COLUMNS = [
    "ratio_id", "source", "upstream_source", "security_id", "symbol", "cik",
    "ratio_code", "ratio_category", "ratio_kind", "basis", "unit",
    "period_start", "period_end", "fiscal_year", "fiscal_period",
    "value", "numerator_code", "numerator_value", "denominator_code",
    "denominator_value", "is_meaningful", "is_latest_revision",
    "as_of_date", "available_at", "input_codes_json", "run_id",
]


@dataclass(frozen=True)
class RatioDef:
    code: str
    category: str          # profitability | leverage | efficiency | liquidity | cash_flow | payout | per_share | growth | health
    kind: str              # ratio | level | difference | per_share | growth | score
    unit: str              # ratio | currency | currency_per_share | score
    numerator_code: str
    denominator_code: str
    inputs: tuple[str, ...]            # raw input keys that gate availability
    operands: Callable[[dict], tuple]  # (numerator_value, denominator_value)
    require_positive_denominator: bool = False
    # For kind='score': a weighted-composite value function over the wide row
    # (bypasses operands; numerator/denominator are not stored).
    composite: Callable[[dict], float | None] | None = None


def _abs(x: float) -> float:
    return abs(float(x))


def _z(x: Any) -> float:
    """Coalesce a missing/NaN optional input to 0.0 (e.g. unreported inventory)."""
    try:
        return 0.0 if pd.isna(x) else float(x)
    except (TypeError, ValueError):
        return 0.0 if x is None else float(x)


def _altman_z_double_prime(r: dict) -> float | None:
    """Altman Z''-score (1995 emerging-markets / non-manufacturer variant).

    Z'' = 6.56·(WC/TA) + 3.26·(RE/TA) + 6.72·(EBIT/TA) + 1.05·(book equity / TL).
    Uses book equity (not market) so it is computable without price data. Operating
    income is the EBIT proxy. Distress zones: <1.1 distress, 1.1–2.6 grey, >2.6 safe.
    """
    ta = float(r["assets"])
    tl = float(r["liabilities"])
    if ta <= 0 or tl <= 0:
        return None
    wc = float(r["current_assets"]) - float(r["current_liabilities"])
    return (
        6.56 * wc / ta
        + 3.26 * float(r["retained_earnings"]) / ta
        + 6.72 * float(r["oi"]) / ta
        + 1.05 * float(r["equity"]) / tl
    )


def _piotroski_f_score(r: dict) -> float | None:
    """Piotroski (2000) F-score: nine binary fundamental-strength signals summed 0-9.

    Profitability (4): ROA>0, CFO>0, ΔROA>0, accruals (CFO>net income).
    Funding / liquidity (3): ΔLong-term leverage<0, ΔCurrent ratio>0, no share issuance.
    Operating efficiency (2): ΔGross margin>0, ΔAsset turnover>0.

    Computed year-over-year, so it only emits where the ~1y-prior value of every input
    is paired in (and, like all flow ratios sourced from annual XBRL, where the prior row
    also reported the annual flows). Higher is stronger: 8-9 is the classic long screen,
    0-1 the short. Returns None when a balance/flow denominator is non-positive (the deltas
    would be sign-ambiguous), so the row is skipped rather than emitting a garbage score.
    """
    ta, ta0 = float(r["assets"]), float(r["assets_prior"])
    cl, cl0 = float(r["current_liabilities"]), float(r["current_liabilities_prior"])
    rev, rev0 = float(r["rev"]), float(r["rev_prior"])
    if ta <= 0 or ta0 <= 0 or cl <= 0 or cl0 <= 0 or rev <= 0 or rev0 <= 0:
        return None
    ni, ni0 = float(r["ni"]), float(r["ni_prior"])
    cfo = float(r["ocf"])
    roa, roa0 = ni / ta, ni0 / ta0
    lev, lev0 = float(r["long_term_debt"]) / ta, float(r["long_term_debt_prior"]) / ta0
    cur, cur0 = float(r["current_assets"]) / cl, float(r["current_assets_prior"]) / cl0
    gm, gm0 = float(r["gross_profit"]) / rev, float(r["gross_profit_prior"]) / rev0
    turn, turn0 = rev / ta, rev0 / ta0
    signals = (
        roa > 0,            # 1. positive return on assets
        cfo > 0,            # 2. positive operating cash flow
        roa > roa0,         # 3. rising return on assets
        cfo > ni,           # 4. accruals: cash flow exceeds reported earnings
        lev < lev0,         # 5. falling long-term leverage
        cur > cur0,         # 6. rising current ratio
        float(r["common_shares_outstanding"]) <= float(r["common_shares_outstanding_prior"]),  # 7. no net share issuance
        gm > gm0,           # 8. rising gross margin
        turn > turn0,       # 9. rising asset turnover
    )
    return float(sum(1 for s in signals if s))


def _ohlson_o_score(r: dict) -> float | None:
    """Ohlson (1980) O-score: a nine-term logit of bankruptcy probability.

    O = -1.32 - 0.407·SIZE + 6.03·TLTA - 1.43·WCTA + 0.0757·CLCA
        - 1.72·OENEG - 2.37·NITA - 1.83·FUTL + 0.285·INTWO - 0.521·CHIN
    where SIZE=ln(total assets), TLTA=liabilities/assets, WCTA=working capital/assets,
    CLCA=current liabilities/current assets, OENEG=1 if liabilities>assets (book
    insolvency), NITA=net income/assets, FUTL=operating cash flow/liabilities (funds-
    from-operations proxy), INTWO=1 if net loss in both this and the prior year, and
    CHIN=(NI_t-NI_{t-1})/(|NI_t|+|NI_{t-1}|). Higher O => higher modeled default
    probability (P = 1/(1+e^-O); the paper's cutoff is O>0.5 i.e. ~0.038 on the logit).

    SIZE's original GNP-price-level deflator is omitted (assets in USD): a constant
    additive shift that preserves the cross-sectional ranking the score is used for.
    Returns None when assets, liabilities, or current assets are non-positive (the
    ratios/log would be undefined), so the row is skipped rather than emitting garbage.
    """
    ta = float(r["assets"])
    tl = float(r["liabilities"])
    ca = float(r["current_assets"])
    cl = float(r["current_liabilities"])
    if ta <= 0 or tl <= 0 or ca <= 0:
        return None
    ni = float(r["ni"])
    ni0 = float(r["ni_prior"])
    ocf = float(r["ocf"])
    chin_den = abs(ni) + abs(ni0)
    return (
        -1.32
        - 0.407 * math.log(ta)
        + 6.03 * (tl / ta)
        - 1.43 * ((ca - cl) / ta)
        + 0.0757 * (cl / ca)
        - 1.72 * (1.0 if tl > ta else 0.0)
        - 2.37 * (ni / ta)
        - 1.83 * (ocf / tl)
        + 0.285 * (1.0 if (ni < 0 and ni0 < 0) else 0.0)
        - 0.521 * ((ni - ni0) / chin_den if chin_den != 0 else 0.0)
    )


def _beneish_m_score(r: dict) -> float | None:
    """Beneish (1999) eight-variable M-score for earnings-manipulation risk.

    M = -4.84 + 0.920*DSRI + 0.528*GMI + 0.404*AQI + 0.892*SGI
        + 0.115*DEPI - 0.172*SGAI + 4.679*TATA - 0.327*LVGI.

    Sales, receivables, COGS, current assets, PP&E, depreciation, SG&A, liabilities,
    net income, and operating cash flow come from the PIT warehouse inputs; prior-year
    terms are paired by period_end in _attach_prior_year. Higher scores indicate more
    manipulation-risk pressure; the classic screen flags scores above about -2.22.
    """
    sales, sales0 = float(r["rev"]), float(r["rev_prior"])
    assets, assets0 = float(r["assets"]), float(r["assets_prior"])
    ppe, ppe0 = float(r["property_plant_equipment_net"]), float(r["property_plant_equipment_net_prior"])
    dep, dep0 = float(r["depreciation_amortization"]), float(r["depreciation_amortization_prior"])
    sga, sga0 = (
        float(r["selling_general_and_administrative_expense"]),
        float(r["selling_general_and_administrative_expense_prior"]),
    )
    liabilities, liabilities0 = float(r["liabilities"]), float(r["liabilities_prior"])
    if (
        sales <= 0 or sales0 <= 0 or assets <= 0 or assets0 <= 0
        or ppe < 0 or ppe0 < 0 or dep <= 0 or dep0 <= 0
        or sga0 <= 0 or liabilities <= 0 or liabilities0 <= 0
    ):
        return None

    receivables, receivables0 = float(r["accounts_receivable"]), float(r["accounts_receivable_prior"])
    cogs, cogs0 = float(r["cost_of_revenue"]), float(r["cost_of_revenue_prior"])
    gross_margin = (sales - cogs) / sales
    gross_margin0 = (sales0 - cogs0) / sales0
    asset_quality = 1.0 - ((float(r["current_assets"]) + ppe) / assets)
    asset_quality0 = 1.0 - ((float(r["current_assets_prior"]) + ppe0) / assets0)
    dep_rate = dep / (dep + ppe)
    dep_rate0 = dep0 / (dep0 + ppe0)
    if (
        receivables0 <= 0 or gross_margin <= 0 or gross_margin0 <= 0
        or asset_quality0 == 0 or dep_rate <= 0
    ):
        return None

    dsri = (receivables / sales) / (receivables0 / sales0)
    gmi = gross_margin0 / gross_margin
    aqi = asset_quality / asset_quality0
    sgi = sales / sales0
    depi = dep_rate0 / dep_rate
    sgai = (sga / sales) / (sga0 / sales0)
    tata = (float(r["ni"]) - float(r["ocf"])) / assets
    lvgi = (liabilities / assets) / (liabilities0 / assets0)
    return (
        -4.84
        + 0.920 * dsri
        + 0.528 * gmi
        + 0.404 * aqi
        + 0.892 * sgi
        + 0.115 * depi
        - 0.172 * sgai
        + 4.679 * tata
        - 0.327 * lvgi
    )


RATIO_DEFS: tuple[RatioDef, ...] = (
    # --- profitability -----------------------------------------------------
    RatioDef("net_profit_margin", "profitability", "ratio", "ratio",
             "net_income", "revenue", ("ni", "rev"),
             lambda r: (r["ni"], r["rev"])),
    RatioDef("operating_margin", "profitability", "ratio", "ratio",
             "operating_income", "revenue", ("oi", "rev"),
             lambda r: (r["oi"], r["rev"])),
    RatioDef("return_on_assets", "profitability", "ratio", "ratio",
             "net_income", "assets", ("ni", "assets"),
             lambda r: (r["ni"], r["assets"])),
    RatioDef("return_on_equity", "profitability", "ratio", "ratio",
             "net_income", "stockholders_equity", ("ni", "equity"),
             lambda r: (r["ni"], r["equity"]), require_positive_denominator=True),
    # --- leverage ----------------------------------------------------------
    RatioDef("assets_to_equity", "leverage", "ratio", "ratio",
             "assets", "stockholders_equity", ("assets", "equity"),
             lambda r: (r["assets"], r["equity"]), require_positive_denominator=True),
    RatioDef("liabilities_to_assets", "leverage", "ratio", "ratio",
             "liabilities", "assets", ("liabilities", "assets"),
             lambda r: (r["liabilities"], r["assets"])),
    RatioDef("liabilities_to_equity", "leverage", "ratio", "ratio",
             "liabilities", "stockholders_equity", ("liabilities", "equity"),
             lambda r: (r["liabilities"], r["equity"]), require_positive_denominator=True),
    # --- cash flow ---------------------------------------------------------
    RatioDef("free_cash_flow", "cash_flow", "level", "currency",
             "operating_cash_flow", "capital_expenditures", ("ocf", "capex"),
             lambda r: (r["ocf"], r["capex"])),
    RatioDef("fcf_margin", "cash_flow", "ratio", "ratio",
             "free_cash_flow", "revenue", ("ocf", "capex", "rev"),
             lambda r: (r["ocf"] + r["capex"], r["rev"])),
    RatioDef("operating_cash_flow_to_net_income", "cash_flow", "ratio", "ratio",
             "operating_cash_flow", "net_income", ("ocf", "ni"),
             lambda r: (r["ocf"], r["ni"]), require_positive_denominator=True),
    RatioDef("capex_to_revenue", "cash_flow", "ratio", "ratio",
             "abs_capital_expenditures", "revenue", ("capex", "rev"),
             lambda r: (_abs(r["capex"]), r["rev"])),
    # --- payout ------------------------------------------------------------
    RatioDef("dividend_payout_ratio", "payout", "ratio", "ratio",
             "abs_dividends_paid", "net_income", ("div", "ni"),
             lambda r: (_abs(r["div"]), r["ni"]), require_positive_denominator=True),
    RatioDef("buyback_to_net_income", "payout", "ratio", "ratio",
             "abs_share_repurchases", "net_income", ("repurch", "ni"),
             lambda r: (_abs(r["repurch"]), r["ni"]), require_positive_denominator=True),
    RatioDef("total_payout_ratio", "payout", "ratio", "ratio",
             "abs_dividends_plus_repurchases", "net_income", ("div", "repurch", "ni"),
             lambda r: (_abs(r["div"]) + _abs(r["repurch"]), r["ni"]),
             require_positive_denominator=True),
    # --- per share ---------------------------------------------------------
    RatioDef("book_value_per_share", "per_share", "per_share", "currency_per_share",
             "stockholders_equity", "shares_outstanding", ("equity", "shares"),
             lambda r: (r["equity"], r["shares"])),
    # --- efficiency / activity (S9b) --------------------------------------
    RatioDef("asset_turnover", "efficiency", "ratio", "ratio",
             "revenue", "assets", ("rev", "assets"),
             lambda r: (r["rev"], r["assets"])),
    RatioDef("equity_turnover", "efficiency", "ratio", "ratio",
             "revenue", "stockholders_equity", ("rev", "equity"),
             lambda r: (r["rev"], r["equity"]), require_positive_denominator=True),
    # --- asset-structure / activity (S10g; consolidated inline-XBRL instants) ---
    RatioDef("fixed_asset_turnover", "efficiency", "ratio", "ratio",
             "revenue", "property_plant_equipment_net", ("rev", "property_plant_equipment_net"),
             lambda r: (r["rev"], r["property_plant_equipment_net"]), require_positive_denominator=True),
    RatioDef("receivables_turnover", "efficiency", "ratio", "ratio",
             "revenue", "accounts_receivable", ("rev", "accounts_receivable"),
             lambda r: (r["rev"], r["accounts_receivable"]), require_positive_denominator=True),
    RatioDef("ppe_to_assets", "efficiency", "ratio", "ratio",
             "property_plant_equipment_net", "assets", ("property_plant_equipment_net", "assets"),
             lambda r: (r["property_plant_equipment_net"], r["assets"]), require_positive_denominator=True),
    # --- additional profitability / cash-flow coverage (S9b) --------------
    RatioDef("operating_return_on_assets", "profitability", "ratio", "ratio",
             "operating_income", "assets", ("oi", "assets"),
             lambda r: (r["oi"], r["assets"])),
    RatioDef("operating_cash_flow_margin", "cash_flow", "ratio", "ratio",
             "operating_cash_flow", "revenue", ("ocf", "rev"),
             lambda r: (r["ocf"], r["rev"])),
    RatioDef("operating_cash_flow_to_assets", "cash_flow", "ratio", "ratio",
             "operating_cash_flow", "assets", ("ocf", "assets"),
             lambda r: (r["ocf"], r["assets"])),
    RatioDef("operating_cash_flow_to_liabilities", "cash_flow", "ratio", "ratio",
             "operating_cash_flow", "liabilities", ("ocf", "liabilities"),
             lambda r: (r["ocf"], r["liabilities"]), require_positive_denominator=True),
    RatioDef("capex_to_operating_cash_flow", "cash_flow", "ratio", "ratio",
             "abs_capital_expenditures", "operating_cash_flow", ("capex", "ocf"),
             lambda r: (_abs(r["capex"]), r["ocf"]), require_positive_denominator=True),
    # --- payout / reinvestment (S9b) --------------------------------------
    RatioDef("retention_ratio", "payout", "ratio", "ratio",
             "net_income_minus_dividends", "net_income", ("ni", "div"),
             lambda r: (r["ni"] - _abs(r["div"]), r["ni"]), require_positive_denominator=True),
    # --- year-over-year growth (S9c) --------------------------------------
    # kind='growth': value = (current - prior) / abs(prior); the prior-year value is
    # paired into the wide frame by _attach_prior_year. is_meaningful is false off a
    # non-positive base (sign of a % change is ambiguous when the prior is negative).
    RatioDef("revenue_growth_yoy", "growth", "growth", "ratio",
             "revenue", "revenue_prior_year", ("rev", "rev_prior"),
             lambda r: (r["rev"], r["rev_prior"]), require_positive_denominator=True),
    RatioDef("net_income_growth_yoy", "growth", "growth", "ratio",
             "net_income", "net_income_prior_year", ("ni", "ni_prior"),
             lambda r: (r["ni"], r["ni_prior"]), require_positive_denominator=True),
    RatioDef("operating_income_growth_yoy", "growth", "growth", "ratio",
             "operating_income", "operating_income_prior_year", ("oi", "oi_prior"),
             lambda r: (r["oi"], r["oi_prior"]), require_positive_denominator=True),
    RatioDef("operating_cash_flow_growth_yoy", "growth", "growth", "ratio",
             "operating_cash_flow", "operating_cash_flow_prior_year", ("ocf", "ocf_prior"),
             lambda r: (r["ocf"], r["ocf_prior"]), require_positive_denominator=True),
    RatioDef("assets_growth_yoy", "growth", "growth", "ratio",
             "assets", "assets_prior_year", ("assets", "assets_prior"),
             lambda r: (r["assets"], r["assets_prior"]), require_positive_denominator=True),
    RatioDef("equity_growth_yoy", "growth", "growth", "ratio",
             "stockholders_equity", "stockholders_equity_prior_year", ("equity", "equity_prior"),
             lambda r: (r["equity"], r["equity_prior"]), require_positive_denominator=True),
    # --- average-balance returns (S9d) ------------------------------------
    # Denominator is the mean of the ending and prior-year balance, matching the
    # Compustat/FactSet convention for return-on-capital ratios.
    RatioDef("average_return_on_assets", "profitability", "ratio", "ratio",
             "net_income", "average_assets", ("ni", "assets", "assets_prior"),
             lambda r: (r["ni"], (r["assets"] + r["assets_prior"]) / 2), require_positive_denominator=True),
    RatioDef("average_return_on_equity", "profitability", "ratio", "ratio",
             "net_income", "average_stockholders_equity", ("ni", "equity", "equity_prior"),
             lambda r: (r["ni"], (r["equity"] + r["equity_prior"]) / 2), require_positive_denominator=True),
    RatioDef("operating_cash_flow_to_average_assets", "cash_flow", "ratio", "ratio",
             "operating_cash_flow", "average_assets", ("ocf", "assets", "assets_prior"),
             lambda r: (r["ocf"], (r["assets"] + r["assets_prior"]) / 2), require_positive_denominator=True),
    # --- liquidity / working capital (S10a; consolidated inline-XBRL instants) ---
    RatioDef("current_ratio", "liquidity", "ratio", "ratio",
             "current_assets", "current_liabilities", ("current_assets", "current_liabilities"),
             lambda r: (r["current_assets"], r["current_liabilities"]), require_positive_denominator=True),
    RatioDef("quick_ratio", "liquidity", "ratio", "ratio",
             "current_assets_minus_inventory", "current_liabilities", ("current_assets", "current_liabilities"),
             lambda r: (r["current_assets"] - _z(r.get("inventory")), r["current_liabilities"]),
             require_positive_denominator=True),
    RatioDef("cash_ratio", "liquidity", "ratio", "ratio",
             "cash_and_equivalents", "current_liabilities", ("cash_and_equivalents", "current_liabilities"),
             lambda r: (r["cash_and_equivalents"], r["current_liabilities"]), require_positive_denominator=True),
    RatioDef("working_capital", "liquidity", "difference", "currency",
             "current_assets", "current_liabilities", ("current_assets", "current_liabilities"),
             lambda r: (r["current_assets"], r["current_liabilities"])),
    RatioDef("working_capital_to_assets", "liquidity", "ratio", "ratio",
             "working_capital", "assets", ("current_assets", "current_liabilities", "assets"),
             lambda r: (r["current_assets"] - r["current_liabilities"], r["assets"]),
             require_positive_denominator=True),
    # --- debt / solvency (S10b; long-term debt from consolidated XBRL instants) ---
    RatioDef("long_term_debt_to_equity", "leverage", "ratio", "ratio",
             "long_term_debt", "stockholders_equity", ("long_term_debt", "equity"),
             lambda r: (r["long_term_debt"], r["equity"]), require_positive_denominator=True),
    RatioDef("long_term_debt_to_assets", "leverage", "ratio", "ratio",
             "long_term_debt", "assets", ("long_term_debt", "assets"),
             lambda r: (r["long_term_debt"], r["assets"]), require_positive_denominator=True),
    RatioDef("net_debt", "leverage", "difference", "currency",
             "long_term_debt", "cash_and_equivalents", ("long_term_debt", "cash_and_equivalents"),
             lambda r: (r["long_term_debt"], r["cash_and_equivalents"])),
    RatioDef("net_debt_to_assets", "leverage", "ratio", "ratio",
             "net_debt", "assets", ("long_term_debt", "cash_and_equivalents", "assets"),
             lambda r: (r["long_term_debt"] - r["cash_and_equivalents"], r["assets"]),
             require_positive_denominator=True),
    # --- margin / coverage / EBITDA (S10c; annual XBRL flows over TTM revenue) ---
    # The flow inputs are fiscal-year (~365d) durations, so these emit at fiscal-year
    # period_ends where the annual flow aligns with the TTM revenue/operating_income.
    RatioDef("gross_margin", "profitability", "ratio", "ratio",
             "gross_profit", "revenue", ("gross_profit", "rev"),
             lambda r: (r["gross_profit"], r["rev"]), require_positive_denominator=True),
    RatioDef("cost_of_revenue_to_revenue", "profitability", "ratio", "ratio",
             "cost_of_revenue", "revenue", ("cost_of_revenue", "rev"),
             lambda r: (r["cost_of_revenue"], r["rev"]), require_positive_denominator=True),
    RatioDef("interest_coverage", "leverage", "ratio", "ratio",
             "operating_income", "interest_expense", ("oi", "interest_expense"),
             lambda r: (r["oi"], r["interest_expense"]), require_positive_denominator=True),
    RatioDef("ebitda", "profitability", "level", "currency",
             "operating_income", "depreciation_amortization", ("oi", "depreciation_amortization"),
             lambda r: (r["oi"], r["depreciation_amortization"])),
    RatioDef("ebitda_margin", "profitability", "ratio", "ratio",
             "ebitda", "revenue", ("oi", "depreciation_amortization", "rev"),
             lambda r: (r["oi"] + r["depreciation_amortization"], r["rev"]),
             require_positive_denominator=True),
    # --- Altman Z'' components + composite distress score (S10d; 'health' family) ---
    RatioDef("retained_earnings_to_assets", "profitability", "ratio", "ratio",
             "retained_earnings", "assets", ("retained_earnings", "assets"),
             lambda r: (r["retained_earnings"], r["assets"]), require_positive_denominator=True),
    RatioDef("equity_to_liabilities", "leverage", "ratio", "ratio",
             "stockholders_equity", "liabilities", ("equity", "liabilities"),
             lambda r: (r["equity"], r["liabilities"]), require_positive_denominator=True),
    RatioDef("altman_z_double_prime", "health", "score", "score",
             "altman_z_double_prime_components", "n/a",
             ("current_assets", "current_liabilities", "assets", "retained_earnings", "oi", "liabilities", "equity"),
             lambda r: (None, None),
             composite=lambda r: _altman_z_double_prime(r)),
    # Piotroski F-score (S10e): nine YoY binary signals summed 0-9. Gates on the current
    # AND ~1y-prior value of every input it scores, so it emits at fiscal-year period_ends
    # where the prior year's annual flows (gross profit) and balances are also present.
    RatioDef("piotroski_f_score", "health", "score", "score",
             "piotroski_signals", "n/a",
             ("ni", "assets", "ocf", "long_term_debt", "current_assets", "current_liabilities",
              "common_shares_outstanding", "gross_profit", "rev",
              "ni_prior", "assets_prior", "long_term_debt_prior", "current_assets_prior",
              "current_liabilities_prior", "common_shares_outstanding_prior", "gross_profit_prior", "rev_prior"),
             lambda r: (None, None),
             composite=lambda r: _piotroski_f_score(r)),
    # Ohlson O-score (S10f): nine-term bankruptcy-probability logit. Needs only the
    # current balances/flows + prior-year net income (already paired) — no new XBRL
    # concept — so it emits wherever current assets/liabilities and a prior year exist.
    RatioDef("ohlson_o_score", "health", "score", "score",
             "ohlson_o_score_terms", "n/a",
             ("assets", "liabilities", "current_assets", "current_liabilities", "ni", "ocf", "ni_prior"),
             lambda r: (None, None),
             composite=lambda r: _ohlson_o_score(r)),
    # Beneish M-score (S27): eight-term manipulation-risk composite. It requires
    # current and prior-year annual-flow/balance inputs, including combined SG&A.
    RatioDef("beneish_m_score", "health", "score", "score",
             "beneish_m_score_terms", "n/a",
             ("rev", "accounts_receivable", "cost_of_revenue", "current_assets",
              "property_plant_equipment_net", "assets", "depreciation_amortization",
              "selling_general_and_administrative_expense", "liabilities", "ni", "ocf",
              "rev_prior", "accounts_receivable_prior", "cost_of_revenue_prior",
              "current_assets_prior", "property_plant_equipment_net_prior", "assets_prior",
              "depreciation_amortization_prior", "selling_general_and_administrative_expense_prior",
              "liabilities_prior"),
             lambda r: (None, None),
             composite=lambda r: _beneish_m_score(r)),
)

# Instant (balance) metrics sourced from the consolidated inline-XBRL extraction
# (fundamental_xbrl_metric), pivoted into the wide frame alongside statement-point balances.
XBRL_BALANCE_INPUTS = {
    "current_assets": "current_assets",
    "current_liabilities": "current_liabilities",
    "cash_and_equivalents": "cash_and_equivalents",
    "inventory": "inventory",
    "long_term_debt": "long_term_debt",
    "retained_earnings": "retained_earnings",
    "common_shares_outstanding": "common_shares_outstanding",  # period-end shares for Piotroski (S10e)
    "property_plant_equipment_net": "property_plant_equipment_net",  # asset structure (S10g)
    "accounts_receivable": "accounts_receivable",  # asset structure (S10g)
}

# Annual (duration) flow metrics from the consolidated inline-XBRL extraction,
# pivoted into the wide frame and joined on the fiscal-year period_end.
XBRL_FLOW_INPUTS = {
    "gross_profit": "gross_profit",
    "cost_of_revenue": "cost_of_revenue",
    "interest_expense": "interest_expense",
    "depreciation_amortization": "depreciation_amortization",
    "selling_general_and_administrative_expense": "selling_general_and_administrative_expense",
}

# Metrics for which a prior-year value is paired in (for YoY growth, average-balance
# ratios, and the Piotroski F-score YoY deltas). The S10e additions (long-term debt,
# current assets/liabilities, shares, gross profit) are paired by the same ~365-day
# window mechanism in _attach_prior_year.
GROWTH_PRIOR_KEYS = (
    "rev", "ni", "oi", "ocf", "assets", "equity",
    "liabilities", "long_term_debt", "current_assets", "current_liabilities",
    "common_shares_outstanding", "gross_profit", "cost_of_revenue",
    "depreciation_amortization", "property_plant_equipment_net",
    "accounts_receivable", "selling_general_and_administrative_expense",
)


@dataclass(frozen=True)
class FundamentalRatiosOptions:
    source: str = DEFAULT_SOURCE
    basis: str = DEFAULT_BASIS
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None


def _present(value: Any) -> bool:
    try:
        return not pd.isna(value)
    except (TypeError, ValueError):
        return value is not None


def _ratio_id(source: str, security_id: str, ratio_code: str, basis: str,
              period_end: Any, available_at: Any) -> str:
    payload = "|".join(str(p) for p in (source, security_id, ratio_code, basis, period_end, available_at))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _ratio_record(d, rec, source, basis, run_id, value, num, den, is_meaningful, available_at) -> dict:
    period_end = rec.get("period_end")
    return {
        "ratio_id": _ratio_id(source, rec.get("security_id"), d.code, basis, period_end, available_at),
        "source": source,
        "upstream_source": rec.get("upstream_source"),
        "security_id": rec.get("security_id"),
        "symbol": rec.get("symbol"),
        "cik": rec.get("cik"),
        "ratio_code": d.code,
        "ratio_category": d.category,
        "ratio_kind": d.kind,
        "basis": basis,
        "unit": d.unit,
        "period_start": rec.get("period_start"),
        "period_end": period_end,
        "fiscal_year": rec.get("fiscal_year"),
        "fiscal_period": rec.get("fiscal_period"),
        "value": value,
        "numerator_code": d.numerator_code,
        "numerator_value": num,
        "denominator_code": None if d.composite is not None else d.denominator_code,
        "denominator_value": den,
        "is_meaningful": is_meaningful,
        "is_latest_revision": True,
        "as_of_date": period_end,
        "available_at": available_at,
        "input_codes_json": json_dumps(list(d.inputs)),
        "run_id": run_id,
    }


def compute_ratio_rows(
    inputs: pd.DataFrame,
    *,
    source: str = DEFAULT_SOURCE,
    basis: str = DEFAULT_BASIS,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: wide per-(security, period) input frame -> long ratio rows.

    Each input column ``<key>`` carries a sibling ``<key>_av`` availability timestamp.
    A ratio is emitted only when every input it depends on (value AND availability) is
    present and the denominator is usable; its ``available_at`` is the max input
    availability and its ``as_of_date`` is the period close.
    """
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=RATIO_COLUMNS)

    records: list[dict] = []
    for rec in inputs.to_dict("records"):
        period_end = rec.get("period_end")
        for d in RATIO_DEFS:
            if not all(_present(rec.get(k)) for k in d.inputs):
                continue
            avs = [rec.get(f"{k}_av") for k in d.inputs]
            if not all(_present(a) for a in avs):
                continue
            available_at = max(avs)
            if d.composite is not None:
                value = d.composite(rec)
                if not _present(value):
                    continue
                records.append(_ratio_record(
                    d, rec, source, basis, run_id, float(value), None, None, True, available_at
                ))
                continue
            num, den = d.operands(rec)
            if not _present(num) or not _present(den):
                continue
            num = float(num)
            den = float(den)
            if d.kind == "level":
                value = num + den
                is_meaningful = True
            elif d.kind == "difference":
                value = num - den
                is_meaningful = True
            elif d.kind == "growth":
                if den == 0:
                    continue
                value = (num - den) / abs(den)
                is_meaningful = (not d.require_positive_denominator) or den > 0
            else:
                if den == 0:
                    continue
                value = num / den
                is_meaningful = (not d.require_positive_denominator) or den > 0
            records.append(_ratio_record(
                d, rec, source, basis, run_id, value, num, den, is_meaningful, available_at
            ))

    if not records:
        return pd.DataFrame(columns=RATIO_COLUMNS)
    return pd.DataFrame(records, columns=RATIO_COLUMNS)


def _as_date(value: Any) -> dt.date | None:
    if value is None or (not isinstance(value, dt.date) and pd.isna(value)):
        return None
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    return pd.Timestamp(value).date()


def _attach_prior_year(wide: pd.DataFrame) -> pd.DataFrame:
    """Pair each (security, period_end) row with its ~1-year-earlier row.

    For each `GROWTH_PRIOR_KEYS` metric, copies the prior period's value and its
    availability into `<key>_prior` / `<key>_prior_av`. The prior row is the one
    whose period_end falls 350-380 days before the current period_end (closest to
    365); rows with no such match get NA, so dependent growth ratios are skipped.
    """
    out = wide.copy()
    keys = [k for k in GROWTH_PRIOR_KEYS if k in out.columns and f"{k}_av" in out.columns]
    for key in keys:
        out[f"{key}_prior"] = pd.NA
        out[f"{key}_prior_av"] = pd.NaT
    if out.empty or "period_end" not in out.columns or "security_id" not in out.columns:
        return out

    ends = {idx: _as_date(pe) for idx, pe in out["period_end"].items()}
    for _, grp in out.groupby("security_id", sort=False):
        idxs = list(grp.index)
        for i in idxs:
            cur = ends.get(i)
            if cur is None:
                continue
            best, best_diff = None, None
            for j in idxs:
                prev = ends.get(j)
                if prev is None or prev >= cur:
                    continue
                gap = (cur - prev).days
                if 350 <= gap <= 380:
                    diff = abs(gap - 365)
                    if best_diff is None or diff < best_diff:
                        best, best_diff = j, diff
            if best is not None:
                for key in keys:
                    out.at[i, f"{key}_prior"] = out.at[best, key]
                    out.at[i, f"{key}_prior_av"] = out.at[best, f"{key}_av"]
    return out


def _pivot_case(prefix: str, value_col: str, metric_map: dict[str, str]) -> str:
    parts = []
    for key, metric in metric_map.items():
        parts.append(
            f"max(CASE WHEN {prefix}.canonical_metric = '{metric}' THEN {prefix}.{value_col} END) AS {key}"
        )
        parts.append(
            f"max(CASE WHEN {prefix}.canonical_metric = '{metric}' THEN {prefix}.available_at END) AS {key}_av"
        )
    return ",\n            ".join(parts)


def load_ratio_inputs(store: DuckDBStore, options: FundamentalRatiosOptions) -> pd.DataFrame:
    """Pivot the latest-revision TTM flows + instant balances into the wide input frame."""
    symbols = tuple(s for s in (options.symbols or ()) if str(s).strip())
    registered = False
    sym_join_t = ""
    sym_join_b = ""
    if symbols:
        store.con.register(
            "ratio_symbol_filter",
            pd.DataFrame({"symbol": sorted({str(s).strip().upper() for s in symbols})}),
        )
        registered = True
        sym_join_t = "JOIN ratio_symbol_filter rsf ON rsf.symbol = t.symbol"
        sym_join_b = "JOIN ratio_symbol_filter rsf ON rsf.symbol = s.symbol"
        sym_join_x = "JOIN ratio_symbol_filter rsf ON rsf.symbol = x.symbol"
    else:
        sym_join_x = ""

    sql = f"""
        WITH ttm AS (
            SELECT
                t.security_id,
                any_value(t.symbol) AS symbol,
                any_value(t.cik) AS cik,
                any_value(t.source) AS upstream_source,
                t.ttm_end_date AS period_end,
                any_value(t.ttm_start_date) AS period_start,
                any_value(t.fiscal_year) AS fiscal_year,
                any_value(t.fiscal_period) AS fiscal_period,
                {_pivot_case('t', 'ttm_value', TTM_INPUTS)}
            FROM fundamental_ttm_points t
            {sym_join_t}
            WHERE t.is_latest_revision
            GROUP BY t.security_id, t.ttm_end_date
        ),
        bal AS (
            SELECT
                s.security_id,
                s.period_end,
                {_pivot_case('s', 'value', BALANCE_INPUTS)}
            FROM fundamental_statement_points s
            {sym_join_b}
            WHERE s.is_latest_revision AND s.period_type = 'instant'
            GROUP BY s.security_id, s.period_end
        ),
        balx AS (
            SELECT
                x.security_id,
                x.period_end,
                {_pivot_case('x', 'value', XBRL_BALANCE_INPUTS)}
            FROM fundamental_xbrl_metric x
            {sym_join_x}
            WHERE x.is_latest_revision AND x.period_type = 'instant'
            GROUP BY x.security_id, x.period_end
        ),
        flowx AS (
            SELECT
                x.security_id,
                x.period_end,
                {_pivot_case('x', 'value', XBRL_FLOW_INPUTS)}
            FROM fundamental_xbrl_metric x
            {sym_join_x}
            WHERE x.is_latest_revision AND x.period_type = 'duration'
            GROUP BY x.security_id, x.period_end
        )
        SELECT
            ttm.*,
            bal.assets, bal.assets_av,
            bal.liabilities, bal.liabilities_av,
            bal.equity, bal.equity_av,
            bal.shares, bal.shares_av,
            balx.current_assets, balx.current_assets_av,
            balx.current_liabilities, balx.current_liabilities_av,
            balx.cash_and_equivalents, balx.cash_and_equivalents_av,
            balx.inventory, balx.inventory_av,
            balx.long_term_debt, balx.long_term_debt_av,
            balx.retained_earnings, balx.retained_earnings_av,
            balx.common_shares_outstanding, balx.common_shares_outstanding_av,
            balx.property_plant_equipment_net, balx.property_plant_equipment_net_av,
            balx.accounts_receivable, balx.accounts_receivable_av,
            flowx.gross_profit, flowx.gross_profit_av,
            flowx.cost_of_revenue, flowx.cost_of_revenue_av,
            flowx.interest_expense, flowx.interest_expense_av,
            flowx.depreciation_amortization, flowx.depreciation_amortization_av,
            flowx.selling_general_and_administrative_expense,
            flowx.selling_general_and_administrative_expense_av
        FROM ttm
        LEFT JOIN bal
          ON bal.security_id = ttm.security_id
         AND bal.period_end = ttm.period_end
        LEFT JOIN balx
          ON balx.security_id = ttm.security_id
         AND balx.period_end = ttm.period_end
        LEFT JOIN flowx
          ON flowx.security_id = ttm.security_id
         AND flowx.period_end = ttm.period_end
    """
    try:
        wide = store.con.execute(sql).df()
    finally:
        if registered:
            store.con.unregister("ratio_symbol_filter")
    return _attach_prior_year(wide)


def refresh_fundamental_ratios(store: DuckDBStore, options: FundamentalRatiosOptions) -> int:
    """Recompute and replace the ratio rows for ``options.source``/``options.basis``."""
    store.initialize()
    wide = load_ratio_inputs(store, options)
    rows = compute_ratio_rows(wide, source=options.source, basis=options.basis, run_id=options.run_id)
    with store.transaction():
        store.con.execute(
            "DELETE FROM fundamental_ratios WHERE source = ? AND basis = ?",
            [options.source, options.basis],
        )
        if not rows.empty:
            insert_frame(store, rows, "fundamental_ratios", "fundamental_ratios_insert")
    return int(len(rows))


class FundamentalRatiosDataset(Dataset):
    dataset_id = "fundamental_ratios"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FundamentalRatiosOptions) -> DatasetLoadResult:
        rows = refresh_fundamental_ratios(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="fundamental_ratios",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source, "basis": options.basis},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"basis": options.basis, "ratio_codes": [d.code for d in RATIO_DEFS]},
        )
