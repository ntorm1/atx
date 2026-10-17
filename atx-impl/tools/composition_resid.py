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
re-orthogonalisation pass, atx/engine/combine/group_residualise.hpp) applies rules 2-5 per session; the C++ is the rule's
only producer (P9 lane D3W-PB, ruling D2-3: the independent numpy reference of the runner's rule, ``blend`` and its
plane-level form ``kernel``, moved to test_composition_resid.py, where both sides meet the same exact values). No TRAIN
statistic, window or read is added: the
fitter's inputs are the parent's.
"""
from __future__ import annotations

import argparse
import json

import composition_engine_fit  # this directory: compare_values, ruling P12's comparison (ruling D2-R2)
import composition_rules  # this directory: module_fingerprint (ruling D2-2)

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
# Finding R6B-O-5: the cell's re-fit against its parent cell's weights file (--theme-resid-parent).
PARENT_CHECK = ("the re-fit minus theme_residualise and provenance.resid equals the parent cell's weights file apart "
                "from provenance.script_sha256 and provenance.admission_sha256, and the re-fit's admission.json equals "
                "the parent's apart from inputs.script_sha256")
# P9 lane D2 (CM-5): the fitter's identity in its outputs is producer_fingerprint (script_sha256 before CM-5); either
# key, on either side, is the fitter identity the check allows to differ (PARENT_CHECK's text is the v8 rule's).
FITTER_IDENTITY_KEYS = ("script_sha256", "producer_fingerprint")
# P9 ruling D2-R2: a fit under --engine-fit carries provenance.engine_fit (composition_engine_fit.PROVENANCE_KEY) and
# states in its ic-shrink block that the ICs are the C++ fit's (ruling D2-S4). Neither is a value the re-fit
# reproduces, so the check sets them aside; whenever either fit ran --engine-fit every other value is compared under
# ruling P12 (composition_engine_fit.compare_values) and provenance.resid records this text beside PARENT_CHECK.
ENGINE_FIT_KEY = composition_engine_fit.PROVENANCE_KEY
ENGINE_FIT_ANNOTATIONS = {"ic_shrink": ("ic_source", "ic_admission_max_abs_diff")}
PARENT_CHECK_ENGINE_FIT = (
    "ruling D2-R2: the parent or the re-fit ran --engine-fit (C++ fit values), so provenance.engine_fit and the "
    "ic-shrink IC annotations (ic_source, ic_admission_max_abs_diff) are set aside and every other value of the two "
    "files agrees to ruling P12 (floats within 1e-12 x max(1, |x|); strings, integers, booleans, keys, lengths and "
    "orders exactly); the admission tables are compared exactly, as without the flag")
# CM-5 / ruling D2-2: a rule block of a fit made before P9 lane D2 names its code by module_sha256 (the whole file).
PRE_D2_KEY = "module_sha256"


class ResidError(ValueError):
    """A loud refusal of the rule's inputs (the fitter passes its own FitError instead)."""


def _require(condition, message: str, error: type[Exception]) -> None:
    if not condition:
        raise error(message)


# The writers of the theme_residualise block and provenance.resid (ruling D2-2; apply adds the parent check's record)
PRODUCERS = ("attach", "apply")


def producer_fingerprint() -> str:
    """provenance.resid's code identity (ruling D2-2: composition_rules.module_fingerprint; module_sha256, the whole
    file's SHA-256, before P9 lane D2)."""
    return composition_rules.module_fingerprint(__file__, PRODUCERS)


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
                   "(absent = 0) over its present names, averaged inside each tie block of that composite first "
                   "(Ruling PM4-12); the first theme adds W_1 x its composite "
                   "(strategy_ic_theme_resid.cpp, IcThemeRule::residualise)"),
        "diagnostic": ("provenance.blend_in_sample_TRAIN_diagnostic excludes the per-session re-rank and "
                       "residualisation"),
        "producer_fingerprint": producer_fingerprint()}
    return document


def _text(value) -> str:
    return json.dumps(value, sort_keys=True, allow_nan=False)


