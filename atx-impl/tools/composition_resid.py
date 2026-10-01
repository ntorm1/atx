#!/usr/bin/env python3
"""Composition rule ``theme-resid-v1`` (platform v8 R-11, Ruling E-38 slot 50) as pure functions.

Registration (lane ORTH, task-R-11-report.md; every constant fixed blind, declared before any read):
  Hypothesis: theme composites residualised against the preceding themes carry uncorrelated alpha and combine better
  than the raw composites.
  1. Order: the registered theme order (Ruling PM4-11, ``registered_order``): fit_composition_weights.PRIOR_THEMES, the
     alpha registry's ``themes`` table in its file order, whose first ten themes are frozen (``FROZEN_PREFIX``: the v4
     pre-registration list plus the v7 appended theme) and any theme registered later follows in registration order
     (``filing_events`` after ``ownership_flow``); restricted to the themes with a weighted member. A theme outside it
     is refused. Derived from the list, never from a data statistic.
  2. Per decision session and theme t, the standardised composite z_t is the parent's (ew-theme-std-v1): the centred
     tied rank, over the names with a present member of t, of the sum of the present members' w_k s_k rank_k. A lone
     present name has z = 0; a name without a present member has none.
  3. The first theme in order keeps z_1. Theme t > 1: e_t = the cross-sectional least-squares residual of z_t on an
     intercept and z_1 .. z_{t-1}, over the names where t is present; a preceding theme absent for a name enters as 0,
     its neutral value. A regressor whose part outside the span of the intercept and the earlier kept regressors is at
     most SPAN_TOLERANCE (1e-10) of its centred norm is dropped (the residual is that of the independent regressors);
     a residual at most SPAN_TOLERANCE of the centred norm of z_t is exactly 0, and theme t adds nothing that session.
  4. Re-standardise: e_t is re-ranked (centred tied rank in [-.5, .5]) over the same names. Ties (Ruling PM4-12, the
     registered text being silent; declared before any read): names tied in theme t's own z_t stay tied: inside each
     exact tie block of z_t (exact equality of the standardised composite over t's names) e_t is replaced by the
     block's arithmetic mean (summed from the block's first name in ascending name order, divided by the block size;
     a block of one name is untouched), and those block means are re-ranked. A composite without ties gives the result
     of the text above bit for bit (``tie_block_means``).
  5. Blend = sum over themes of W_t x the re-standardised residual (z_1 for the first), W_t = the parent's theme share
     (the sum of the theme's member weights). Member weights inside each theme, signs, tiers and the member cap are the
     parent's, unchanged: the fitter writes the parent composition's weights file plus the ``theme_residualise`` block.
  6. Defined only on a parent whose weights file carries ``theme_standardise`` with rerank true (ew-theme-std-v1 and the
     compositions that write its block, and R-10's ic-shrink-v1 / ic-shrink-aim-v1 blocks, whose per-date
     standardisation is the same: ``STANDARDISE_RULES``); the fitter refuses ``--theme-resid`` otherwise.

Split of work. The fitter (``fit_composition_weights.py --theme-resid theme-resid-v1``) fits the parent's composition
unchanged and attaches ``theme_residualise`` {rule, order} and ``provenance.resid`` (``attach``). The IC runner
(atx-impl/src/strategy_ic_theme_resid.cpp, IcThemeRule::residualise; least squares by modified Gram-Schmidt with one
re-orthogonalisation pass, atx/engine/combine/group_residualise.hpp) applies rules 2-5 per session. ``blend`` below is
the independent numpy reference of the runner's rule (least squares by ``numpy.linalg.lstsq``) and ``kernel`` its
plane-level form (the arguments of add_theme_residualised), the Python side of the "Python equals C++" check
(strategy_ic_theme_resid_test.cpp pins the same fixture values). No TRAIN statistic, window or read is added: the
fitter's inputs are the parent's.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

import numpy as np

RULE_ID = "theme-resid-v1"
BLOCK = "theme_residualise"
STD_RULE_ID = "ew-theme-std-v1"   # composition_rules.STD_RULE_ID: the runner block the rule rides on
# Rule 6 (Rulings E-44, E-45): every theme_standardise rule of the IC runner's table (strategy_ic_admission.cpp
# standardise_rules) runs ew-theme-std-v1's per-date standardisation, so a rerank-true block of any of them is a
# standardised parent: ew-theme-std-v1 (R-1; R-3's ew-theme-std-aim-v1 writes it too) and R-10's ic-shrink-v1 /
# ic-shrink-aim-v1 (composition_ic_shrink.RULES; tested against it).
STANDARDISE_RULES = (STD_RULE_ID, "ic-shrink-v1", "ic-shrink-aim-v1")
WEIGHTS_SCHEMA_V2 = "atx.dsl-composition-weights/v2"
# Rule 1 (blind, Ruling PM4-11): the first ten themes of the registered order, frozen in their registered order
# (fit_composition_weights.V4_THEMES + V7_APPENDED_THEMES); the order itself is PRIOR_THEMES (registered_order).
FROZEN_PREFIX = ("value", "profitability_quality", "investment_issuance", "earnings_momentum", "price_momentum",
                 "low_risk", "short_interest", "reversal_seasonality", "options_implied", "ownership_flow")
SPAN_TOLERANCE = 1e-10            # rule 3: atx/engine/combine/group_residualise.hpp kResidualSpanTolerance
RULE_TEXT = (
    "1. themes in the registered order (Ruling PM4-11: PRIOR_THEMES, the registry order; its first ten frozen: v4 "
    "pre-registration list + v7 appended theme; a later theme in registration order, filing_events after "
    "ownership_flow), restricted to the themes with a weighted member",
    "2. per session the parent's standardised theme composite z_t: centred tied rank over the names with a present "
    "member of the sum of present w_k*s_k*rank_k (ew-theme-std-v1)",
    "3. theme t>1: least-squares residual of z_t on an intercept and z_1..z_{t-1} over the names where t is present "
    "(an absent preceding theme enters as 0); dependent regressors dropped and a residual within 1e-10 of its centred "
    "norm is 0 (span tolerance 1e-10 relative)",
    "4. re-standardise: re-rank the residual (centred tied rank) over the same names; Ruling PM4-12: names tied in z_t "
    "(exact equality) stay tied: inside each tie block the residual is replaced by the block mean (summed in ascending "
    "name order) before the re-rank; without ties the residual is unchanged",
    "5. blend = sum over themes of W_t x re-standardised residual (z_1 for the first theme); W_t, member weights, "
    "signs and the member cap are the parent's, unchanged")
PRIOR_ONLY = ("--theme-resid needs the prior path (--orientation prior, a v4 screen) and a composition whose weights "
              "file carries theme_standardise with rerank true")


class ResidError(ValueError):
    """A loud refusal of the rule's inputs (the fitter passes its own FitError instead)."""


