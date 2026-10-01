#!/usr/bin/env python3
"""Composition rule ``ew-theme-std-v1`` (platform v8 R-1: S-1 + S-4 + tier re-grade) as pure functions.

Registration (task-R-1-brief.md, binding; declared before any read):
  1. Per date and theme, take the weighted mean of the theme's signed member ranks. Within-theme weights are
     proportional to the tier score in the registry.
  2. Re-rank that theme composite across names with at least one present member (centred tied rank in [-.5, +.5]).
  3. Blend = sum over themes of (1 / T) x re-ranked composite. Missing stays neutral; no redistribution inside a theme.
  4. Member cap: no member's weight above 1 / (2T); the excess goes pro rata to the other themes.
  5. Tier re-grades from the v8 literature: res_mom_12_1 B+ to B-; ear B+ to C+; sue C+ stays; ins_opp B- to C+. No
     other tier changes. No tier comes from a TRAIN statistic.

Split of work. This module fits rules 1 (the within-theme shares), 3 (1/T), 4 and 5 into one weight per member,
w_k = (1/T) * score_k / sum_{theme} score, then capped. The IC runner applies rules 1-3 per date from the weights
file's ``theme_standardise`` block (atx-impl/src/strategy_ic_composition.cpp, IcThemeRule::standardise): per theme the
sum of its present members' w_k * s_k * rank_k (missing members neutral; its order is the weighted mean's, whose
divisor W_theme is one constant per theme), re-ranked over the names with a present member and added as
W_theme * rank, W_theme = the sum of the theme's weights. So the cap moves theme mass: a capped member's theme
enters with less than 1/T and the other themes with more.

Tiers (rule 5 table ``TIER_REGRADES_V8``, applied to the source tier): the alpha registry
``atx-impl/strategies/alphas/registry.json`` (``atx.alpha-registry/v1``, task A-1: ``alphas[].tier`` and
``tier_scores``) when the file exists, per id, else the ``tier`` field the fitter read from the library / recipe; the
score table is the registry's ``tier_scores`` or, without a registry, the declared ``TIER_SCORES_DECLARED`` (the A-1
schema values). A re-grade whose source tier is neither its declared from- nor to-grade is refused.

Cap (rule 4, ``member_cap``): repeat until no member exceeds cap * (1 + CAP_TOLERANCE): every member above the cap is
set to the cap and frozen; each theme's total excess is added to the unfrozen members of the OTHER themes pro rata to
their current weights. Infeasible (no other theme can absorb an excess) is refused.

Identity cell (R-1 step 3): ``identity-weights`` grafts ``theme_standardise`` {rerank: false} onto an accepted
ew-theme-v1 weights file (schema -> v2, weights and signs untouched), so the runner's blend must equal the accepted
ew-theme-v1 blend byte for byte: re-rank off, cap off and tier shares off (equal within-theme weights).

``ew-theme-std-aim-v1`` (platform v8 R-3 on an R-1 parent, Ruling E-27): the persistence gains g_k of ew-theme-aim-v1
(theta .05, as coded in fit_composition_weights.aim_gain, clipped to [.05, 1]) multiply the tier weights inside each
theme, renormalised inside the theme so each theme keeps 1/T: w_k = (1/T) * score_k * g_k / sum_{theme(k)} score * g;
then the member cap 1/(2T) as in rule 4. Fitter only: the weights file is an ew-theme-std-v1 file (the same
``theme_standardise`` block, rule "ew-theme-std-v1", which the IC runner applies unchanged) whose member weights carry
the gains; provenance.rule names ew-theme-std-aim-v1 and provenance.std records the gains. Gains of 1 give the
ew-theme-std-v1 weights bit for bit.

``ew-theme-aim-v2`` (R-3 on an ew-theme-v1 parent, Rulings E-27a and E-27b): the same rule on the parent's equal
member weights, ``ew_theme_aim_v2``: w_k = (1/T) * g_k / sum_{theme(k)} g, each theme 1/T, then the member cap 1/(2T).
Both v8 aim rules are ``theme_gain_weights`` (rules 1, 3 and 4 on member scores times gains); an ew-theme-aim-v2
weights file stays schema v1 (a plain weighted blend: no theme block). The v5 rule ew-theme-aim-v1 (gains normalised
globally) stays in fit_composition_weights.py, unchanged, and is refused in a v8 spec (research_cycle.py).
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import hashlib
import json
import math
from pathlib import Path
import sys

import numpy as np

STD_RULE_ID = "ew-theme-std-v1"
STD_AIM_RULE_ID = "ew-theme-std-aim-v1"   # R-3 on the R-1 rule (Ruling E-27); the runner block stays STD_RULE_ID
STD_RULES = (STD_RULE_ID, STD_AIM_RULE_ID)
WEIGHTS_SCHEMA_V1 = "atx.dsl-composition-weights/v1"
WEIGHTS_SCHEMA_V2 = "atx.dsl-composition-weights/v2"
REGISTRY_SCHEMA = "atx.alpha-registry/v1"
REGISTRY_PATH = Path(__file__).resolve().parents[1] / "strategies" / "alphas" / "registry.json"
REGISTRY_LIMIT = 4 << 20
# Declared tier scores (task A-1 registry schema; library-v7-draft section 0 plus A- = .9, code review S-4).
TIER_SCORES_DECLARED = {"A": 1.0, "A-": 0.9, "B+": 0.8, "B": 0.7, "B-": 0.55, "C+": 0.4}
# Rule 5, verbatim: (id, from, to). "sue C+ stays" is declared as a no-op row.
TIER_REGRADES_V8 = (("res_mom_12_1", "B+", "B-"), ("ear", "B+", "C+"), ("sue", "C+", "C+"), ("ins_opp", "B-", "C+"))
CAP_TOLERANCE = 1e-12  # relative: a member within 1e-12 of the cap is at the cap (no rounding-noise redistribution)
RULE_TEXT = (
    "1. per date and theme the weighted mean of the theme's signed member ranks; within-theme weights proportional to "
    "the tier score in the registry",
    "2. re-rank that theme composite across names with at least one present member (centred tied rank in [-.5, +.5])",
    "3. blend = sum over themes of (1 / T) x re-ranked composite; missing stays neutral; no redistribution inside a "
    "theme",
    "4. member cap: no member's weight above 1 / (2T); the excess goes pro rata to the other themes",
    "5. tier re-grades: res_mom_12_1 B+ to B-; ear B+ to C+; sue C+ stays; ins_opp B- to C+; no other tier changes; "
    "no tier comes from a TRAIN statistic")
COMPOSITION_TEXT = (
    "ew-theme-std-v1: w_k=(1/T)*score_k/sum_{theme(k)} score (tier scores after the v8 re-grades), then member cap "
    "1/(2T) with the excess pro rata to the other themes' uncapped members (repeated to a fixed point); T=themes with "
    ">=1 admitted non-degenerate member; per date the IC runner re-ranks each theme's sum of present w_k*s_k*rank_k "
    "over names with a present member and adds W_theme*rank, W_theme=sum of the theme's w_k (theme_standardise); no "
    "mean or covariance estimation")
FIT_SERIES = ("none (prior tier-score theme weights; no TRAIN statistic); diagnostic uses s_k*f over ALL TRAIN scored "
              "decisions, flat decisions 0, without the per-date theme re-rank")
AIM_RULE_TEXT = ("R-3 (Ruling E-27): the persistence gains g_k (theta .05 as coded, clipped [.05, 1]) multiply the tier "
                 "weights inside each theme, renormalised inside the theme so each theme keeps 1/T; the member cap "
                 "1/(2T) applies after")
STD_AIM_TEXT = (
    "ew-theme-std-aim-v1: w_k=(1/T)*score_k*g_k/sum_{theme(k)} score*g (tier scores after the v8 re-grades; "
    "g_k=theta*sum_{j=0..126}(1-theta)^j*rho_k(j) clipped to [0.05,1], rho_k from TRAIN rank autocorrelation), then "
    "member cap 1/(2T) with the excess pro rata to the other themes' uncapped members (repeated to a fixed point); "
    "T=themes with >=1 admitted non-degenerate member; per date the IC runner applies theme_standardise "
    "(ew-theme-std-v1) to these weights; no mean or covariance estimation")
STD_AIM_FIT_SERIES = ("none (prior tier-score theme weights scaled within theme by TRAIN signal-rank persistence "
                      "gains); diagnostic uses s_k*f over ALL TRAIN scored decisions, flat decisions 0, without the "
                      "per-date theme re-rank")
AIM_V2_RULE_ID = "ew-theme-aim-v2"        # R-3 on an ew-theme-v1 parent (Rulings E-27a, E-27b)
AIM_V2_TEXT = (
    "ew-theme-aim-v2 (Rulings E-27a, E-27b): w_k=(1/T)*g_k/sum_{theme(k)} g (the ew-theme-v1 member weights times the "
    "gains, "
    "renormalised inside the theme so each theme keeps 1/T; g_k=theta*sum_{j=0..126}(1-theta)^j*rho_k(j) clipped to "
    "[0.05,1], rho_k from TRAIN rank autocorrelation), then member cap 1/(2T) with the excess pro rata to the other "
    "themes' uncapped members (repeated to a fixed point); T=themes with >=1 admitted non-degenerate member; no mean "
    "or covariance estimation")
AIM_V2_FIT_SERIES = ("none (equal theme weights scaled within theme by TRAIN signal-rank persistence gains, member cap "
                     "1/(2T)); diagnostic uses s_k*f over ALL TRAIN scored decisions, flat decisions 0")


class RuleError(ValueError):
    """A loud refusal of the rule's inputs (the fitter passes its own FitError instead)."""


