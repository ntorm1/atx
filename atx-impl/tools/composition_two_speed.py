#!/usr/bin/env python3
"""Fitter flag ``--two-speed two-speed-v1`` (platform v8 Y-5, lane YCOMB; Rulings PM8-5, PM8-10, PM8-12): a thin
wrapper that writes the rule's spec into the weights document and nothing else.

The rule lives in C++ (Ruling PM8-12): the registered half-life table and the fast/slow split
(atx-impl/src/strategy_two_speed.hpp, applied by the IC runner's theme_sleeves block, strategy_ic_two_speed.cpp), the
sleeve planes (IcComposition::set_theme_sleeves), and the virtual fast sleeve, both thetas and the netting
(atx/engine/book/two_speed.hpp two_speed_aim, driven by nav --two-speed two-speed-v1). This module only adds
``theme_sleeves: {"rule": "two-speed-v1"}`` to a standardised parent document; the w pass's <role>_sleeves.json and
the NAV summary are the receipts. Refused with --theme-resid (residualised composites are not split) and under --era.
"""
from __future__ import annotations

import argparse

RULE_ID = "two-speed-v1"
BLOCK = "theme_sleeves"
PARENT_RULES = ("ew-theme-std-v1", "ic-shrink-v1")
PARENT_ONLY = (f"--two-speed {RULE_ID} needs the prior path (--orientation prior, a v4 screen) and a standardised "
               "composition (ew-theme-std-v1 or ic-shrink-v1, with or without --theme-erc / --theme-tsmom); it is "
               "refused with --theme-resid and under --era")


class TwoSpeedError(ValueError):
    """A loud refusal (the fitter passes its own FitError instead)."""


def attach(document: dict, error: type[Exception] = TwoSpeedError) -> dict:
    """The parent's document plus the spec block; weights, signs, theme_standardise and provenance.rule unchanged."""
    std = document.get("theme_standardise")
    if not (isinstance(std, dict) and std.get("rerank") is True and "theme_residualise" not in document):
        raise error(f"{RULE_ID}: {PARENT_ONLY}")
    document[BLOCK] = {"rule": RULE_ID}
    return document


def add_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--two-speed", default=None, choices=(RULE_ID,),
                        help="v8 Y-5 (lane YCOMB): write the two-speed-v1 spec (theme_sleeves) into the weights file; "
                             "the IC runner splits the blend into fast and slow sleeves by the registered half-life "
                             "table and nav --two-speed trades them; absent: output bytes unchanged")


def check_args(args, prior: bool, pooled: bool, error: type[Exception] = TwoSpeedError) -> None:
    """Refused before any compute: the flag needs the prior path and a standardised composition, and is refused with
    --theme-resid and under --era."""
    if getattr(args, "two_speed", None) is None:
        return
    if not (prior and getattr(args, "composition", None) in PARENT_RULES and
            getattr(args, "theme_resid", None) is None and not pooled):
        raise error(f"{RULE_ID}: {PARENT_ONLY}")
