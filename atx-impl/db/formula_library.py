"""PF-S4 S4-0: formula_registry seed catalog -- definition-as-data for formulas.

This module is the FOUNDATION for the formula library: it defines the seed
row shape and the strict CSV contract for ``formula_registry``, mirroring
the item_registry seed pattern (``db/item_registry.py``) exactly:

* ``csv.DictReader`` with a strict ``fieldnames == SEED_COLUMNS`` contract
  (:func:`read_formula_registry_seed`).
* A frozen seed-row dataclass (:class:`FormulaRegistrySeedRow`).
* A direct-call loader -- NOT a :class:`db.dataset.Dataset` subclass, mirroring
  ``seed_fundamental_item_registry`` -- that DELETEs by the seed's own
  ``formula_code`` set then upserts, all inside one transaction
  (:func:`seed_formula_registry`).

Definition-as-data + citation precedent: like ``fundamental_items.csv``
(``item_registry.SEED_COLUMNS`` -- ``definition``/``citation`` columns), every
seed row carries a human ``definition`` and an optional academic/vendor
``citation``. Unlike the item registry (which requires a non-blank citation
on every row), ``citation`` here may be blank: the brief is explicit that
citation is empty for plain accounting ratios and required only for scores
and named academic formulas -- that requirement is enforced by review/S4-1/
S4-2 content discipline, not a blanket non-blank check in this loader.

S4-0 ships schema + the seed loader ONLY. No formula codes are ported here
(the committed seed CSV is a minimal placeholder); S4-1 mechanically
translates the 53 existing ``RatioDef`` entries into registry rows under a
byte-identity gate, and S4-2 adds new formula families as additional rows.

PF-S4 S4-1 additions (this task): the seed now carries the ported formula
rows, and this module gains the two small, closed, DETERMINISTIC pieces of
machinery that let ``fundamental_ratios.py`` DRIVE its ``RATIO_DEFS`` from
these rows instead of hand-written lambdas, WITHOUT ``eval``/``exec``:

* :func:`eval_operand_term` -- a tiny operand-term mini-grammar
  interpreter. Every pre-S4-1 ``RatioDef.operands`` lambda body is one of a
  handful of shapes (bare lookup, abs, sum, difference, a NaN-coalesced
  difference, an average, or "unused" for score composites). Each shape gets
  a stable ``shape:key[,key2]`` term string (e.g. ``"key:ni"``,
  ``"diff_z:current_assets,inventory"``) stored in the registry row's
  ``expression`` column as ``"<numerator_term>|<denominator_term>"``. The
  interpreter is a fixed dispatch over ``_OPERAND_TERM_EVALUATORS`` --
  parsing a small closed grammar, never executing arbitrary code.
* :data:`COMPOSITE_EVALUATORS` -- a whitelisted ``formula_code ->
  Callable[[dict], float | None]`` table for the 4 distress/quality scores
  (Altman Z'', Piotroski F, Ohlson O, Beneish M). These composites mix
  multiplicative coefficients, ``math.log``, and boolean signal-counting --
  not representable as a numerator/denominator term pair -- so per the S4-0
  review's CRITICAL design flag they stay as vetted, unit-tested Python
  functions, but are now REGISTRY-DRIVEN: the registry row names the
  evaluator via a stable ``expression`` dispatch key
  (e.g. ``"composite:altman_z_double_prime_v1"``) rather than the engine
  importing a free-floating lambda from a literal tuple.
"""
from __future__ import annotations

import csv
import json
import math
import re
from dataclasses import dataclass
from datetime import date
from pathlib import Path
from typing import Any, Callable

from .connection import DuckDBStore


SEED_PATH = Path(__file__).resolve().parent / "seeds" / "formula_registry.csv"

# Seed CSV column names. NOTE these differ slightly from the warehouse table
# column names: the CSV carries the un-suffixed `numerator_item_ids` /
# `denominator_item_ids` / `inputs` (each a JSON array literal, validated at
# read time); the table stores them with an explicit `_json` suffix
# (`numerator_item_ids_json` etc.) to match the warehouse's existing
# `*_json` VARCHAR convention (input_codes_json, natural_key_json, ...).
SEED_COLUMNS = (
    "formula_code",
    "family",
    "kind",
    "unit",
    "numerator_code",
    "denominator_code",
    "numerator_item_ids",
    "denominator_item_ids",
    "inputs",
    "transform",
    "expression",
    "is_meaningful_rule",
    "definition",
    "citation",
    "valid_from",
    "valid_to",
)

# Governed enums. `kind` mirrors fundamental_ratios.RatioDef.kind exactly
# (fundamental_ratios.py RatioDef :76-77 docstring). `transform` is the
# reducer-selector mini-grammar named in the S4-0 brief: divide/sum/
# difference/pct_change cover the existing kind branches in
# compute_ratio_rows (:618-633); identity covers multi-term `expression`
# formulas (composites, DuPont) where the top-level value is not itself a
# binary reduction of numerator/denominator.
VALID_KINDS = frozenset({"ratio", "level", "difference", "growth", "per_share", "score"})
VALID_TRANSFORMS = frozenset({"divide", "sum", "difference", "pct_change", "identity"})