def _require(condition, message: str, error: type[Exception]) -> None:
    if not condition:
        raise error(message)


def module_sha256() -> str:
    return hashlib.sha256(Path(__file__).resolve().read_bytes()).hexdigest()


# ---------------------------------------------------------------- tiers
def load_registry(path: Path | None = None, error: type[Exception] = RuleError) -> dict | None:
    """The alpha registry's tiers and score table, or None when the file does not exist."""
    path = REGISTRY_PATH if path is None else Path(path)
    if not path.is_file():
        return None
    raw = path.read_bytes()
    _require(len(raw) <= REGISTRY_LIMIT, f"alpha registry {path}: over {REGISTRY_LIMIT} bytes", error)
    j = json.loads(raw.decode("utf-8"))
    scores, alphas = (j.get("tier_scores"), j.get("alphas")) if isinstance(j, dict) else (None, None)
    _require(isinstance(j, dict) and j.get("schema") == REGISTRY_SCHEMA and isinstance(scores, dict) and
             isinstance(alphas, list), f"alpha registry {path}: not an {REGISTRY_SCHEMA} document", error)
    _require(all(isinstance(g, str) and isinstance(s, (int, float)) and not isinstance(s, bool) and math.isfinite(s)
                 and s > 0 for g, s in scores.items()), f"alpha registry {path}: tier_scores must be positive", error)
    tiers = {}
    for row in alphas:
        _require(isinstance(row, dict) and isinstance(row.get("id"), str) and isinstance(row.get("tier"), str) and
                 row["id"] not in tiers, f"alpha registry {path}: alphas[] needs unique id and tier", error)
        tiers[row["id"]] = row["tier"]
    return {"path": str(path), "sha256": hashlib.sha256(raw).hexdigest(),
            "tier_scores": {g: float(s) for g, s in scores.items()}, "tiers": tiers}


