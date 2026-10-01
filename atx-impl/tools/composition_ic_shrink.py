#!/usr/bin/env python3
"""Composition rule ``ic-shrink-v1`` (platform v8 R-10, Ruling E-38 slot 49) as pure functions.

Hypothesis: inside each theme, member weights proportional to a shrunk estimate of each member's IC combine better
than equal tier weights (the parent ew-theme-std-v1's within-theme shares).

Registration (lane COMB2, task-R-10-report.md; every constant fixed blind, declared before any cell read):
  1. IC estimate: ic_k = the admission row's ``train_mean`` of member k, the mean of s_k * f_k over the live decisions
     of the fit window the parent fitter already uses (TRAIN; the pooled era decisions under --era). f_k is the factor
     return of the member's neutralised gross-1 centred-rank book, a dispersion-scaled rank IC; s_k = the prior sign
     (+1). No new window, no new read.
  2. Shrinkage, James-Stein toward the theme's equal weight with the fixed intensity INTENSITY = 0.5: per theme t
     (members M_t, n_t of them) shrunk_k = 0.5 * mean_{M_t} ic + 0.5 * ic_k.
  3. Floor: p_k = shrunk_k when above FLOOR = 0, else 0.
  4. Within-theme share a_k = p_k / sum_{M_t} p; a theme with no positive p takes the shrinkage target, the equal share
     1 / n_t. Without a floored member and with a positive theme mean, a_k = 0.5 / n_t + 0.5 * ic_k / sum_{M_t} ic:
     the IC-proportional share shrunk halfway toward the equal share.
  5. w_k = a_k / T (theme share 1/T, T = themes with >= 1 member), then the member cap 1/(2T) of ew-theme-std-v1 rule 4
     (composition_rules.member_cap: excess pro rata to the other themes' uncapped members, to a fixed point).
  6. Standardisation unchanged: the IC runner applies ew-theme-std-v1's per-date rule. The weights file carries
     theme_standardise {rule: ic-shrink-v1, rerank: true, themes, ic_shrink: {intensity, floor, members: {id: {theme,
     ic}}}}; the runner's theme_standardise rule table (strategy_ic_admission.cpp) re-applies the rule to the recorded
     members (strategy_ic_shrink.cpp, atx/engine/combine/group_shrink.hpp) and refuses a weight that differs by more
     than RUNNER_TOLERANCE.
Members: the fitter's admitted non-degenerate members (the parent rule's member set). The arithmetic follows the C++
kernel step for step in member order; the shared fixture atx-impl/tests/fixtures/ic_shrink_v1.json pins both.

Variant ``ic-shrink-aim-v1`` (fix round 1, Ruling E-44: R-10 runs on the last accepted parent whatever its
composition; on an aim parent, ew-theme-std-aim-v1 or ew-theme-aim-v1): step 5 takes the parent's per-member aim gains
g_k (fit_composition_weights.aim_gain, theta .05 as coded, clipped [.05, 1]) inside each theme by the E-27a mechanism,
composition_rules.tier_weights on share_k * g_k: w_k = share_k * g_k / (T * sum_{theme(k)} share * g), each theme keeps
1/T; then composition_rules.member_cap at 1/(2T). Steps 1-4 and 6 unchanged; the block's rule is ic-shrink-aim-v1 and
each member records its gain ({theme, ic, gain}); the runner re-applies the variant with the gains. The shared fixture
atx-impl/tests/fixtures/ic_shrink_aim_v1.json pins both rules.
Parent: scripts/specs/v8/r10.json derives the rule from the parent's composition (std -> ic-shrink-v1, aim ->
ic-shrink-aim-v1).
"""
from __future__ import annotations

import hashlib
import math
from pathlib import Path

import numpy as np

import composition_rules  # this directory: member_cap, StdFit, RuleError, the v2 schema

