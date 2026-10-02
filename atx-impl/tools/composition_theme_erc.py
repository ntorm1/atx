#!/usr/bin/env python3
"""Composition rule ``theme-erc-v1`` (platform v8 expansion X, lane XCOMB) as pure functions.

Hypothesis: theme shares that give every theme sleeve an equal share of the blend's risk (equal risk contribution,
ERC) combine better than the parent's equal theme shares 1/T. Under the registry's no-information prior on theme
Sharpe ratios (every theme admitted on its literature sign, none ranked by a TRAIN statistic), equal shares let the
most volatile sleeves carry most of the blend's risk; ERC uses second moments only (no mean, no IC), which are
estimated far more precisely than means (Merton 1980; Chopra and Ziemba 1993), and sits between the minimum-variance
and the equal-weight mix (Maillard, Roncalli and Teiletche 2010).

Registration (task-XCOMB-report.md; every constant fixed blind, declared before any cell read):
  1. Members and within-theme shares are the parent's: a_k = the parent rule's pre-cap within-theme share of member k
     (its provenance ``weights_before_cap`` normalised inside the theme: ew-theme-std-v1 tier-score shares,
     ic-shrink-v1 shrunk-IC shares); signs, tiers and the member set unchanged.
  2. Theme sleeve t: r_t(d) = sum_{k in t} a_k * s_k * f_k(d) over the parent fit's decisions (fit_prior's mask: the
     TRAIN decisions of the role), f_k the fitter's factor series (the member's neutralised gross-1 centred-rank
     book, a flat decision 0), s_k the prior sign. C = the sample covariance of the sleeves (divisor n - 1), themes
     in sorted name order. No new window and no new read: the series are the ones the fitter already holds.
  3. Theme shares b = the ERC shares of C: Spinu's (2013) problem by cyclical coordinate descent, exactly SWEEPS
     sweeps from the inverse-volatility point (``erc_shares``, the arithmetic of atx/engine/combine/group_erc.hpp in
     the same order); refused unless max_t |c_t / mean(c) - 1| <= DISPERSION (c_t = b_t (C b)_t).
  4. w_k = a_k * b_theme(k), then the parent's member cap 1/(2T) (composition_rules.member_cap; T = themes with a
     member).
  5. Standardisation unchanged: the IC runner applies ew-theme-std-v1's per-date re-rank; W_theme = the sum of the
     theme's weights carries b into the blend.
  Parents: ew-theme-std-v1 and ic-shrink-v1 (``PARENT_RULES``: the standardised compositions a v8 parent can carry; the
  aim variants need R-3, which was not accepted). Refused with --theme-resid (residualised composites are not the
  sleeves whose covariance is measured) and under --era (registered on the single role).

Split of work. The fitter (``fit_composition_weights.py --theme-erc theme-erc-v1`` on the parent's fit argv) fits the
parent composition, then this rule; the weights file carries theme_standardise {rule: theme-erc-v1, rerank: true,
themes, theme_erc: {sweeps, dispersion, members: {id: {theme, share}}, covariance: {themes, matrix}}} and
provenance.theme_erc. The IC runner's theme_standardise rule table (strategy_ic_admission.cpp) re-applies steps 3-4
to the recorded members and covariance (strategy_ic_theme_erc.cpp) and refuses a weight off by more than
RUNNER_TOLERANCE. The shared fixture atx-impl/tests/fixtures/theme_erc_v1.json pins both sides.
"""
from __future__ import annotations

import argparse
import hashlib
import math
from pathlib import Path

import numpy as np

import composition_rules  # this directory: member_cap, StdFit, the v2 schema