# --------------------------------------------------------------------------- #
# Operand-term mini-grammar (PF-S4 S4-1)
#
# A CLOSED, DETERMINISTIC set of operand "shapes" that together reproduce
# every RatioDef.operands lambda body that existed before S4-1. No eval/exec:
# each shape is a fixed Python function looked up by name, applied to a
# small fixed number of positional input keys parsed out of the term string.
# --------------------------------------------------------------------------- #


def _term_abs(x: Any) -> float:
    return abs(float(x))


def _term_z(x: Any) -> float:
    """Coalesce a missing/NaN optional input to 0.0 (e.g. unreported inventory)."""
    try:
        import pandas as pd  # local import: formula_library has no hard pandas dependency otherwise

        return 0.0 if pd.isna(x) else float(x)
    except (TypeError, ValueError):
        return 0.0 if x is None else float(x)


def _op_key(r: dict, keys: list[str]) -> float:
    (k,) = keys
    return r[k]


def _op_abs(r: dict, keys: list[str]) -> float:
    (k,) = keys
    return _term_abs(r[k])


def _op_sum(r: dict, keys: list[str]) -> float:
    k1, k2 = keys
    return r[k1] + r[k2]


def _op_sum3(r: dict, keys: list[str]) -> float:
    """r[k1] + r[k2] + r[k3] -- e.g. cash_interest_coverage's OCF + interest + tax.

    PF-S4 S4-2: the one genuinely new operand shape this task adds (the brief's
    "extend the closed dispatch table if a new shape is genuinely needed" clause).
    A fixed 3-key sum, dispatched the same closed way as every other shape here --
    no eval/exec.
    """
    k1, k2, k3 = keys
    return r[k1] + r[k2] + r[k3]


def _op_abs_sum(r: dict, keys: list[str]) -> float:
    k1, k2 = keys
    return _term_abs(r[k1]) + _term_abs(r[k2])


def _op_diff(r: dict, keys: list[str]) -> float:
    k1, k2 = keys
    return r[k1] - r[k2]


def _op_diff_abs(r: dict, keys: list[str]) -> float:
    """r[k1] - abs(r[k2]) -- e.g. retention_ratio's net_income - |dividends|."""
    k1, k2 = keys
    return r[k1] - _term_abs(r[k2])


def _op_diff_z(r: dict, keys: list[str]) -> float:
    """r[k1] - z(r.get(k2)) -- NaN-coalesced subtraction (quick_ratio's inventory gap)."""
    k1, k2 = keys
    return r[k1] - _term_z(r.get(k2))


def _op_avg(r: dict, keys: list[str]) -> float:
    k1, k2 = keys
    return (r[k1] + r[k2]) / 2


def _op_none(r: dict, keys: list[str]) -> None:
    """The term is unused: score composites bypass operands entirely."""
    return None


# Term string shape: "<evaluator_name>:<key1>[,<key2>]". Registry rows store
# "<numerator_term>|<denominator_term>" in the `expression` column when
# `transform` is one of divide/sum/difference/pct_change (i.e. not a
# composite). This is the ENTIRE vocabulary -- adding a new shape requires
# adding a new named function here, never a string of code to execute.
_OPERAND_TERM_EVALUATORS: dict[str, Callable[[dict, list[str]], float | None]] = {
    "key": _op_key,
    "abs": _op_abs,
    "sum": _op_sum,
    "sum3": _op_sum3,
    "abs_sum": _op_abs_sum,
    "diff": _op_diff,
    "diff_abs": _op_diff_abs,
    "diff_z": _op_diff_z,
    "avg": _op_avg,
    "none": _op_none,
}


def eval_operand_term(term: str, r: dict) -> float | None:
    """Evaluate one operand term (e.g. ``"diff_z:current_assets,inventory"``) against a row.

    Raises ``KeyError`` if the term references an evaluator name outside the
    fixed :data:`_OPERAND_TERM_EVALUATORS` dispatch table (fails closed --
    never falls through to eval/exec).
    """

    shape, _, keys_part = term.partition(":")
    keys = [k for k in keys_part.split(",") if k]
    try:
        evaluator = _OPERAND_TERM_EVALUATORS[shape]
    except KeyError as exc:
        raise KeyError(f"unknown operand term shape {shape!r} in term {term!r}") from exc
    return evaluator(r, keys)


def parse_operand_expression(expression: str) -> tuple[str, str]:
    """Split a registry row's ``"<numerator_term>|<denominator_term>"`` expression."""

    numerator_term, sep, denominator_term = expression.partition("|")
    if not sep:
        raise ValueError(f"malformed operand expression {expression!r}: expected 'numer|denom'")
    return numerator_term, denominator_term


