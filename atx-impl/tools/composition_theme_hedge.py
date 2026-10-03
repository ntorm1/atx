#!/usr/bin/env python3
"""Fitter flag ``--theme-hedge theme-cov-hedge-v1`` (P9 lane AL-COMB, cell P9-H; DEC-15): a thin wrapper that writes
the rule's spec into the weights document and nothing else.

The rule lives in C++ (PM8-12, G-P5): the kernel atx-engine/include/atx/engine/combine/char_cov_hedge.hpp (per theme,
each name's trailing loading on the theme's own sleeve return, its part the theme's score does not explain, and the
hedge ratio re-estimated every 21 decisions) and the IC runner's rule atx-impl/src/strategy_ic_theme_hedge.{hpp,cpp}
(block theme_hedge, row theme-cov-hedge-v1 of the composition rule table). Every estimate is made at apply time from
the scored role's own sessions, so nothing fitted on TRAIN enters the file: this module only adds
``theme_hedge: {"rule": "theme-cov-hedge-v1", "lookback": 252, "lag": 2, "step": 21, "min_pairs": 126}`` to a
standardised parent document (the registered constants, which the runner checks). The recipe's and the combined
manifest's ``composition_hedge`` are the receipts. Refused with --theme-resid and --two-speed (a residualised or split
composite is another object) and under --era (the hedge reads one role's sessions).
"""
from __future__ import annotations

import argparse

RULE_ID = "theme-cov-hedge-v1"
BLOCK = "theme_hedge"
LOOKBACK = 252   # strategy_ic_theme_hedge.hpp theme_hedge_lookback: the house's one-year window
LAG = 2          # theme_hedge_lag: a decision's book earns the return two sessions later
STEP = 21        # theme_hedge_step: the house's monthly re-estimation of the hedge ratio
MIN_PAIRS = 126  # theme_hedge_min_pairs: price-risk-v1's beta minimum, half the window
PARENT_RULES = ("ew-theme-std-v1", "ic-shrink-v1")
PARENT_ONLY = (f"--theme-hedge {RULE_ID} needs the prior path (--orientation prior, a v4 screen) and a standardised "
               "composition (ew-theme-std-v1 or ic-shrink-v1, with or without --theme-erc / --theme-tsmom / "
               "--mom-volman); it is refused with --theme-resid and --two-speed and under --era")


class HedgeError(ValueError):
    """A loud refusal (the fitter passes its own FitError instead)."""


def spec_block() -> dict:
    """The block the IC runner parses (ic_detail::composition_hedge): the registered constants, nothing fitted."""
    return {"rule": RULE_ID, "lookback": LOOKBACK, "lag": LAG, "step": STEP, "min_pairs": MIN_PAIRS}


def attach(document: dict, error: type[Exception] = HedgeError) -> dict:
    """The parent's document plus the spec block; weights, signs, theme_standardise and provenance.rule unchanged."""
    std = document.get("theme_standardise")
    if not (isinstance(std, dict) and std.get("rerank") is True and "theme_residualise" not in document and
            "theme_sleeves" not in document):
        raise error(f"{RULE_ID}: {PARENT_ONLY}")
    document[BLOCK] = spec_block()
    return document


def add_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--theme-hedge", default=None, choices=(RULE_ID,),
                        help="P9 AL-COMB: write the theme-cov-hedge-v1 spec (theme_hedge) into the weights file; the "
                             "IC runner hedges each standardised theme on its own sleeve's covariance loading, "
                             "estimated from the role's past returns (Daniel-Mota-Rottke-Santos 2020); absent: output "
                             "bytes unchanged")


def check_args(args, prior: bool, pooled: bool, error: type[Exception] = HedgeError) -> None:
    """Refused before any compute: the flag needs the prior path and a standardised composition, and is refused with
    --theme-resid, --two-speed and under --era."""
    if getattr(args, "theme_hedge", None) is None:
        return
    if not (prior and getattr(args, "composition", None) in PARENT_RULES and
            getattr(args, "theme_resid", None) is None and getattr(args, "two_speed", None) is None and not pooled):
        raise error(f"{RULE_ID}: {PARENT_ONLY}")