def resolve_tiers(ids: list[str], prior_tiers: list, registry: dict | None) -> tuple[list[str], dict[str, str]]:
    """Source tier per member: the registry's when it lists the id, else the library / recipe tier."""
    tiers, source = [], {}
    for cid, prior in zip(ids, prior_tiers):
        if registry is not None and cid in registry["tiers"]:
            tiers.append(registry["tiers"][cid])
            source[cid] = "registry"
        else:
            tiers.append(prior)
            source[cid] = "library-recipe"
    return tiers, source


def apply_regrades(ids: list[str], tiers: list[str], table=TIER_REGRADES_V8,
                   error: type[Exception] = RuleError) -> tuple[list[str], list[dict]]:
    """Rule 5: each declared (id, from, to) re-grade applied to that member's source tier."""
    out, applied = list(tiers), []
    for cid, before, after in table:
        if cid not in ids:
            applied.append({"id": cid, "from": before, "to": after, "status": "not-a-member"})
            continue
        k = ids.index(cid)
        source = out[k]
        _require(source in (before, after), f"tier re-grade of {cid}: source tier {source} is neither the declared "
                                            f"{before} nor {after}", error)
        status = "declared-unchanged" if before == after else ("applied" if source == before else "already-at-target")
        out[k] = after
        applied.append({"id": cid, "from": before, "to": after, "source_tier": source, "status": status})
    return out, applied