# --------------------------------------------------------------------------- #
# Composite score evaluators (PF-S4 S4-1) -- the 4 distress/quality scores.
#
# Per the S4-0 review's CRITICAL design flag: these mix multiplicative
# coefficients, math.log, and boolean signal-counting in ways that are not a
# numerator/denominator term pair, so a fully declarative representation
# without eval/exec is not practical. They remain vetted, unit-tested Python
# functions (ported verbatim from fundamental_ratios.py, same math, same
# docstrings) but are now REGISTRY-DRIVEN: a registry row names the evaluator
# via a whitelisted ``"composite:<key>"`` expression dispatch key in
# COMPOSITE_EVALUATORS, rather than the engine importing a free-floating
# lambda out of a literal RATIO_DEFS tuple.
# --------------------------------------------------------------------------- #


def altman_z_double_prime(r: dict) -> float | None:
    """Altman Z''-score (1995 emerging-markets / non-manufacturer variant).

    Z'' = 6.56·(WC/TA) + 3.26·(RE/TA) + 6.72·(EBIT/TA) + 1.05·(book equity / TL).
    Uses book equity (not market) so it is computable without price data. Operating
    income is the EBIT proxy. Distress zones: <1.1 distress, 1.1-2.6 grey, >2.6 safe.

    Citation: Altman, E.I., Hartzell, J. & Peck, M. (1995), "Emerging Markets
    Corporate Bonds: A Scoring System", Salomon Brothers.
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


def piotroski_f_score(r: dict) -> float | None:
    """Piotroski (2000) F-score: nine binary fundamental-strength signals summed 0-9.

    Profitability (4): ROA>0, CFO>0, ΔROA>0, accruals (CFO>net income).
    Funding / liquidity (3): ΔLong-term leverage<0, ΔCurrent ratio>0, no share issuance.
    Operating efficiency (2): ΔGross margin>0, ΔAsset turnover>0.

    Computed year-over-year, so it only emits where the ~1y-prior value of every input
    is paired in (and, like all flow ratios sourced from annual XBRL, where the prior row
    also reported the annual flows). Higher is stronger: 8-9 is the classic long screen,
    0-1 the short. Returns None when a balance/flow denominator is non-positive (the deltas
    would be sign-ambiguous), so the row is skipped rather than emitting a garbage score.

    Citation: Piotroski, J.D. (2000), "Value Investing: The Use of Historical
    Financial Statement Information to Separate Winners from Losers", Journal
    of Accounting Research, 38 (Supplement), pp. 1-41.
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


def ohlson_o_score(r: dict) -> float | None:
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

    Citation: Ohlson, J.A. (1980), "Financial Ratios and the Probabilistic
    Prediction of Bankruptcy", Journal of Accounting Research, 18(1), pp. 109-131.
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


