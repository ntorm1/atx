#!/usr/bin/env python3
"""Composition schedule ``mom-volman-v1`` (P9 lane AL-COMB, cell P9-M; DEC-16: read after Y-2): the fitter's half.

Hypothesis (Barroso and Santa-Clara 2015, JFE 116(1); Daniel and Moskowitz 2016; real-time form Cederburg, O'Doherty,
Wang and Yan 2020, JFE 138(1)): momentum's realised volatility forecasts its volatility, not its mean, so the
price_momentum theme's mass is scaled each month by target / realised volatility (126 decisions), the target being the
running mean of the sleeve's own estimates. Every estimate is C++ (PM8-12, G-P5: strategy_ic_mom_volman.{hpp,cpp},
mom_volman_blocks, applied by the IC runner through the theme schedule theme-tsmom-v1 uses). This module computes
nothing new: it takes the price_momentum sleeve exactly as theme-tsmom-v1's step 1 forms it
(composition_theme_tsmom.sleeves: sum over the theme's members of (w_k / W_t) s_k f_k(d) over the parent fit's role
decisions, the parent's final weights, flat 0, 0 outside the TRAIN mask) and records it, with the decision sessions,
in the top-level theme_schedule block {rule, theme, lookback, lag, step, sessions, sleeve}; weights, signs,
theme_standardise and provenance.rule stay the parent's.

Parents: ew-theme-std-v1 and ic-shrink-v1, with or without --theme-erc theme-erc-v1. Refused with --theme-resid (a
residualised composite is not the sleeve), with --theme-tsmom (one theme_schedule block holds one rule) and under --era
(the schedule's sessions are one role's). Under --engine-fit (P9 lane D3W, ruling ALCOMB-7) the C++ composition fit
records the same sleeve (atx-research-composition fit, spec key mom_volman), composition_engine_fit holds this
module's sleeve to it under ruling P12, and the fitter's main() writes the C++ sleeve (ruling D2-1); this module's
functions are unchanged.

provenance.mom_volman names its writer by ``producer_fingerprint`` (CM-5 / ruling D2-2, as every composition module
since P9 lane D2: composition_rules.module_fingerprint of PRODUCERS, which also covers the composition_theme_tsmom code
the sleeve reads).
"""
from __future__ import annotations

import argparse

import numpy as np

import composition_rules  # this directory: module_fingerprint (ruling D2-2)
import composition_theme_tsmom

RULE_ID = "mom-volman-v1"
BLOCK = "theme_schedule"            # the block theme-tsmom-v1 writes too: the two rules exclude each other
THEME = "price_momentum"
LOOKBACK = 126                      # strategy_ic_mom_volman.hpp mom_volman_lookback: BSC's six months
LAG = 2                             # mom_volman_lag: f(d) is realised at d + 2
STEP = 21                           # mom_volman_step: one month of decisions
MAX_DECISIONS = 65536               # mom_volman_max_decisions
PARENT_RULES = ("ew-theme-std-v1", "ic-shrink-v1")
RULE_TEXT = (
    "1. the price_momentum sleeve r(d) = sum_{k in theme} (w_k/W) s_k f_k(d) over the parent fit's decisions (w the "
    "parent's final weights), recorded with the decision sessions",
    "2. blocks start at decision j = 126 + 2 - 1 + 21 b",
    "3. sigma_b = sqrt(mean of r(d)^2 over d = j - 2 - 126 + 1 .. j - 2)",
    "4. target_b = the mean of sigma_0 .. sigma_b (the running mean of the sleeve's own estimates)",
    "5. price_momentum's mass in the block is W_pm target_b / sigma_b; every other theme keeps W_t; before the first "
    "block the parent's masses (the IC runner, strategy_ic_mom_volman.cpp)")
PARENT_ONLY = (f"--mom-volman {RULE_ID} needs the prior path (--orientation prior, a v4 screen) and --composition "
               f"{' or '.join(PARENT_RULES)} (with or without --theme-erc); it is refused with --theme-resid and "
               "--theme-tsmom and under --era")
PRODUCERS = ("schedule", "attach")  # the writers of theme_schedule and provenance.mom_volman (ruling D2-2)


class VolmanError(ValueError):
    """A loud refusal of the rule's inputs (the fitter passes its own FitError instead)."""


def _require(condition, message: str, error: type[Exception]) -> None:
    if not condition:
        raise error(message)


def producer_fingerprint() -> str:
    """provenance.mom_volman's code identity (ruling D2-2: composition_rules.module_fingerprint; module_sha256, the
    whole file's SHA-256, before the lane merged P9 lane D2)."""
    return composition_rules.module_fingerprint(__file__, PRODUCERS)