# ---------------------------------------------------------------- weights
def tier_weights(themes: list[str], scores: list[float]) -> np.ndarray:
    """Rules 1 and 3: w_k = score_k / (T * sum of its theme's scores); each theme sums to 1/T."""
    present = sorted(set(themes))
    total = {t: 0.0 for t in present}
    for t, s in zip(themes, scores):
        total[t] += s
    return np.array([s / (len(present) * total[t]) for t, s in zip(themes, scores)], dtype=np.float64)


def member_cap(weights: np.ndarray, themes: list[str], cap: float,
               error: type[Exception] = RuleError) -> tuple[np.ndarray, list[dict]]:
    """Rule 4: no member above ``cap``; each theme's excess goes pro rata (by current weight) to the uncapped members of
    the other themes, repeated until none exceeds cap * (1 + CAP_TOLERANCE). Returns (weights, iterations)."""
    w = np.array(weights, dtype=np.float64)
    frozen = np.zeros(len(w), dtype=bool)
    iterations = []
    for _ in range(len(w) + 1):
        over = [k for k in range(len(w)) if not frozen[k] and w[k] > cap * (1.0 + CAP_TOLERANCE)]
        if not over:
            return w, iterations
        excess: dict[str, float] = {}
        for k in over:
            excess[themes[k]] = excess.get(themes[k], 0.0) + (w[k] - cap)
            w[k], frozen[k] = cap, True
        base = w.copy()
        for theme in sorted(excess):
            receivers = [j for j in range(len(w)) if not frozen[j] and themes[j] != theme]
            mass = float(sum(base[j] for j in receivers))
            _require(mass > 0, f"member cap 1/(2T)={cap}: no other theme can take the excess of theme {theme}", error)
            for j in receivers:
                w[j] += excess[theme] * base[j] / mass
        iterations.append({"capped": sorted(over), "excess_by_theme": {t: excess[t] for t in sorted(excess)}})
    raise error(f"member cap 1/(2T)={cap}: no fixed point")  # unreachable: every pass freezes a member


def theme_gain_weights(themes: list[str], scores: list[float],
                       error: type[Exception] = RuleError) -> tuple[np.ndarray, np.ndarray, list[dict], float]:
    """Rules 1, 3 and 4 on member scores: ``tier_weights`` (w_k = score_k / (T * sum of its theme's scores), each theme
    1/T), then ``member_cap`` at 1/(2T). The v8 aim rules (E-27, E-27a/b) pass score_k = the parent's within-theme
    score times the gain g_k. Returns (weights, uncapped weights, cap passes, cap)."""
    uncapped = tier_weights(themes, scores)
    cap = 1.0 / (2 * len(set(themes)))
    weights, passes = member_cap(uncapped, themes, cap, error)
    return weights, uncapped, passes, cap


def check_gains(gains, n: int, rule: str, error: type[Exception]) -> None:
    """One finite positive aim gain per member (``n`` members)."""
    _require(gains is not None and len(gains) == n and all(
        isinstance(g, (int, float)) and not isinstance(g, bool) and math.isfinite(g) and g > 0 for g in gains),
             f"{rule}: one finite positive aim gain per member", error)


def ew_theme_aim_v2(ids: list[str], themes: list[str], gains: list[float],
                    error: type[Exception] = RuleError) -> tuple[np.ndarray, dict]:
    """ew-theme-aim-v2 (Rulings E-27a, E-27b) over the members that take part, in input order: ``theme_gain_weights``
    with every member's score 1 times its gain (w_k = (1/T) g_k / sum_{theme(k)} g, then the member cap 1/(2T)).
    Returns (weights, theme table {admitted_count, nominal_theme_weight 1/T, aim_theme_weight (after the cap),
    capped_members})."""
    _require(len(ids) == len(themes) and ids, f"{AIM_V2_RULE_ID}: members and themes differ in length or are empty",
             error)
    check_gains(gains, len(ids), AIM_V2_RULE_ID, error)
    weights, _, passes, _ = theme_gain_weights(themes, [float(g) for g in gains], error)
    capped = {k for it in passes for k in it["capped"]}
    present = sorted(set(themes))
    table = {}
    for t in present:
        members = [k for k in range(len(ids)) if themes[k] == t]
        table[t] = {"admitted_count": len(members), "nominal_theme_weight": 1.0 / len(present),
                    "aim_theme_weight": float(sum(weights[k] for k in members)),
                    "capped_members": [ids[k] for k in members if k in capped]}
    return weights, table