def _differing(a: dict, b: dict, prefix: str = "") -> list[str]:
    """The keys of ``a`` and ``b`` present in only one of them or with different canonical JSON values."""
    return [f"{prefix}{k}" for k in sorted(set(a) | set(b))
            if k not in a or k not in b or _text(a[k]) != _text(b[k])]


def parent_check(document: dict, admission: bytes, parent: bytes, parent_admission: bytes,
                 error: type[Exception] = ResidError, engine_fit: bool = False) -> bool:
    """Finding R6B-O-5: R-11's weights are a re-fit of its parent cell's argv plus the block, so ``document`` (the
    re-fit, block and provenance.resid attached) must be the parent's weights file ``parent`` again (PARENT_CHECK): the
    two differ only in the fitter identity (provenance.producer_fingerprint, or provenance.script_sha256 in a parent
    fitted before CM-5) and provenance.admission_sha256, which may differ only because the admission table records the
    fitter identity (``admission`` and ``parent_admission`` equal apart from it). Anything else -- fitter-affecting code
    or an input changed between the parent's fit and this one -- is refused, naming the keys that differ. A parent that
    already carries the block is refused, and so is a parent fitted before P9 lane D2 (its rule blocks name their code
    by module_sha256: no re-fit can equal it; refit the parent). Compared as canonical JSON (sorted keys: the fitter's
    canonical_bytes up to whitespace).

    Ruling D2-R2: ``engine_fit`` (the re-fit runs --engine-fit) or a parent carrying provenance.engine_fit puts the
    check in engine-fit mode: provenance.engine_fit and the ic-shrink IC annotations (ENGINE_FIT_ANNOTATIONS) are set
    aside and the rest is compared under ruling P12 (composition_engine_fit.compare_values: floats within 1e-12 x
    max(1, |x|), everything else exactly; P9 D3W, ruling D2-9: strict, so an integer against an equal float is a
    difference too), so C++ values within P12 of the Python's pass and a change beyond it is refused, naming the
    values. Returns whether the check ran in engine-fit mode."""
    try:
        mother, child_adm, mother_adm = json.loads(parent), json.loads(admission), json.loads(parent_admission)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise error(f"{RULE_ID}: --theme-resid-parent: JSON parse: {exc}") from exc
    _require(isinstance(mother, dict) and isinstance(mother.get("provenance"), dict) and isinstance(child_adm, dict)
             and isinstance(mother_adm, dict), f"{RULE_ID}: --theme-resid-parent: not a weights file with provenance",
             error)
    _require(BLOCK not in mother and "resid" not in mother["provenance"],
             f"{RULE_ID}: --theme-resid-parent already carries {BLOCK}: the parent cell is not the rule's parent", error)
    stale = sorted(k for k, v in mother["provenance"].items() if isinstance(v, dict) and PRE_D2_KEY in v)
    _require(not stale, f"{RULE_ID}: --theme-resid-parent was fitted before P9 lane D2: its provenance blocks {stale} "
                        f"name their code by {PRE_D2_KEY}, which CM-5 / ruling D2-2 replaced by producer_fingerprint, "
                        "so no re-fit by this fitter can equal it; refit the parent cell with this fitter, then re-run "
                        "this cell against the new parent file", error)
    parent_engine = ENGINE_FIT_KEY in mother["provenance"]
    mode = engine_fit or parent_engine
    child = json.loads(_text(document))
    child.pop(BLOCK, None)
    child["provenance"].pop("resid", None)
    for key in FITTER_IDENTITY_KEYS:  # CM-5: the fitter identity under either name
        for doc in (child, mother):
            doc["provenance"].pop(key, None)
        for doc in (child_adm, mother_adm):
            if isinstance(doc.get("inputs"), dict):
                doc["inputs"].pop(key, None)
    adm_differ = _differing(child_adm, mother_adm)
    _require(not adm_differ, f"{RULE_ID}: --theme-resid-parent: the re-fit's admission.json differs from the parent's "
                             f"beyond inputs.script_sha256 (keys {adm_differ})", error)
    for doc in (child, mother):
        doc["provenance"].pop("admission_sha256", None)
        doc["provenance"].pop(ENGINE_FIT_KEY, None)
        for block, keys in ENGINE_FIT_ANNOTATIONS.items():
            if mode and isinstance(doc["provenance"].get(block), dict):
                for key in keys:
                    doc["provenance"][block].pop(key, None)
    if not mode:
        differ = sorted([k for k in _differing(child, mother) if k != "provenance"] +
                        _differing(child["provenance"], mother["provenance"], "provenance."))
        _require(not differ, f"{RULE_ID}: --theme-resid-parent: the re-fit is not the parent cell's weights file plus "
                             f"the block (differ: {differ}); a fitter-affecting change lies between the two fits", error)
        return False
    differ, _ = composition_engine_fit.compare_values(mother, child, strict=True)
    sides = " and ".join(name for name, on in (("the parent", parent_engine), ("this re-fit", engine_fit)) if on)
    _require(not differ, f"{RULE_ID}: --theme-resid-parent: --engine-fit on {sides} (ruling D2-R2): the re-fit is not "
                         f"the parent cell's weights file plus the block within ruling P12 ({len(differ)} values differ: "
                         f"{differ[:20]}); C++ rounding stays within P12, so a fitter-affecting change or a changed "
                         "input lies between the two fits", error)
    return True