RULE_ID = "theme-erc-v1"
BLOCK = "theme_erc"                      # the key inside theme_standardise
PARENT_RULES = ("ew-theme-std-v1", "ic-shrink-v1")
SWEEPS = 10000                           # strategy_ic_theme_erc.hpp theme_erc_sweeps (no early exit)
DISPERSION = 1e-10                       # theme_erc_dispersion: max |c_t / mean(c) - 1| after the sweeps
SHARE_TOLERANCE = 1e-12                  # theme_erc_share_tolerance: |sum of a theme's shares - 1|
RUNNER_TOLERANCE = 1e-12                 # theme_erc_weight_tolerance: |pinned - rule| per weight
MAX_THEMES = 32                          # the runner's theme_standardise bound
ANNUALIZATION = 252                      # report only (sleeve vols)
SERIES = ("r_t(d)=sum_{k in t} a_k*s_k*f_k(d) over the parent fit's decisions (fit_prior's mask; f_k the member's "
          "neutralised gross-1 centred-rank book return, a flat decision 0; s_k the prior sign); C = sample covariance "
          "(divisor n-1), themes in sorted name order")
RULE_TEXT = (
    "1. members and within-theme shares a_k = the parent rule's pre-cap within-theme shares (unchanged)",
    "2. theme sleeve returns r_t(d) = sum_{k in t} a_k s_k f_k(d) over the parent fit's decisions; C = their sample "
    "covariance (divisor n-1)",
    "3. theme shares b = the equal risk contribution shares of C (Spinu's problem, cyclical coordinate descent, 10000 "
    "sweeps from the inverse-volatility point; refused unless max |c_t/mean(c) - 1| <= 1e-10)",
    "4. w_k = a_k * b_theme(k), then the member cap 1/(2T) as in ew-theme-std-v1",
    "5. standardisation unchanged (ew-theme-std-v1's per-date re-rank; W_theme = the theme's weight sum)")
COMPOSITION_TEXT = (
    "theme-erc-v1: w_k=a_k*b_t (a_k the parent's pre-cap within-theme share; b the equal-risk-contribution shares of "
    "the theme sleeves' TRAIN covariance, sleeve r_t=sum_{k in t} a_k*s_k*f_k), then member cap 1/(2T) with the excess "
    "pro rata to the other themes' uncapped members (repeated to a fixed point); per date the IC runner applies "
    "theme_standardise (ew-theme-std-v1's re-rank) to these weights")
FIT_SERIES = ("TRAIN covariance of the theme sleeves (s_k*f_k over ALL TRAIN scored decisions, flat decisions 0, mixed "
              "by the parent's within-theme shares) sets the theme shares; no mean is estimated; diagnostic uses "
              "s_k*f over ALL TRAIN scored decisions without the per-date theme re-rank")
PARENT_ONLY = (f"--theme-erc {RULE_ID} needs the prior path (--orientation prior, a v4 screen) and --composition "
               f"{' or '.join(PARENT_RULES)}; it is refused with --theme-resid and under --era")


class ErcError(ValueError):
    """A loud refusal of the rule's inputs (the fitter passes its own FitError instead)."""


def _require(condition, message: str, error: type[Exception]) -> None:
    if not condition:
        raise error(message)


def module_sha256() -> str:
    return hashlib.sha256(Path(__file__).resolve().read_bytes()).hexdigest()