@dataclass
class StdFit:
    """The fitted rule: per-member weights (fitter input order), the theme table, the runner block and provenance."""
    weights: np.ndarray
    theme_table: dict
    block: dict
    provenance: dict
    text: str = COMPOSITION_TEXT
    fit_series: str = FIT_SERIES
    ids: list = field(default_factory=list)


def ew_theme_std(ids: list[str], themes: list[str], prior_tiers: list, registry_path: Path | None = None,
                 error: type[Exception] = RuleError, gains: list[float] | None = None) -> StdFit:
    """ew-theme-std-v1 weights over the members that take part (admitted, non-degenerate), in input order; with
    ``gains`` (the members' aim gains, same order) ew-theme-std-aim-v1: each tier score times its gain inside the
    theme (each theme still 1/T before the cap). Gains of 1 give the ew-theme-std-v1 weights bit for bit."""
    _require(len(ids) == len(themes) == len(prior_tiers) and ids, "ew-theme-std-v1: members, themes and tiers differ "
                                                                  "in length or are empty", error)
    if gains is not None:
        check_gains(gains, len(ids), STD_AIM_RULE_ID, error)
    registry = load_registry(registry_path, error)
    source_tiers, tier_source = resolve_tiers(ids, prior_tiers, registry)
    tiers, regrades = apply_regrades(ids, source_tiers, error=error)
    table = registry["tier_scores"] if registry is not None else TIER_SCORES_DECLARED
    unscored = [f"{i}:{t}" for i, t in zip(ids, tiers) if t not in table]
    _require(not unscored, f"ew-theme-std-v1: tiers without a declared score {sorted(table)}: {unscored}", error)
    scores = [float(table[t]) for t in tiers]
    weights, uncapped, passes, cap = theme_gain_weights(
        themes, scores if gains is None else [s * float(g) for s, g in zip(scores, gains)], error)
    present = sorted(set(themes))
    capped = {k for it in passes for k in it["capped"]}
    iterations = [{"capped": [ids[k] for k in it["capped"]], "excess_by_theme": it["excess_by_theme"]} for it in passes]
    theme_table = {}
    for t in present:
        members = [k for k in range(len(ids)) if themes[k] == t]
        mass = float(sum(weights[k] for k in members))
        theme_table[t] = {
            "members": [ids[k] for k in members], "admitted_count": len(members),
            "theme_weight_uncapped": 1.0 / len(present), "theme_weight": mass,
            "tiers": {ids[k]: tiers[k] for k in members}, "tier_scores": {ids[k]: scores[k] for k in members},
            "within_theme_weights": {ids[k]: float(weights[k]) / mass for k in members},
            "member_weights": {ids[k]: float(weights[k]) for k in members},
            "capped_members": [ids[k] for k in members if k in capped]}
        if gains is not None:                        # R-3: the fitter's summary reads aim_theme_weight
            theme_table[t].update(aim_gains={ids[k]: float(gains[k]) for k in members}, aim_theme_weight=mass)
    block = {"rule": STD_RULE_ID, "rerank": True, "themes": {ids[k]: themes[k] for k in range(len(ids))
                                                             if weights[k] > 0}}
    provenance = {
        "rule": STD_RULE_ID if gains is None else STD_AIM_RULE_ID,
        "registration": "task-R-1-brief.md (platform v8 R-1), declared before any read" + (
            "" if gains is None else "; R-3 aim gains on top: Ruling E-27 (platform v8 A2 follow-up)"),
        "rule_text": list(RULE_TEXT) + ([] if gains is None else [AIM_RULE_TEXT]),
        "tier_source": ("registry" if registry is not None else "library-recipe tier field (registry absent)"),
        "registry_path": None if registry is None else registry["path"],
        "registry_sha256": None if registry is None else registry["sha256"],
        "member_tier_source": tier_source, "source_tiers": dict(zip(ids, source_tiers)),
        "tier_regrades": regrades, "member_tiers": dict(zip(ids, tiers)),
        "tier_scores": dict(table), "tier_scores_source": "registry" if registry is not None else "declared",
        "member_scores": dict(zip(ids, scores)),
        "theme_count": len(present), "cap": cap, "cap_tolerance_relative": CAP_TOLERANCE,
        "cap_rule": ("no member above 1/(2T); each pass sets every member above the cap to the cap (frozen) and adds "
                     "its theme's excess to the unfrozen members of the other themes pro rata to their current "
                     "weights, until none exceeds cap*(1+tolerance)"),
        "cap_iterations": iterations, "capped_members": [ids[k] for k in sorted(capped)],
        "weights_before_cap": {ids[k]: float(uncapped[k]) for k in range(len(ids))},
        "runner": ("atx-equity-strategy-ic theme_standardise {rule, rerank: true, themes}: per date and theme the sum "
                   "of present w_k*s_k*rank_k re-ranked (centred tied) over names with a present member, times "
                   "W_theme=sum of the theme's w_k (strategy_ic_composition.cpp IcThemeRule::standardise)"),
        "module_sha256": module_sha256()}
    if gains is None:
        return StdFit(weights=weights, theme_table=theme_table, block=block, provenance=provenance, ids=list(ids))
    provenance.update(aim_gains=dict(zip(ids, map(float, gains))),
                      aim_rule=("the gains multiply the tier scores inside each theme (renormalised within the theme, "
                                "each theme 1/T before the cap); the runner block stays ew-theme-std-v1"))
    return StdFit(weights=weights, theme_table=theme_table, block=block, provenance=provenance, ids=list(ids),
                  text=STD_AIM_TEXT, fit_series=STD_AIM_FIT_SERIES)