def _require(condition, message: str, error: type[Exception]) -> None:
    if not condition:
        raise error(message)


def module_sha256() -> str:
    return hashlib.sha256(Path(__file__).resolve().read_bytes()).hexdigest()


# ---------------------------------------------------------------- the weights file
def registered_order(prior_themes, error: type[Exception] = ResidError) -> tuple[str, ...]:
    """Rule 1 (Ruling PM4-11): the registered theme order derived from ``prior_themes`` (the fitter's PRIOR_THEMES, the
    registry order): its first ten themes must be ``FROZEN_PREFIX`` in that order, every later theme follows in the
    order it was registered, and no theme repeats."""
    themes = tuple(prior_themes)
    _require(all(isinstance(t, str) and t for t in themes) and len(set(themes)) == len(themes),
             f"{RULE_ID}: the registered theme list must name each theme once: {list(themes)}", error)
    _require(themes[:len(FROZEN_PREFIX)] == FROZEN_PREFIX,
             f"{RULE_ID}: the registered theme order must begin with the frozen ten {list(FROZEN_PREFIX)} in that order "
             f"(Ruling PM4-11; a later theme is appended in registration order): {list(themes)}", error)
    return themes


def theme_order(themes, registered, error: type[Exception] = ResidError) -> list[str]:
    """Rule 1: ``registered_order(registered)`` restricted to ``themes``; a theme outside it is refused."""
    order = registered_order(registered, error)
    present = set(themes)
    unknown = sorted(present - set(order))
    _require(not unknown, f"{RULE_ID}: themes outside the registered order {list(order)}: {unknown}", error)
    _require(present, f"{RULE_ID}: no weighted theme", error)
    return [t for t in order if t in present]