def beneish_m_score(r: dict) -> float | None:
    """Beneish (1999) eight-variable M-score for earnings-manipulation risk.

    M = -4.84 + 0.920*DSRI + 0.528*GMI + 0.404*AQI + 0.892*SGI
        + 0.115*DEPI - 0.172*SGAI + 4.679*TATA - 0.327*LVGI.

    Sales, receivables, COGS, current assets, PP&E, depreciation, SG&A, liabilities,
    net income, and operating cash flow come from the PIT warehouse inputs; prior-year
    terms are paired by period_end in _attach_prior_year. Higher scores indicate more
    manipulation-risk pressure; the classic screen flags scores above about -2.22.

    Citation: Beneish, M.D. (1999), "The Detection of Earnings Manipulation",
    Financial Analysts Journal, 55(5), pp. 24-36.
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


# --------------------------------------------------------------------------- #
# PF-S4 S4-2 composite evaluators -- DuPont decomposition, Sloan accruals, and
# Montier's C-score. Like the 4 S4-1 distress/quality scores above, these mix
# multi-term products or several independent balance-sheet deltas in ways not
# representable as a single numerator/denominator operand-term pair, so they
# are vetted, unit-tested Python functions dispatched via COMPOSITE_EVALUATORS
# -- never eval/exec.
# --------------------------------------------------------------------------- #


def roe_dupont_3way(r: dict) -> float | None:
    """3-way DuPont ROE decomposition: net margin x asset turnover x equity multiplier.

    ROE = (NI/Revenue) x (Revenue/Assets) x (Assets/Equity) -- an algebraic identity
    that telescopes to NI/Equity (== ``return_on_equity``). Returns None when equity
    is non-positive (the multiplier and ROE both become sign-ambiguous).

    Citation: Brealey, R.A., Myers, S.C. & Allen, F., "Principles of Corporate
    Finance" (standard DuPont identity presentation, financial-ratio-analysis chapter).
    """
    rev = float(r["rev"])
    assets = float(r["assets"])
    equity = float(r["equity"])
    if rev == 0 or assets == 0 or equity <= 0:
        return None
    ni = float(r["ni"])
    net_margin = ni / rev
    asset_turnover = rev / assets
    equity_multiplier = assets / equity
    return net_margin * asset_turnover * equity_multiplier


def roe_dupont_5way(r: dict) -> float | None:
    """5-way extended DuPont ROE decomposition: tax burden x interest burden x
    operating margin x asset turnover x leverage.

    ROE = (NI/Pretax Income) x (Pretax Income/EBIT) x (EBIT/Revenue) x
          (Revenue/Assets) x (Assets/Equity)
    using operating income as the EBIT proxy (consistent with every other EBIT
    usage in this module, e.g. ``interest_coverage``). Telescopes to the same
    NI/Equity identity as the 3-way form. Returns None when revenue, assets,
    equity, or pretax income are non-positive/zero (a burden ratio would be
    undefined or sign-ambiguous).

    Citation: Brealey, R.A., Myers, S.C. & Allen, F., "Principles of Corporate
    Finance" (extended/5-way DuPont decomposition, financial-ratio-analysis chapter).
    """
    rev = float(r["rev"])
    assets = float(r["assets"])
    equity = float(r["equity"])
    pretax = float(r["pretax_income"])
    oi = float(r["oi"])
    if rev == 0 or assets == 0 or equity <= 0 or pretax == 0 or oi == 0:
        return None
    ni = float(r["ni"])
    tax_burden = ni / pretax
    interest_burden = pretax / oi
    operating_margin = oi / rev
    asset_turnover = rev / assets
    leverage = assets / equity
    return tax_burden * interest_burden * operating_margin * asset_turnover * leverage


def total_accruals(r: dict) -> float | None:
    """Sloan (1996) total accruals, cash-flow-statement operationalization.

    ACC = (Net Income - Operating Cash Flow) / average total assets. This is the
    standard post-Sloan simplified measure (equivalent under the paper's own
    accounting identity to the balance-sheet working-capital measure when there
    are no non-operating/discontinued items) used by most later accruals-anomaly
    replications; see :func:`working_capital_accruals` for the balance-sheet form.
    Higher (more positive) accruals indicate a larger gap between reported earnings
    and cash generation -- the earnings-quality signal Sloan's paper documents.
    Returns None when average assets is non-positive.

    Citation: Sloan, R.G. (1996), "Do Stock Prices Fully Reflect Information in
    Accruals and Cash Flows About Future Earnings?", The Accounting Review, 71(3),
    pp. 289-315; operationalization per Hribar, P. & Collins, D.W. (2002), "Errors
    in Estimating Accruals: Implications for Empirical Research", Journal of
    Accounting Research, 40(1), pp. 105-134.
    """
    assets, assets0 = float(r["assets"]), float(r["assets_prior"])
    avg_assets = (assets + assets0) / 2
    if avg_assets <= 0:
        return None
    ni = float(r["ni"])
    ocf = float(r["ocf"])
    return (ni - ocf) / avg_assets


def working_capital_accruals(r: dict) -> float | None:
    """Sloan (1996) working-capital accruals, balance-sheet operationalization.

    ACC = [(delta Current Assets - delta Cash) - (delta Current Liabilities)] /
          average total assets.

    This is Sloan's balance-sheet ACC = (dCA - dCASH) - (dCL - dSTD - dTP), with
    the dSTD (change in short-term/current-portion debt) and dTP (change in income
    taxes payable) carve-outs OMITTED: this warehouse has no short-term-debt or
    taxes-payable item registered (a genuine, documented S4-2 input gap -- see
    S4-2 report), so delta Current Liabilities is used in full rather than
    fabricating either carve-out. This is a conservative deviation (it treats
    financing-related and tax-related current-liability movements as part of the
    accrual, when Sloan's original measure nets them out); see :func:`total_accruals`
    for the unaffected cash-flow-statement measure. Returns None when average
    assets is non-positive.

    Citation: Sloan, R.G. (1996), "Do Stock Prices Fully Reflect Information in
    Accruals and Cash Flows About Future Earnings?", The Accounting Review, 71(3),
    pp. 289-315 (equation 1's balance-sheet approach; dSTD/dTP terms omitted per
    the S4-2 input-gap note above).
    """
    assets, assets0 = float(r["assets"]), float(r["assets_prior"])
    avg_assets = (assets + assets0) / 2
    if avg_assets <= 0:
        return None
    ca, ca0 = float(r["current_assets"]), float(r["current_assets_prior"])
    cash, cash0 = float(r["cash_and_equivalents"]), float(r["cash_and_equivalents_prior"])
    cl, cl0 = float(r["current_liabilities"]), float(r["current_liabilities_prior"])
    d_wc_ex_cash = (ca - ca0) - (cash - cash0)
    d_cl = cl - cl0
    return (d_wc_ex_cash - d_cl) / avg_assets


def montier_c_score(r: dict) -> float | None:
    """Montier (2008) C-score: six binary earnings-quality/manipulation red flags, summed 0-6.

    Each flag scores 1 if triggered, 0 otherwise:
      1. Growing divergence between net income and operating cash flow (NI - OCF rising).
      2. Rising accounts-receivable days (DSO increasing YoY).
      3. Rising inventory days (DIO increasing YoY).
      4. Rising "other" (non-receivable, non-inventory, non-cash) current assets as a
         share of total assets YoY -- proxied here from current_assets net of cash,
         receivables, and inventory (no dedicated "other current assets" item is
         registered; this is the documented S4-2 proxy for flag 4).
      5. Declining depreciation rate relative to gross PP&E (depreciation / (depreciation
         + net PP&E) falling YoY -- same depreciation-rate construction Beneish's DEPI uses).
      6. Total asset growth exceeding 10% YoY.

    Score interpretation: 0 = no red flags, 6 = all six triggered; scores of 4-6 are
    Montier's flagged "cook the books" zone. Returns None when a YoY input needed for
    any flag is unusable (e.g. a zero prior-year denominator for the day-count or rate
    ratios), so the row is skipped rather than emitting a partial/undercounted score.

    Citation: Montier, J. (2008), "Cooking the Books, or, More Sailing Under the Black
    Flag", Societe Generale Cross Asset Research (Global Equity Strategy), 6 June 2008.
    """
    ni, ocf = float(r["ni"]), float(r["ocf"])
    ni0, ocf0 = float(r["ni_prior"]), float(r["ocf_prior"])
    rev, rev0 = float(r["rev"]), float(r["rev_prior"])
    ar, ar0 = float(r["accounts_receivable"]), float(r["accounts_receivable_prior"])
    inv, inv0 = float(r["inventory"]), float(r["inventory_prior"])
    ca, ca0 = float(r["current_assets"]), float(r["current_assets_prior"])
    cash, cash0 = float(r["cash_and_equivalents"]), float(r["cash_and_equivalents_prior"])
    assets, assets0 = float(r["assets"]), float(r["assets_prior"])
    dep, dep0 = float(r["depreciation_amortization"]), float(r["depreciation_amortization_prior"])
    ppe, ppe0 = float(r["property_plant_equipment_net"]), float(r["property_plant_equipment_net_prior"])
    if rev == 0 or rev0 == 0 or assets <= 0 or assets0 <= 0 or (dep + ppe) == 0 or (dep0 + ppe0) == 0:
        return None

    accruals_gap = (ni - ocf) - (ni0 - ocf0)
    dso = ar / rev * 365.0
    dso0 = ar0 / rev0 * 365.0
    dio = inv / rev * 365.0
    dio0 = inv0 / rev0 * 365.0
    other_ca = ca - cash - ar - inv
    other_ca0 = ca0 - cash0 - ar0 - inv0
    other_ca_ratio = other_ca / assets
    other_ca_ratio0 = other_ca0 / assets0
    dep_rate = dep / (dep + ppe)
    dep_rate0 = dep0 / (dep0 + ppe0)
    asset_growth = (assets - assets0) / assets0

    flags = (
        accruals_gap > 0,          # 1. widening NI-over-OCF gap
        dso > dso0,                 # 2. rising receivable days
        dio > dio0,                 # 3. rising inventory days
        other_ca_ratio > other_ca_ratio0,  # 4. rising other-current-assets share
        dep_rate < dep_rate0,       # 5. declining depreciation rate
        asset_growth > 0.10,        # 6. total asset growth > 10%
    )
    return float(sum(1 for f in flags if f))


def days_sales_outstanding(r: dict) -> float | None:
    """DSO = (average accounts receivable / revenue) x 365.

    Standard working-capital-management definition (average balance, TTM revenue
    as the annualized denominator). Returns None when revenue is non-positive.

    Citation: Brigham, E.F. & Ehrhardt, M.C., "Financial Management: Theory &
    Practice" (cash-conversion-cycle chapter, standard DSO/DIO/DPO definitions).
    """
    rev = float(r["rev"])
    if rev <= 0:
        return None
    ar, ar0 = float(r["accounts_receivable"]), float(r["accounts_receivable_prior"])
    avg_ar = (ar + ar0) / 2
    return avg_ar / rev * 365.0


def days_inventory_outstanding(r: dict) -> float | None:
    """DIO = (average inventory / cost of revenue) x 365.

    Standard working-capital-management definition (average balance, TTM cost of
    revenue as the annualized denominator). Returns None when cost of revenue is
    non-positive.

    Citation: Brigham, E.F. & Ehrhardt, M.C., "Financial Management: Theory &
    Practice" (cash-conversion-cycle chapter, standard DSO/DIO/DPO definitions).
    """
    cogs = float(r["cost_of_revenue"])
    if cogs <= 0:
        return None
    inv, inv0 = float(r["inventory"]), float(r["inventory_prior"])
    avg_inv = (inv + inv0) / 2
    return avg_inv / cogs * 365.0


def days_payables_outstanding(r: dict) -> float | None:
    """DPO = (average accounts payable / cost of revenue) x 365.

    Standard working-capital-management definition (average balance, TTM cost of
    revenue as the annualized denominator). Returns None when cost of revenue is
    non-positive.

    Citation: Brigham, E.F. & Ehrhardt, M.C., "Financial Management: Theory &
    Practice" (cash-conversion-cycle chapter, standard DSO/DIO/DPO definitions).
    """
    cogs = float(r["cost_of_revenue"])
    if cogs <= 0:
        return None
    ap, ap0 = float(r["accounts_payable"]), float(r["accounts_payable_prior"])
    avg_ap = (ap + ap0) / 2
    return avg_ap / cogs * 365.0


def cash_conversion_cycle(r: dict) -> float | None:
    """CCC = DSO + DIO - DPO (days cash is tied up in the operating cycle).

    Composes the three day-count measures above rather than re-deriving them, so
    a change to any one definition automatically flows through. Returns None when
    any of the three components is unusable (revenue or cost of revenue non-positive).

    Citation: Brigham, E.F. & Ehrhardt, M.C., "Financial Management: Theory &
    Practice" (cash-conversion-cycle chapter).
    """
    dso = days_sales_outstanding(r)
    dio = days_inventory_outstanding(r)
    dpo = days_payables_outstanding(r)
    if dso is None or dio is None or dpo is None:
        return None
    return dso + dio - dpo


def eps_basic(r: dict) -> float | None:
    """Trailing basic EPS = TTM net income / weighted-average basic shares outstanding.

    Uses the TTM net income basis already computed for every other ratio in this
    module (consistent PIT/availability treatment) divided by the filed weighted-
    average basic share count -- the standard EPS denominator (NOT period-end
    shares outstanding, which would overstate/understate EPS versus the filed
    figure). Returns None when the share count is non-positive.

    Citation: FASB ASC 260, Earnings Per Share (basic EPS = income available to
    common shareholders / weighted-average common shares outstanding).
    """
    shares = float(r["shares_basic_avg"])
    if shares <= 0:
        return None
    return float(r["ni"]) / shares


def eps_diluted(r: dict) -> float | None:
    """Trailing diluted EPS = TTM net income / weighted-average diluted shares outstanding.

    Same construction as :func:`eps_basic` using the filed weighted-average DILUTED
    share count (includes the dilutive effect of options/convertibles as already
    computed by the filer -- this warehouse does not re-derive dilution). Returns
    None when the share count is non-positive.

    Citation: FASB ASC 260, Earnings Per Share (diluted EPS = income available to
    common shareholders / weighted-average diluted common shares outstanding).
    """
    shares = float(r["shares_diluted_avg"])
    if shares <= 0:
        return None
    return float(r["ni"]) / shares


def tangible_book_value_per_share(r: dict) -> float | None:
    """Tangible book value per share = (equity - goodwill - other intangibles) / shares.

    Standard "tangible common equity" per-share measure: strips goodwill and
    other (non-goodwill) intangible assets from stockholders' equity before
    dividing by period-end shares outstanding, since neither is realizable in
    a liquidation/distress scenario. Returns None when shares outstanding is
    non-positive.

    Citation: Fridson, M.S. & Alvarez, F., "Financial Statement Analysis: A
    Practitioner's Guide" (tangible book value construction).
    """
    shares = float(r["shares"])
    if shares <= 0:
        return None
    equity = float(r["equity"])
    goodwill = float(r["goodwill"])
    intangibles_other = float(r["intangibles_other"])
    return (equity - goodwill - intangibles_other) / shares


# Whitelisted formula_code -> composite evaluator. This IS the "registered
# named-callable dispatch" fallback flagged as acceptable in the S4-1 brief's
# CRITICAL design note: the registry row's `expression` column stores the
# dispatch key (``"composite:altman_z_double_prime_v1"`` etc., see
# `composite_dispatch_key`/`resolve_composite_evaluator` below); the engine
# never executes a string as code.
COMPOSITE_EVALUATORS: dict[str, Callable[[dict], float | None]] = {
    "altman_z_double_prime_v1": altman_z_double_prime,
    "piotroski_f_score_v1": piotroski_f_score,
    "ohlson_o_score_v1": ohlson_o_score,
    "beneish_m_score_v1": beneish_m_score,
    "roe_dupont_3way_v1": roe_dupont_3way,
    "roe_dupont_5way_v1": roe_dupont_5way,
    "total_accruals_v1": total_accruals,
    "working_capital_accruals_v1": working_capital_accruals,
    "montier_c_score_v1": montier_c_score,
    "days_sales_outstanding_v1": days_sales_outstanding,
    "days_inventory_outstanding_v1": days_inventory_outstanding,
    "days_payables_outstanding_v1": days_payables_outstanding,
    "cash_conversion_cycle_v1": cash_conversion_cycle,
    "eps_basic_v1": eps_basic,
    "eps_diluted_v1": eps_diluted,
    "tangible_book_value_per_share_v1": tangible_book_value_per_share,
}

_COMPOSITE_EXPRESSION_PREFIX = "composite:"


def composite_dispatch_key(formula_code: str) -> str:
    """The stable ``expression`` column value naming a composite's evaluator."""

    return f"{_COMPOSITE_EXPRESSION_PREFIX}{formula_code}_v1"


def resolve_composite_evaluator(expression: str) -> Callable[[dict], float | None]:
    """Resolve a registry row's ``expression`` dispatch key to its whitelisted evaluator.

    Raises ``KeyError`` if the key is not in the fixed COMPOSITE_EVALUATORS
    table (fails closed -- never falls through to eval/exec on unknown input).
    """

    if not expression.startswith(_COMPOSITE_EXPRESSION_PREFIX):
        raise KeyError(f"not a composite dispatch expression: {expression!r}")
    key = expression[len(_COMPOSITE_EXPRESSION_PREFIX):]
    try:
        return COMPOSITE_EVALUATORS[key]
    except KeyError as exc:
        raise KeyError(f"unknown composite evaluator key {key!r} in expression {expression!r}") from exc


@dataclass(frozen=True)
class FormulaRegistrySeedRow:
    formula_code: str
    family: str
    kind: str
    unit: str
    numerator_code: str | None
    denominator_code: str | None
    numerator_item_ids: str | None
    denominator_item_ids: str | None
    inputs: str
    transform: str
    expression: str | None
    is_meaningful_rule: str | None
    definition: str
    citation: str | None
    valid_from: str
    valid_to: str | None


def _none_if_blank(value: str | None) -> str | None:
    if value is None:
        return None
    value = value.strip()
    return value or None


def _fail(seed_path: Path, row_number: int, message: str) -> None:
    raise ValueError(f"{seed_path} row {row_number}: {message}")


def _validate_date(value: str | None, *, seed_path: Path, row_number: int, field_name: str) -> None:
    value = _none_if_blank(value)
    if value is None:
        return
    if re.fullmatch(r"\d{4}-\d{2}-\d{2}", value) is None:
        _fail(seed_path, row_number, f"invalid {field_name} date {value!r}")
    try:
        date.fromisoformat(value)
    except ValueError:
        _fail(seed_path, row_number, f"invalid {field_name} date {value!r}")


def _validate_json_array(
    value: str | None, *, seed_path: Path, row_number: int, field_name: str
) -> None:
    if value is None:
        return
    try:
        parsed = json.loads(value)
    except (TypeError, ValueError) as exc:
        _fail(seed_path, row_number, f"invalid {field_name} JSON {value!r} ({exc})")
        return
    if not isinstance(parsed, list):
        _fail(seed_path, row_number, f"invalid {field_name} JSON {value!r}: expected a JSON array")


def _validate_raw_row(raw: dict[str, str], *, seed_path: Path, row_number: int) -> None:
    if None in raw:
        _fail(seed_path, row_number, f"extra CSV fields {raw[None]!r}")

    missing_columns = [column for column in SEED_COLUMNS if column not in raw]
    if missing_columns:
        _fail(seed_path, row_number, f"missing expected fields {missing_columns!r}")

    missing_values = [column for column in SEED_COLUMNS if raw[column] is None]
    if missing_values:
        _fail(seed_path, row_number, f"missing CSV values for fields {missing_values!r}")

    for column in ("formula_code", "family", "kind", "unit", "transform", "definition", "inputs"):
        if _none_if_blank(raw[column]) is None:
            _fail(seed_path, row_number, f"blank required field {column}")

    kind = raw["kind"].strip()
    if kind not in VALID_KINDS:
        _fail(
            seed_path,
            row_number,
            f"invalid kind {kind!r}; expected one of {sorted(VALID_KINDS)}",
        )

    transform = raw["transform"].strip()
    if transform not in VALID_TRANSFORMS:
        _fail(
            seed_path,
            row_number,
            f"invalid transform {transform!r}; expected one of {sorted(VALID_TRANSFORMS)}",
        )

    numerator_code = _none_if_blank(raw["numerator_code"])
    denominator_code = _none_if_blank(raw["denominator_code"])
    if (numerator_code is None) != (denominator_code is None):
        _fail(seed_path, row_number, "partial numerator/denominator code pair")

    _validate_json_array(
        raw["inputs"], seed_path=seed_path, row_number=row_number, field_name="inputs"
    )
    _validate_json_array(
        _none_if_blank(raw["numerator_item_ids"]),
        seed_path=seed_path,
        row_number=row_number,
        field_name="numerator_item_ids",
    )
    _validate_json_array(
        _none_if_blank(raw["denominator_item_ids"]),
        seed_path=seed_path,
        row_number=row_number,
        field_name="denominator_item_ids",
    )

    _validate_date(raw["valid_from"], seed_path=seed_path, row_number=row_number, field_name="valid_from")
    _validate_date(raw["valid_to"], seed_path=seed_path, row_number=row_number, field_name="valid_to")


def _seed_row(raw: dict[str, str], *, seed_path: Path, row_number: int) -> FormulaRegistrySeedRow:
    _validate_raw_row(raw, seed_path=seed_path, row_number=row_number)
    valid_from = _none_if_blank(raw["valid_from"])
    if valid_from is None:
        _fail(seed_path, row_number, "blank required field valid_from")
    return FormulaRegistrySeedRow(
        formula_code=raw["formula_code"].strip(),
        family=raw["family"].strip(),
        kind=raw["kind"].strip(),
        unit=raw["unit"].strip(),
        numerator_code=_none_if_blank(raw["numerator_code"]),
        denominator_code=_none_if_blank(raw["denominator_code"]),
        numerator_item_ids=_none_if_blank(raw["numerator_item_ids"]),
        denominator_item_ids=_none_if_blank(raw["denominator_item_ids"]),
        inputs=raw["inputs"].strip(),
        transform=raw["transform"].strip(),
        expression=_none_if_blank(raw["expression"]),
        is_meaningful_rule=_none_if_blank(raw["is_meaningful_rule"]),
        definition=raw["definition"].strip(),
        citation=_none_if_blank(raw["citation"]),
        valid_from=valid_from,
        valid_to=_none_if_blank(raw["valid_to"]),
    )


def read_formula_registry_seed(path: Path | str = SEED_PATH) -> tuple[FormulaRegistrySeedRow, ...]:
    """Read the committed offline formula registry seed with stdlib csv."""

    seed_path = Path(path)
    with seed_path.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if tuple(reader.fieldnames or ()) != SEED_COLUMNS:
            raise ValueError(f"{seed_path} has unexpected columns: {reader.fieldnames}")
        return tuple(
            _seed_row(row, seed_path=seed_path, row_number=row_number)
            for row_number, row in enumerate(reader, start=2)
        )


def _dedupe_formula_rows(
    rows: tuple[FormulaRegistrySeedRow, ...],
) -> dict[str, FormulaRegistrySeedRow]:
    """Collapse seed rows to one per ``formula_code``, rejecting conflicting duplicates.

    An exact repeated row (identical in every field) is tolerated the same
    way item_registry tolerates repeated alias rows; a duplicate
    ``formula_code`` with DIFFERENT field values is a seed authoring error
    and fails closed.
    """
    by_code: dict[str, FormulaRegistrySeedRow] = {}
    for row in rows:
        existing = by_code.get(row.formula_code)
        if existing is not None and existing != row:
            raise ValueError(f"Duplicate conflicting formula_registry seed rows for formula_code={row.formula_code}")
        by_code[row.formula_code] = row
    return by_code


def seed_formula_registry(
    store: DuckDBStore,
    *,
    seed_path: Path | str = SEED_PATH,
) -> int:
    """Seed the formula registry from the committed CSV.

    Mirrors ``item_registry.seed_fundamental_item_registry``: DELETE the rows
    for every ``formula_code`` present in the seed file, then re-INSERT them,
    all inside one transaction. Codes NOT present in the seed file (e.g. a
    formula retired from the CSV but still present from a prior load) are
    left untouched, matching the item registry's DELETE-by-ids-then-upsert
    contract.
    """

    rows = read_formula_registry_seed(seed_path)
    by_code = _dedupe_formula_rows(rows)
    seed_codes = sorted(by_code)

    with store.transaction():
        store.con.execute(
            "DELETE FROM formula_registry WHERE formula_code = ANY(?)",
            [seed_codes],
        )
        for code in seed_codes:
            row = by_code[code]
            store.con.execute(
                """
                INSERT INTO formula_registry (
                    formula_code,
                    family,
                    kind,
                    unit,
                    numerator_code,
                    denominator_code,
                    numerator_item_ids_json,
                    denominator_item_ids_json,
                    inputs_json,
                    transform,
                    expression,
                    is_meaningful_rule,
                    definition,
                    citation,
                    valid_from,
                    valid_to
                )
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, CAST(? AS DATE), CAST(? AS DATE))
                """,
                [
                    row.formula_code,
                    row.family,
                    row.kind,
                    row.unit,
                    row.numerator_code,
                    row.denominator_code,
                    row.numerator_item_ids,
                    row.denominator_item_ids,
                    row.inputs,
                    row.transform,
                    row.expression,
                    row.is_meaningful_rule,
                    row.definition,
                    row.citation,
                    row.valid_from,
                    row.valid_to,
                ],
            )

    return len(seed_codes)


# --------------------------------------------------------------------------- #
# Registry -> engine bridge (PF-S4 S4-1)
#
# `fundamental_ratios.py` no longer hand-writes RATIO_DEFS; it calls
# `load_ratio_formula_rows()` and wraps each row into its own RatioDef
# dataclass, using `eval_operand_term`/`resolve_composite_evaluator` above to
# reconstruct the operand/composite callables. Kept file-local to
# formula_library.py (not the DB) so the pure `compute_ratio_rows` transform
# stays DB-free and unit-testable, per ROADMAP constraint (D).
# --------------------------------------------------------------------------- #

REQUIRE_POSITIVE_DENOMINATOR_RULE = "require_positive_denominator"


def load_ratio_formula_rows(
    seed_path: Path | str = SEED_PATH,
) -> tuple[FormulaRegistrySeedRow, ...]:
    """Read+dedupe the committed formula seed, in ``formula_code`` seed-file order.

    This is the offline, DB-free counterpart to :func:`seed_formula_registry`:
    it drives ``fundamental_ratios.RATIO_DEFS`` directly from the CSV so the
    pure ``compute_ratio_rows`` transform never needs a live DuckDB
    connection (ROADMAP constraint D / S4-1 brief PIT note).
    """

    rows = read_formula_registry_seed(seed_path)
    by_code = _dedupe_formula_rows(rows)
    # Preserve first-occurrence seed order (mirrors the CSV's own row order,
    # which mirrors the pre-S4-1 RATIO_DEFS tuple order) rather than sorting.
    seen: set[str] = set()
    ordered: list[FormulaRegistrySeedRow] = []
    for row in rows:
        if row.formula_code in seen:
            continue
        seen.add(row.formula_code)
        ordered.append(by_code[row.formula_code])
    return tuple(ordered)