# ---------------------------------------------------------------- step 3: the engine kernel's arithmetic
def erc_shares(cov: list[list[float]], sweeps: int = SWEEPS,
               error: type[Exception] = ErcError) -> tuple[list[float], list[float], float]:
    """atx::engine::combine::group_erc_shares in the same order: (shares, contributions, dispersion)."""
    g = len(cov)
    _require(g > 0 and sweeps > 0 and all(isinstance(r, (list, tuple)) and len(r) == g for r in cov),
             f"{RULE_ID}: the covariance must be square and non-empty", error)
    _require(all(isinstance(v, float) and math.isfinite(v) for r in cov for v in r),
             f"{RULE_ID}: a non-finite covariance entry", error)
    for t in range(g):
        _require(cov[t][t] > 0.0, f"{RULE_ID}: a variance that is not > 0", error)
        for u in range(t + 1, g):
            _require(cov[t][u] == cov[u][t], f"{RULE_ID}: the covariance is not symmetric", error)
    budget = 1.0 / g
    x = [1.0 / math.sqrt(cov[t][t]) for t in range(g)]
    for _ in range(sweeps):
        for t in range(g):
            row = cov[t]
            beta = 0.0
            for u in range(g):
                if u != t:
                    beta += row[u] * x[u]
            diag = row[t]
            root = math.sqrt(beta * beta + 4.0 * diag * budget)
            x[t] = 2.0 * budget / (beta + root) if beta >= 0.0 else (-beta + root) / (2.0 * diag)
    total = 0.0
    for v in x:
        total += v
    _require(math.isfinite(total) and total > 0.0 and all(math.isfinite(v) and v > 0.0 for v in x),
             f"{RULE_ID}: the iterates are not finite and positive (an indefinite covariance)", error)
    share = [v / total for v in x]
    contribution = []
    total_c = 0.0
    for t in range(g):
        marginal = 0.0
        for u in range(g):
            marginal += cov[t][u] * share[u]
        contribution.append(share[t] * marginal)
        total_c += contribution[-1]
    mean = total_c / g
    _require(math.isfinite(mean) and mean > 0.0,
             f"{RULE_ID}: the risk contributions are not positive (an indefinite covariance)", error)
    dispersion = 0.0
    for c in contribution:
        dispersion = max(dispersion, abs(c / mean - 1.0))
    return share, contribution, dispersion


# ---------------------------------------------------------------- steps 1, 2 and 4
def parent_shares(parent: composition_rules.StdFit, ids: list[str], themes: list[str],
                  error: type[Exception] = ErcError) -> list[float]:
    """Step 1: the parent's pre-cap weights (provenance.weights_before_cap) normalised inside each theme."""
    before = parent.provenance.get("weights_before_cap") if isinstance(parent.provenance, dict) else None
    _require(isinstance(before, dict) and all(i in before for i in ids),
             f"{RULE_ID}: the parent fit records no weights_before_cap for every member", error)
    uncapped = [float(before[i]) for i in ids]
    total: dict[str, float] = {}
    for t, u in zip(themes, uncapped):
        total[t] = total.get(t, 0.0) + u
    _require(all(v > 0.0 for v in total.values()), f"{RULE_ID}: a theme of the parent with no weight", error)
    return [u / total[t] for t, u in zip(themes, uncapped)]


def sleeve_series(matrix: np.ndarray, themes: list[str], shares: list[float], order: list[str],
                  mask: np.ndarray) -> np.ndarray:
    """Step 2: one row per theme of ``order``, r_t(d) = sum_{k in t} a_k * matrix[k, d] (members in input order) over
    the decisions of ``mask``. ``matrix`` rows are the members' signed factor series s_k f_k (flat 0)."""
    cols = np.asarray(matrix, dtype=np.float64)[:, np.asarray(mask, dtype=bool)]
    rows = []
    for t in order:
        r = np.zeros(cols.shape[1], dtype=np.float64)
        for k, theme in enumerate(themes):
            if theme == t:
                r = r + shares[k] * cols[k]
        rows.append(r)
    return np.vstack(rows)


def covariance(series: np.ndarray, error: type[Exception] = ErcError) -> list[list[float]]:
    """Step 2: the sample covariance (divisor n - 1) of the rows of ``series``, exactly symmetric."""
    n = series.shape[1]
    _require(n >= 2, f"{RULE_ID}: the theme sleeves need at least two decisions", error)
    centred = series - series.mean(axis=1, keepdims=True)
    g = series.shape[0]
    cov = [[0.0] * g for _ in range(g)]
    for i in range(g):
        for j in range(i, g):
            cov[i][j] = cov[j][i] = float(np.dot(centred[i], centred[j])) / (n - 1)
    return cov