RULE_ID = "ic-shrink-v1"
AIM_RULE_ID = "ic-shrink-aim-v1"   # the variant on an aim parent (fix round 1, Ruling E-44)
RULES = (RULE_ID, AIM_RULE_ID)
INTENSITY = 0.5            # James-Stein intensity: weight of the theme mean
FLOOR = 0.0                # shrunk ICs below it are floored to it
RUNNER_TOLERANCE = 1e-12   # strategy_ic_shrink.hpp ic_shrink_weight_tolerance: |pinned - rule| per weight
MAX_THEMES = 32            # the runner's theme_standardise bound
IC_SOURCE = ("admission.json candidates[].train_mean: mean of s_k*f_k over the live decisions of the parent fit window "
             "(s_k the prior sign +1; f_k the factor return of the member's neutralised gross-1 centred-rank book)")
RULE_TEXT = (
    "1. ic_k = the admission row's train_mean (the fitter's IC estimate over the parent fit window; no new window, no "
    "new read)",
    "2. per theme, James-Stein shrinkage toward the theme's equal weight with fixed intensity 0.5: shrunk_k = 0.5 * "
    "theme mean ic + 0.5 * ic_k",
    "3. negative shrunk values floored at 0",
    "4. within-theme share = floored / theme sum; a theme with no positive shrunk value takes the equal share 1/n",
    "5. w_k = share_k / T (theme share 1/T), then the member cap 1/(2T) as in ew-theme-std-v1; standardisation "
    "unchanged")
COMPOSITION_TEXT = (
    "ic-shrink-v1: within theme t, shrunk_k=0.5*mean_t(ic)+0.5*ic_k (ic_k = admission train_mean), p_k=max(shrunk_k,0), "
    "share_k=p_k/sum_t p (equal 1/n_t when sum_t p=0), w_k=share_k/T, then member cap 1/(2T) with the excess pro rata "
    "to the other themes' uncapped members (repeated to a fixed point); T=themes with >=1 admitted non-degenerate "
    "member; per date the IC runner applies theme_standardise (ew-theme-std-v1's re-rank) to these weights")
FIT_SERIES = ("TRAIN mean of s_k*f_k per member (the admission train_mean) sets the within-theme shares; diagnostic uses "
              "s_k*f over ALL TRAIN scored decisions, flat decisions 0, without the per-date theme re-rank")
AIM_RULE_TEXT = ("ic-shrink-aim-v1 (fix round 1, Ruling E-44; the E-27a mechanism): the parent's aim gains g_k (theta .05 "
                 "as coded, clipped [.05, 1]) multiply the within-theme shares, renormalised inside the theme so each "
                 "theme keeps 1/T (composition_rules.tier_weights); the member cap 1/(2T) applies after")
AIM_COMPOSITION_TEXT = (
    "ic-shrink-aim-v1: within theme t, shrunk_k=0.5*mean_t(ic)+0.5*ic_k (ic_k = admission train_mean), "
    "p_k=max(shrunk_k,0), share_k=p_k/sum_t p (equal 1/n_t when sum_t p=0), w_k=share_k*g_k/(T*sum_t share*g) "
    "(g_k=theta*sum_{j=0..126}(1-theta)^j*rho_k(j) clipped to [0.05,1], rho_k from TRAIN rank autocorrelation), then "
    "member cap 1/(2T) with the excess pro rata to the other themes' uncapped members (repeated to a fixed point); "
    "T=themes with >=1 admitted non-degenerate member; per date the IC runner applies theme_standardise "
    "(ew-theme-std-v1's re-rank) to these weights")
AIM_FIT_SERIES = ("TRAIN mean of s_k*f_k per member (the admission train_mean) sets the within-theme shares, scaled within "
                  "theme by TRAIN signal-rank persistence gains; diagnostic uses s_k*f over ALL TRAIN scored decisions, "
                  "flat decisions 0, without the per-date theme re-rank")


def module_sha256() -> str:
    return hashlib.sha256(Path(__file__).resolve().read_bytes()).hexdigest()