def attach_std(document: dict, fit: StdFit) -> dict:
    """The weights document of the rule: schema v2, the top-level theme_standardise block, provenance.std."""
    document["schema"] = WEIGHTS_SCHEMA_V2
    document["theme_standardise"] = dict(fit.block)
    document["provenance"]["std"] = fit.provenance
    return document


# ---------------------------------------------------------------- identity cell
def identity_document(accepted: dict) -> dict:
    """The R-1 identity weights: an accepted ew-theme-v1 document (schema v1) with theme_standardise {rerank: false}
    grafted on; weights and signs untouched, themes from its provenance rows."""
    _require(isinstance(accepted, dict) and accepted.get("schema") == WEIGHTS_SCHEMA_V1 and
             accepted.get("provenance", {}).get("rule") == "ew-theme-v1" and "theme_redistribution" not in accepted,
             "identity weights: the source must be an ew-theme-v1 weights document (schema v1)", RuleError)
    rows = {r["id"]: r for r in accepted["provenance"]["candidates"]}
    weighted = [cid for cid, w in accepted["weights"].items() if w > 0]
    _require(all(cid in rows and isinstance(rows[cid].get("theme"), str) for cid in weighted),
             "identity weights: every weighted member needs its theme in provenance.candidates", RuleError)
    out = json.loads(json.dumps(accepted))
    out["schema"] = WEIGHTS_SCHEMA_V2
    out["theme_standardise"] = {"rule": STD_RULE_ID, "rerank": False, "themes": {c: rows[c]["theme"] for c in weighted}}
    out["provenance"]["std_identity"] = {
        "rule": STD_RULE_ID, "rerank": False, "cap": "off (the source weights)", "tier_shares": "off (the source "
        "weights)", "claim": "the IC runner's blend equals the source ew-theme-v1 blend byte for byte",
        "module_sha256": module_sha256()}
    return out


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="ew-theme-std-v1 helpers (platform v8 R-1)")
    sub = p.add_subparsers(dest="verb", required=True)
    ident = sub.add_parser("identity-weights", help="graft theme_standardise {rerank: false} onto ew-theme-v1 weights")
    ident.add_argument("--weights", type=Path, required=True, help="accepted ew-theme-v1 composition_weights.json")
    ident.add_argument("--weights-sha256", required=True)
    ident.add_argument("--out", type=Path, required=True, help="new weights file (never overwritten)")
    args = p.parse_args(argv)
    try:
        raw = args.weights.read_bytes()
        _require(hashlib.sha256(raw).hexdigest() == args.weights_sha256, "--weights SHA-256 differs from its pin",
                 RuleError)
        _require(not args.out.exists(), f"--out exists; refusing overwrite: {args.out}", RuleError)
        doc = identity_document(json.loads(raw.decode("utf-8")))
    except RuleError as exc:
        print(f"composition_rules: {exc}", file=sys.stderr)
        return 1
    data = (json.dumps(doc, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")
    args.out.write_bytes(data)
    print(json.dumps({"out": str(args.out), "sha256": hashlib.sha256(data).hexdigest(), "rule": STD_RULE_ID,
                      "rerank": False, "source_sha256": args.weights_sha256}, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