def rule_weights(ids: list[str], themes: list[str], shares: list[float], order: list[str], cov: list[list[float]],
                 error: type[Exception] = ErcError) -> dict:
    """Steps 3-4 on recorded inputs (what the runner re-applies): theme shares, contributions, dispersion, weights
    before and after the cap, cap passes."""
    _require(len(ids) == len(themes) == len(shares) and ids, f"{RULE_ID}: members, themes and shares differ in length "
                                                             "or are empty", error)
    _require(len(set(ids)) == len(ids), f"{RULE_ID}: duplicate member", error)
    _require(sorted(set(themes)) == sorted(order) and len(set(order)) == len(order) and len(order) <= MAX_THEMES,
             f"{RULE_ID}: the covariance themes must be the members' themes (at most {MAX_THEMES})", error)
    _require(all(isinstance(a, float) and math.isfinite(a) and a >= 0.0 for a in shares),
             f"{RULE_ID}: within-theme shares must be finite and >= 0", error)
    for t in order:
        s = 0.0
        for th, a in zip(themes, shares):
            if th == t:
                s += a
        _require(abs(s - 1.0) <= SHARE_TOLERANCE, f"{RULE_ID}: the within-theme shares of {t} do not sum to 1", error)
    b, c, dispersion = erc_shares(cov, SWEEPS, error)
    _require(dispersion <= DISPERSION, f"{RULE_ID}: the risk contributions did not equalise within the registered "
                                       f"sweeps (dispersion {dispersion})", error)
    share_of = dict(zip(order, b))
    uncapped = np.array([a * share_of[t] for t, a in zip(themes, shares)], dtype=np.float64)
    cap = 1.0 / (2 * len(order))
    weights, passes = composition_rules.member_cap(uncapped, themes, cap, error)
    return {"theme_shares": b, "contributions": c, "dispersion": dispersion, "uncapped": uncapped,
            "weights": weights, "passes": passes, "cap": cap}