def _require(condition, message: str, error: type[Exception]) -> None:
    if not condition:
        raise error(message)


def shrunk_shares(themes: list[str], ics: list[float], intensity: float = INTENSITY,
                  floor: float = FLOOR) -> tuple[list[float], list[float], dict[str, float], list[str]]:
    """Steps 2-4 in member order, as the engine kernel group_shrink_shares computes them (same expressions, same
    order): (shrunk, shares, theme means, themes that took the equal share)."""
    count: dict[str, int] = {}
    total: dict[str, float] = {}
    for t, x in zip(themes, ics):
        count[t] = count.get(t, 0) + 1
        total[t] = total.get(t, 0.0) + x
    means = {t: total[t] / count[t] for t in count}
    shrunk, kept, mass = [], [], {t: 0.0 for t in count}
    for t, x in zip(themes, ics):
        s = intensity * means[t] + (1.0 - intensity) * x
        p = s if s > floor else floor
        shrunk.append(s)
        kept.append(p)
        mass[t] += p
    equal = [t for t in count if not mass[t] > 0.0]
    shares = [1.0 / count[t] if t in equal else p / mass[t] for t, p in zip(themes, kept)]
    return shrunk, shares, means, equal


def ic_shrink(ids: list[str], themes: list[str], ics: list[float],
              error: type[Exception] = composition_rules.RuleError,
              gains: list[float] | None = None) -> composition_rules.StdFit:
    """ic-shrink-v1 weights over the members that take part (admitted, non-degenerate), in input order, with the theme
    table, the runner block and provenance (composition_rules.StdFit, attached by ``attach``). With ``gains`` (the
    parent's aim gains, same order) the variant ic-shrink-aim-v1: the shares times the gains, renormalised inside each
    theme by composition_rules.tier_weights (the E-27a mechanism), then the same cap."""
    rule = RULE_ID if gains is None else AIM_RULE_ID
    _require(len(ids) == len(themes) == len(ics) and ids, f"{rule}: members, themes and ICs differ in length or "
                                                          "are empty", error)
    _require(len(set(ids)) == len(ids), f"{rule}: duplicate member", error)
    _require(all(isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x) for x in ics),
             f"{rule}: one finite IC estimate per member", error)
    _require(gains is None or (len(gains) == len(ids) and all(
        isinstance(g, (int, float)) and not isinstance(g, bool) and math.isfinite(g) and g > 0 for g in gains)),
             f"{rule}: one finite positive aim gain per member", error)
    present = list(dict.fromkeys(themes))          # first appearance: the runner's theme order
    _require(len(present) <= MAX_THEMES, f"{rule}: at most {MAX_THEMES} themes", error)
    ics = [float(x) for x in ics]
    shrunk, shares, means, equal = shrunk_shares(themes, ics)
    n_themes = len(present)
    if gains is None:
        uncapped = np.array([a / n_themes for a in shares], dtype=np.float64)
    else:  # E-27a: the aim rules' own function, each theme 1/T before the cap
        gains = [float(g) for g in gains]
        uncapped = composition_rules.tier_weights(themes, [a * g for a, g in zip(shares, gains)])
    cap = 1.0 / (2 * n_themes)
    weights, passes = composition_rules.member_cap(uncapped, themes, cap, error)
    capped = {k for it in passes for k in it["capped"]}
    theme_table = {}
    for t in sorted(present):
        members = [k for k in range(len(ids)) if themes[k] == t]
        theme_table[t] = {
            "members": [ids[k] for k in members], "admitted_count": len(members),
            "theme_weight_uncapped": 1.0 / n_themes, "theme_weight": float(sum(weights[k] for k in members)),
            "mean_ic": means[t], "equal_share": t in equal,
            "ic": {ids[k]: ics[k] for k in members}, "shrunk_ic": {ids[k]: shrunk[k] for k in members},
            "within_theme_shares": {ids[k]: shares[k] for k in members},
            "member_weights": {ids[k]: float(weights[k]) for k in members},
            "capped_members": [ids[k] for k in members if k in capped]}
        if gains is not None:  # the fitter's aim summary reads aim_theme_weight (as for ew-theme-std-aim-v1)
            theme_table[t].update(aim_gains={ids[k]: gains[k] for k in members},
                                  aim_theme_weight=theme_table[t]["theme_weight"])
    members_block = {ids[k]: {"theme": themes[k], "ic": ics[k]} for k in range(len(ids))}
    if gains is not None:
        for k in range(len(ids)):
            members_block[ids[k]]["gain"] = gains[k]
    block = {"rule": rule, "rerank": True,
             "themes": {ids[k]: themes[k] for k in range(len(ids)) if weights[k] > 0},
             "ic_shrink": {"intensity": INTENSITY, "floor": FLOOR, "members": members_block}}
    provenance = {
        "rule": rule,
        "registration": "task-R-10-report.md (platform v8 R-10, lane COMB2; Ruling E-38 slot 49), declared before any "
                        "cell read" + ("" if gains is None else "; the aim variant: fix round 1, Ruling E-44 (E-27a)"),
        "rule_text": list(RULE_TEXT) + ([] if gains is None else [AIM_RULE_TEXT]), "ic_source": IC_SOURCE,
        "intensity": INTENSITY, "floor": FLOOR, "theme_count": n_themes, "cap": cap,
        "cap_tolerance_relative": composition_rules.CAP_TOLERANCE, "runner_tolerance_absolute": RUNNER_TOLERANCE,
        "member_ic": dict(zip(ids, ics)), "theme_mean_ic": {t: means[t] for t in sorted(present)},
        "shrunk_ic": dict(zip(ids, shrunk)), "within_theme_shares": dict(zip(ids, shares)),
        "floored_members": [ids[k] for k in range(len(ids)) if not shrunk[k] > FLOOR],
        "equal_share_themes": sorted(equal),
        "weights_before_cap": {ids[k]: float(uncapped[k]) for k in range(len(ids))},
        "cap_iterations": [{"capped": [ids[k] for k in it["capped"]], "excess_by_theme": it["excess_by_theme"]}
                           for it in passes],
        "capped_members": [ids[k] for k in sorted(capped)],
        "runner": (f"atx-equity-strategy-ic theme_standardise {{rule: {rule}, rerank: true, themes, ic_shrink}}: "
                   f"the ew-theme-std-v1 per-date re-rank, unchanged; the rule table re-applies {rule} to "
                   "ic_shrink.members and refuses a weight off by more than runner_tolerance_absolute "
                   "(strategy_ic_admission.cpp, strategy_ic_shrink.cpp)"),
        "module_sha256": module_sha256()}
    if gains is None:
        return composition_rules.StdFit(weights=weights, theme_table=theme_table, block=block, provenance=provenance,
                                        text=COMPOSITION_TEXT, fit_series=FIT_SERIES, ids=list(ids))
    provenance.update(aim_gains=dict(zip(ids, gains)),
                      aim_rule=("the gains multiply the within-theme shares inside each theme (renormalised within the "
                                "theme by composition_rules.tier_weights, each theme 1/T before the cap); the runner "
                                "block names ic-shrink-aim-v1 and records each member's gain"))
    return composition_rules.StdFit(weights=weights, theme_table=theme_table, block=block, provenance=provenance,
                                    text=AIM_COMPOSITION_TEXT, fit_series=AIM_FIT_SERIES, ids=list(ids))


def attach(document: dict, fit: composition_rules.StdFit) -> dict:
    """The weights document of the rule: schema v2, the top-level theme_standardise block, provenance.ic_shrink."""
    document["schema"] = composition_rules.WEIGHTS_SCHEMA_V2
    document["theme_standardise"] = dict(fit.block)
    document["provenance"]["ic_shrink"] = fit.provenance
    return document