def add_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--theme-resid", default=None, choices=(RULE_ID,),
                        help="v8 R-11: theme-resid-v1 on the composition's theme_standardise block (rerank true): "
                             "the same weights plus a theme_residualise block, so the IC runner residualises each "
                             "standardised theme composite on the earlier ones in the registered theme order; absent: "
                             "output bytes unchanged")
    parser.add_argument("--theme-resid-parent", default=None,
                        help="v8 R-11 (finding R6B-O-5): the parent cell's composition_weights.json (its admission.json "
                             "beside it, bound by the file's provenance.admission_sha256); with --theme-resid the re-fit "
                             "must be that file plus the block (only the fitter identity, producer_fingerprint, and the "
                             "admission SHA it moves may differ; with --engine-fit on either fit, ruling D2-R2: values "
                             "within ruling P12), and provenance.resid records its SHA-256; a parent fitted before P9 "
                             "lane D2 is refused (refit it)")
    parser.add_argument("--theme-resid-parent-sha256", default=None, help="the SHA-256 pin of --theme-resid-parent")


def apply(args, document: dict, summary: dict, registered, error: type[Exception] = ResidError,
          parent: dict | None = None, admission: bytes | None = None) -> None:
    """The fitter hook: with ``--theme-resid`` attach the block (refused without a rerank-true theme_standardise) and
    record it in the summary; without it nothing changes. ``registered``: the fitter's PRIOR_THEMES (rule 1).
    ``parent`` (finding R6B-O-5; the fitter's load of --theme-resid-parent: {sha256, weights, admission} bytes) checks
    the re-fit against the parent cell's file (``parent_check``, with ``admission`` the re-fit's admission.json bytes)
    and records the parent's SHA-256 in provenance.resid and the summary."""
    if getattr(args, "theme_resid", None) is None:
        _require(parent is None, f"{RULE_ID}: --theme-resid-parent needs --theme-resid", error)
        return
    attach(document, registered, error)
    if parent is not None:
        if admission is None:
            raise error(f"{RULE_ID}: --theme-resid-parent needs the re-fit's admission table")
        mode = parent_check(document, admission, parent["weights"], parent["admission"], error,
                            engine_fit=bool(getattr(args, "engine_fit", False)))
        document["provenance"]["resid"].update(parent_weights_sha256=parent["sha256"], parent_check=PARENT_CHECK)
        if mode:  # ruling D2-R2: the comparison this file passed (never written without --engine-fit on a side)
            document["provenance"]["resid"]["parent_check_engine_fit"] = PARENT_CHECK_ENGINE_FIT
        summary["theme_residualise_parent_sha256"] = parent["sha256"]
    summary[BLOCK] = dict(document[BLOCK])