def theme_erc(parent: composition_rules.StdFit, ids: list[str], themes: list[str], matrix: np.ndarray,
              mask: np.ndarray, parent_rule: str, error: type[Exception] = ErcError) -> composition_rules.StdFit:
    """theme-erc-v1 over the parent fit's members (input order: the fitter's admission order), with the theme table,
    the runner block and provenance (attached by ``attach``)."""
    _require(parent_rule in PARENT_RULES, f"{RULE_ID}: {PARENT_ONLY}", error)
    _require(len(ids) == len(themes) == np.asarray(matrix).shape[0] and ids,
             f"{RULE_ID}: one factor series per member", error)
    shares = parent_shares(parent, ids, themes, error)
    order = sorted(set(themes))
    mask = np.asarray(mask, dtype=bool)
    series = sleeve_series(matrix, themes, shares, order, mask)
    cov = covariance(series, error)
    fit = rule_weights(ids, themes, shares, order, cov, error)
    weights, cap = fit["weights"], fit["cap"]
    capped = {k for it in fit["passes"] for k in it["capped"]}
    vols = [math.sqrt(ANNUALIZATION * cov[i][i]) for i in range(len(order))]
    corr = [[cov[i][j] / math.sqrt(cov[i][i] * cov[j][j]) for j in range(len(order))] for i in range(len(order))]
    theme_table = {}
    for i, t in enumerate(order):
        members = [k for k in range(len(ids)) if themes[k] == t]
        theme_table[t] = {
            "members": [ids[k] for k in members], "admitted_count": len(members),
            "theme_weight_uncapped": fit["theme_shares"][i], "erc_share": fit["theme_shares"][i],
            "risk_contribution": fit["contributions"][i], "sleeve_vol_annualised": vols[i],
            "theme_weight": float(sum(weights[k] for k in members)),
            "within_theme_shares": {ids[k]: shares[k] for k in members},
            "member_weights": {ids[k]: float(weights[k]) for k in members},
            "capped_members": [ids[k] for k in members if k in capped]}
    block = {"rule": RULE_ID, "rerank": True,
             "themes": {ids[k]: themes[k] for k in range(len(ids)) if weights[k] > 0},
             BLOCK: {"sweeps": SWEEPS, "dispersion": DISPERSION,
                     "members": {ids[k]: {"theme": themes[k], "share": shares[k]} for k in range(len(ids))},
                     "covariance": {"themes": list(order), "matrix": cov}}}
    provenance = {
        "rule": RULE_ID, "parent_rule": parent_rule,
        "registration": "task-XCOMB-report.md (platform v8 expansion X, lane XCOMB), declared before any cell read",
        "rule_text": list(RULE_TEXT), "series": SERIES, "decisions": int(mask.sum()),
        "sweeps": SWEEPS, "dispersion_tolerance": DISPERSION, "dispersion": fit["dispersion"],
        "share_tolerance": SHARE_TOLERANCE, "runner_tolerance_absolute": RUNNER_TOLERANCE,
        "theme_order": list(order), "covariance": cov,
        "theme_shares": dict(zip(order, fit["theme_shares"])),
        "risk_contributions": dict(zip(order, fit["contributions"])),
        "sleeve_vol_annualised": dict(zip(order, vols)),
        "sleeve_correlation_report_only": {t: dict(zip(order, row)) for t, row in zip(order, corr)},
        "within_theme_shares": dict(zip(ids, shares)), "theme_count": len(order), "cap": cap,
        "cap_tolerance_relative": composition_rules.CAP_TOLERANCE,
        "weights_before_cap": {ids[k]: float(fit["uncapped"][k]) for k in range(len(ids))},
        "cap_iterations": [{"capped": [ids[k] for k in it["capped"]], "excess_by_theme": it["excess_by_theme"]}
                           for it in fit["passes"]],
        "capped_members": [ids[k] for k in sorted(capped)],
        "runner": (f"atx-equity-strategy-ic theme_standardise {{rule: {RULE_ID}, rerank: true, themes, {BLOCK}}}: the "
                   "ew-theme-std-v1 per-date re-rank, unchanged; the rule table re-applies the ERC shares and the cap "
                   "to theme_erc.members and theme_erc.covariance and refuses a weight off by more than "
                   "runner_tolerance_absolute (strategy_ic_admission.cpp, strategy_ic_theme_erc.cpp)"),
        "module_sha256": module_sha256()}
    return composition_rules.StdFit(weights=weights, theme_table=theme_table, block=block, provenance=provenance,
                                    text=COMPOSITION_TEXT, fit_series=FIT_SERIES, ids=list(ids))


def attach(document: dict, fit: composition_rules.StdFit) -> dict:
    """The weights document of the rule: schema v2, the rule's theme_standardise block in place of the parent's,
    provenance.theme_erc, and provenance.rule = theme-erc-v1 (the parent's rule stays in provenance.theme_erc and its
    own provenance block, std or ic_shrink, which records the shares this rule read)."""
    document["schema"] = composition_rules.WEIGHTS_SCHEMA_V2
    document["theme_standardise"] = dict(fit.block)
    document["provenance"]["rule"] = RULE_ID
    document["provenance"]["theme_erc"] = fit.provenance
    return document


# ---------------------------------------------------------------- the fitter hooks
def add_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--theme-erc", default=None, choices=(RULE_ID,),
                        help="v8 X (lane XCOMB): theme-erc-v1 on the composition's within-theme shares "
                             f"({', '.join(PARENT_RULES)}): theme shares with equal risk contributions of the theme "
                             "sleeves' TRAIN covariance, then the member cap 1/(2T); absent: output bytes unchanged")


def check_args(args, prior: bool, pooled: bool, error: type[Exception] = ErcError) -> None:
    """Refused before any compute: the flag needs the prior path and a PARENT_RULES composition, and is refused with
    --theme-resid and under --era."""
    if getattr(args, "theme_erc", None) is None:
        return
    _require(prior and getattr(args, "composition", None) in PARENT_RULES and
             getattr(args, "theme_resid", None) is None and not pooled, f"{RULE_ID}: {PARENT_ONLY}", error)