def resid_block(document: dict, registered, error: type[Exception] = ResidError) -> dict:
    """The ``theme_residualise`` block for a weights document that carries a rerank-true ``theme_standardise``;
    ``registered``: the fitter's PRIOR_THEMES (rule 1)."""
    std = document.get("theme_standardise") if isinstance(document, dict) else None
    if not (isinstance(std, dict) and std.get("rule") in STANDARDISE_RULES and std.get("rerank") is True and
            isinstance(std.get("themes"), dict) and document.get("schema") == WEIGHTS_SCHEMA_V2):
        raise error(f"{RULE_ID}: {PRIOR_ONLY}")
    weights = document.get("weights", {})
    themes = [t for cid, t in std["themes"].items() if weights.get(cid, 0) > 0]
    return {"rule": RULE_ID, "order": theme_order(themes, registered, error)}


def attach(document: dict, registered, error: type[Exception] = ResidError) -> dict:
    """The weights document of the rule: the parent's document plus ``theme_residualise`` and ``provenance.resid``;
    weights, signs, schema and the theme_standardise block are untouched. ``registered``: the fitter's PRIOR_THEMES."""
    block = resid_block(document, registered, error)
    document[BLOCK] = block
    document.setdefault("provenance", {})["resid"] = {
        "rule": RULE_ID,
        "registration": "task-R-11-report.md (platform v8 R-11, Ruling E-38 slot 50), declared before any read",
        "rule_text": list(RULE_TEXT), "order": list(block["order"]),
        "registered_order": list(registered_order(registered, error)), "frozen_prefix": list(FROZEN_PREFIX),
        "span_tolerance_relative": SPAN_TOLERANCE,
        "parent_composition": document.get("provenance", {}).get("rule"),
        "weights": "the parent composition's, unchanged (theme shares, within-theme weights, signs, member cap)",
        "runner": ("atx-equity-strategy-ic theme_residualise {rule, order} beside theme_standardise {rerank: true}: "
                   "per session the theme at position t of `order` adds W_t x the centred tied re-rank of the "
                   "least-squares residual of its standardised composite on an intercept and the earlier themes' "
                   "(absent = 0) over its present names; the first theme adds W_1 x its composite "
                   "(strategy_ic_theme_resid.cpp, IcThemeRule::residualise)"),
        "diagnostic": ("provenance.blend_in_sample_TRAIN_diagnostic excludes the per-session re-rank and "
                       "residualisation"),
        "module_sha256": module_sha256()}
    return document


def add_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--theme-resid", default=None, choices=(RULE_ID,),
                        help="v8 R-11: theme-resid-v1 on the composition's theme_standardise block (rerank true): "
                             "the same weights plus a theme_residualise block, so the IC runner residualises each "
                             "standardised theme composite on the earlier ones in the registered theme order; absent: "
                             "output bytes unchanged")


def apply(args, document: dict, summary: dict, registered, error: type[Exception] = ResidError) -> None:
    """The fitter hook: with ``--theme-resid`` attach the block (refused without a rerank-true theme_standardise) and
    record it in the summary; without it nothing changes. ``registered``: the fitter's PRIOR_THEMES (rule 1)."""
    if getattr(args, "theme_resid", None) is None:
        return
    attach(document, registered, error)
    summary[BLOCK] = dict(document[BLOCK])


# ---------------------------------------------------------------- reference of the runner's rule (numpy)
def centred_tied_ranks(values: np.ndarray, keep: np.ndarray) -> np.ndarray:
    """Centred tied rank of each kept value among the kept ones, (less + (less + equal - 1)) / (2 (n - 1)) - .5; NaN
    elsewhere and everywhere when fewer than two are kept."""
    out = np.full(values.shape, np.nan)
    idx = np.flatnonzero(keep)
    n = len(idx)
    if n < 2:
        return out
    v = values[idx]
    for a, i in enumerate(idx):
        less, equal = int(np.sum(v < v[a])), int(np.sum(v == v[a]))
        out[i] = (less + (less + equal - 1)) / (2.0 * (n - 1)) - 0.5
    return out


def residual(y: np.ndarray, x: np.ndarray, tolerance: float = SPAN_TOLERANCE) -> tuple[np.ndarray, bool]:
    """Least-squares residual of y on [1, x] (x: n x m) by numpy.linalg.lstsq; exactly 0 (spanned) within tolerance."""
    design = np.column_stack([np.ones(len(y))] + ([x] if x.size else []))
    beta = np.linalg.lstsq(design, y, rcond=None)[0]
    e = y - design @ beta
    centred = float(np.linalg.norm(y - y.mean()))
    if not centred > 0 or float(np.linalg.norm(e)) <= tolerance * centred:
        return np.zeros(len(y)), True
    return e, False