class Schedule:
    def __init__(self, block: dict, provenance: dict, summary: dict):
        self.block, self.provenance, self.summary = block, provenance, summary


def schedule(ids: list[str], themes: list[str], weights, matrix: np.ndarray, sessions, mask: np.ndarray,
             parent_rule: str, error: type[Exception] = VolmanError) -> Schedule:
    """The block over the parent fit's members (the fitter's active order), final weights and signed factor series."""
    weights = [float(w) for w in weights]
    m = np.asarray(matrix, dtype=np.float64)
    sessions = np.asarray(sessions)
    _require(len(ids) == len(themes) == len(weights) == m.shape[0] and ids, f"{RULE_ID}: one factor series and one "
                                                                            "weight per member", error)
    _require(all(np.isfinite(w) and w >= 0.0 for w in weights), f"{RULE_ID}: weights must be finite and >= 0", error)
    _require(m.shape[1] == sessions.shape[0] == np.asarray(mask).shape[0], f"{RULE_ID}: one session and one mask entry "
                                                                           "per decision", error)
    _require(bool(np.all(np.diff(sessions.astype(np.int64)) > 0)), f"{RULE_ID}: decision sessions must increase", error)
    _require(any(t == THEME and w > 0.0 for t, w in zip(themes, weights)),
             f"{RULE_ID}: {THEME} has no weighted member", error)
    _require(LOOKBACK + LAG <= m.shape[1] <= MAX_DECISIONS, f"{RULE_ID}: the role has {m.shape[1]} decisions; the "
             f"first block needs more than {LOOKBACK + LAG - 1} (at most {MAX_DECISIONS})", error)
    sleeve = composition_theme_tsmom.sleeves(m, themes, weights, [THEME], mask)[0]
    block = {"rule": RULE_ID, "theme": THEME, "lookback": LOOKBACK, "lag": LAG, "step": STEP,
             "sessions": [int(s) for s in sessions], "sleeve": [float(x) for x in sleeve]}
    provenance = {
        "rule": RULE_ID, "parent_rule": parent_rule,
        "registration": "scripts/specs/p9/rules/mom-volman-v1.json (P9 lane AL-COMB), declared before any cell read",
        "rule_text": list(RULE_TEXT), "series": composition_theme_tsmom.SERIES + " for the price_momentum theme only",
        "lookback": LOOKBACK, "lag": LAG, "step": STEP, "decisions": int(m.shape[1]),
        "train_decisions": int(np.asarray(mask, dtype=bool).sum()),
        "runner": (f"atx-equity-strategy-ic {BLOCK} {{rule: {RULE_ID}, theme, lookback, lag, step, sessions, sleeve}}: "
                   "the runner computes every block's sigma, target and scale from the recorded sleeve "
                   "(strategy_ic_mom_volman.cpp mom_volman_blocks) and the composition re-ranks each theme per date "
                   "unchanged, times the mass in force (IcComposition::schedule_theme_masses)"),
        "producer_fingerprint": producer_fingerprint()}
    summary = {"parent_rule": parent_rule, "decisions": int(m.shape[1])}
    return Schedule(block, provenance, summary)


def attach(document: dict, fit: Schedule) -> dict:
    """The weights document of the rule: the parent's, plus the top-level theme_schedule block and provenance.mom_volman
    (provenance.rule stays the parent's: it is the rule that writes theme_standardise, finding R6B-C-5)."""
    document[BLOCK] = dict(fit.block)
    document["provenance"]["mom_volman"] = fit.provenance
    return document


def add_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--mom-volman", default=None, choices=(RULE_ID,),
                        help="P9 AL-COMB: mom-volman-v1, the price_momentum theme's mass scaled each month by the "
                             "running-mean target over its sleeve's 126-decision realised volatility (the IC runner "
                             f"computes it from the recorded sleeve; {', '.join(PARENT_RULES)}, with or without "
                             "--theme-erc); absent: output bytes unchanged")


def check_args(args, prior: bool, pooled: bool, error: type[Exception] = VolmanError) -> None:
    """Refused before any compute: the flag needs the prior path and a PARENT_RULES composition, and is refused with
    --theme-resid, --theme-tsmom and under --era. --engine-fit is accepted (P9 lane D3W, ruling ALCOMB-7: the C++ fit
    records the sleeve; composition_engine_fit checks and writes it)."""
    if getattr(args, "mom_volman", None) is None:
        return
    _require(prior and getattr(args, "composition", None) in PARENT_RULES and
             getattr(args, "theme_resid", None) is None and getattr(args, "theme_tsmom", None) is None and not pooled,
             f"{RULE_ID}: {PARENT_ONLY}", error)