def tie_block_means(e: np.ndarray, z: np.ndarray) -> np.ndarray:
    """Rule 4's tie step (Ruling PM4-12): ``e`` with each run of exactly equal ``z`` (a tie block; the names sorted by
    (z, position), so a block lists its names in ascending order) of two or more names replaced by the block mean,
    summed from the block's first name in ascending name order and divided by the block size (the C++ expression and
    order, strategy_ic_theme_resid.cpp mean_over_tie_blocks). A block of one name is untouched."""
    out = np.array(e, dtype=np.float64)
    order = sorted(range(len(z)), key=lambda k: (float(z[k]), k))
    b = 0
    while b < len(order):
        end = b + 1
        while end < len(order) and z[order[end]] == z[order[b]]:
            end += 1
        if end - b > 1:
            total = float(e[order[b]])
            for k in order[b + 1:end]:
                total += float(e[k])
            mean = total / float(end - b)
            for k in order[b:end]:
                out[k] = mean
        b = end
    return out


def standardise(total: np.ndarray, present: np.ndarray) -> np.ndarray:
    """Rule 2 on one session (themes x names): the centred tied rank of each theme's sum over its present names; a lone
    present name 0; NaN where the theme has no present member."""
    z = np.full(total.shape, np.nan)
    for t in range(total.shape[0]):
        if present[t].sum() == 1:
            z[t, present[t]] = 0.0
        elif present[t].sum() >= 2:
            z[t] = centred_tied_ranks(total[t], present[t])
    return z


def add_session(z: np.ndarray, mass, out: np.ndarray) -> None:
    """Rules 3-5 on one session's standardised composites ``z`` (themes x names, registered order, NaN = absent): adds
    W_t x the theme's term to ``out`` (names) at the names where theme t is present (two or more of them)."""
    names = z.shape[1]
    for t in range(z.shape[0]):
        names_t = np.flatnonzero(~np.isnan(z[t]))
        if len(names_t) < 2:
            continue
        if t == 0:
            out[names_t] += mass[0] * z[0, names_t]
            continue
        x = np.nan_to_num(z[:t][:, names_t], nan=0.0).T
        e, spanned = residual(z[t, names_t], x)
        if spanned:
            continue
        e = tie_block_means(e, z[t, names_t])
        full = np.zeros(names)
        full[names_t] = e
        keep = np.zeros(names, dtype=bool)
        keep[names_t] = True
        rr = centred_tied_ranks(full, keep)
        out[names_t] += mass[t] * rr[names_t]


def kernel(planes, mass, names: int) -> np.ndarray:
    """The numpy reference of strategy_ic_theme_resid.cpp add_theme_residualised: ``planes`` one dates x names row-major
    plane per theme (registered order) of the raw sums (NaN = no member present), ``mass`` W per theme; returns what the
    kernel adds to a zero ``out`` (dates * names)."""
    planes = np.asarray(planes, dtype=np.float64)
    dates = planes.shape[1] // names
    out = np.zeros(dates * names)
    for d in range(dates):
        total = planes[:, d * names:(d + 1) * names]
        add_session(standardise(np.nan_to_num(total, nan=0.0), ~np.isnan(total)), mass,
                    out[d * names:(d + 1) * names])
    return out


def blend(member: np.ndarray, signals: list[np.ndarray], weights: list[float], signs: list[int], position: list[int],
          themes: int) -> np.ndarray:
    """theme-resid-v1 blend (dates x names): ``position[k]`` is member k's theme position in the registered order;
    W_t sums the theme's positive weights; members with weight 0 or sign 0 add no rank; nonmember cells NaN, as the IC
    composition writes them."""
    dates, names = member.shape
    out = np.where(member == 1, 0.0, np.nan)
    mass = np.zeros(themes)
    for k, w in enumerate(weights):
        if w > 0:
            mass[position[k]] += w
    for d in range(dates):
        total = np.zeros((themes, names))
        present = np.zeros((themes, names), dtype=bool)
        for k, sig in enumerate(signals):
            if not (weights[k] > 0) or signs[k] == 0:
                continue
            row = sig[d]
            r = centred_tied_ranks(row, (member[d] == 1) & np.isfinite(row))
            ok = ~np.isnan(r)
            total[position[k], ok] += signs[k] * weights[k] * r[ok]
            present[position[k], ok] = True
        add_session(standardise(total, present), mass, out[d])
    return out
