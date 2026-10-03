#!/usr/bin/env python3
"""Composition schedule ``theme-tsmom-v1`` (platform v8 Y, lane YCOMB, rule Y-2) as pure functions.

Hypothesis (Ehsani and Linnainmaa 2022, "Factor Momentum and the Momentum Factor", Journal of Finance 77(3); Gupta and
Kelly 2019): a factor's own trailing one-year return predicts its next month. Factors after a losing year earn about
zero; after a winning year they earn the premium. Applied walk-forward to the blend's theme sleeves, a theme whose
sleeve lost money over the trailing year sits out the next month and its mass goes pro rata to the other themes. No mean
is fitted on the scored window: each block reads only the SIGN of each sleeve's trailing sum, and every term of that sum
is realised at least LAG sessions before the block's first decision. R-10 (ic-shrink-v1) and R-11 (theme-resid-v1) fit
static member weights and theme-erc-v1 static theme shares from second moments; none changes a weight through time.

Registration (task-YCOMB-report.md; every constant fixed blind from the paper's 12-month formation and one-month holding,
declared before any cell read):
  1. Sleeve r_t(d) = sum_{k in t} (w_k / W_t) * s_k * f_k(d) over the parent fit's role decisions in decision order:
     f_k the fitter's factor series (the member's neutralised gross-1 centred-rank book formed at decision d, realised at
     d + 2; a flat decision 0; 0 outside the TRAIN mask), s_k the prior sign, w the parent's FINAL weights (after its
     cap; theme-erc-v1's when it ran), W_t = sum_{k in t} w_k. Members with w_k = 0 do not enter.
  2. Block b starts at decision index j_b = LOOKBACK + LAG - 1 + b * STEP (while inside the role); its trailing sum
     T_t(j) = sum_{d = j - LAG - LOOKBACK + 1}^{j - LAG} r_t(d).
  3. g_t = 1 if T_t > 0 else 0; if every g is 1 or every g is 0 the block's masses are the parent's W_t verbatim; else
     W'_t = g_t * W_t * (S / K), S = sum_u W_u, K = sum_u g_u W_u (sums in the block's theme order: ascending names).
     Before the first block (the first LOOKBACK + LAG - 1 decisions) the parent's W_t.
  4. Member weights, signs, the member set and the per-date standardisation are the parent's; the IC runner applies the
     masses (strategy_ic_theme_tsmom.cpp re-applies step 3 to the recorded T; the composition re-ranks unchanged).
Parents: ew-theme-std-v1 and ic-shrink-v1, with or without --theme-erc theme-erc-v1 (the standardised compositions a v8
parent carries; X-5 is ew-theme-std-v1 + theme-erc-v1). Refused with --theme-resid (residualised composites are not the
sleeves whose trailing return is read) and under --era (the schedule's sessions are one role's).

Split of work. The fitter (``fit_composition_weights.py --theme-tsmom theme-tsmom-v1`` on the parent's fit argv) fits
the parent, then this schedule; the weights file gains a top-level block theme_schedule {rule, lookback, lag, step,
themes, blocks: [{from_session, trailing}]} and provenance.theme_tsmom; weights, signs, theme_standardise and
provenance.rule stay the parent's. The shared fixture atx-impl/tests/fixtures/theme_tsmom_v1.json pins step 3 on both
sides (ThemeTsmom.SharedFixtureMasses).
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

import numpy as np

RULE_ID = "theme-tsmom-v1"
BLOCK = "theme_schedule"                 # the top-level key of the weights document
PARENT_RULES = ("ew-theme-std-v1", "ic-shrink-v1")
LOOKBACK = 252                           # strategy_ic_theme_tsmom.hpp theme_tsmom_lookback (12 months)
LAG = 3                                  # theme_tsmom_lag: f(d) is realised at d + 2; one session of slack
STEP = 21                                # theme_tsmom_step (one month)
MAX_BLOCKS = 4096                        # theme_tsmom_max_blocks
MAX_THEMES = 32                          # the runner's theme_standardise bound
SERIES = ("r_t(d)=sum_{k in t} (w_k/W_t)*s_k*f_k(d) over the parent fit's role decisions (w the parent's final "
          "weights; f_k the member's neutralised gross-1 centred-rank book return formed at d and realised at d+2, a flat "
          "decision 0, 0 outside the TRAIN mask; s_k the prior sign)")
RULE_TEXT = (
    "1. theme sleeves r_t(d) = sum_{k in t} (w_k/W_t) s_k f_k(d) over the parent fit's decisions (w the parent's final "
    "weights)",
    "2. blocks start at decision j = 252 + 3 - 1 + 21 b; T_t(j) = sum of r_t(d) over d = j - 3 - 252 + 1 .. j - 3",
    "3. g_t = 1[T_t > 0]; all 1 or all 0: the parent's W_t; else W'_t = g_t W_t S / K (S = sum W, K = sum g W); before "
    "the first block the parent's W_t",
    "4. member weights, signs and the per-date standardisation unchanged (the runner applies the masses)")
TEXT = ("theme-tsmom-v1: the parent's weights and per-date standardisation; from decision 254 on, every 21 decisions, a "
        "theme whose sleeve's trailing 252-decision return (ending 3 decisions earlier) is not positive gets mass 0 and "
        "its mass goes pro rata to the other themes (all positive or none positive: the parent's masses)")
PARENT_ONLY = (f"--theme-tsmom {RULE_ID} needs the prior path (--orientation prior, a v4 screen) and --composition "
               f"{' or '.join(PARENT_RULES)} (with or without --theme-erc); it is refused with --theme-resid and under "
               "--era")


class TsmomError(ValueError):
    """A loud refusal of the rule's inputs (the fitter passes its own FitError instead)."""


def _require(condition, message: str, error: type[Exception]) -> None:
    if not condition:
        raise error(message)


def module_sha256() -> str:
    return hashlib.sha256(Path(__file__).resolve().read_bytes()).hexdigest()


# ---------------------------------------------------------------- step 3: the runner kernel's arithmetic
def masses(parent: list[float], trailing: list[float], error: type[Exception] = TsmomError) -> tuple[list[float], int]:
    """atx::impl::strategy::theme_tsmom_masses in the same order: (masses, themes switched off)."""
    _require(len(parent) > 0 and len(trailing) == len(parent), f"{RULE_ID}: one parent mass and one trailing sum per "
                                                                "theme", error)
    on, total, kept = 0, 0.0, 0.0
    for w, t in zip(parent, trailing):
        _require(isinstance(w, float) and np.isfinite(w) and w > 0.0 and isinstance(t, float) and np.isfinite(t),
                 f"{RULE_ID}: parent masses must be finite and > 0 and trailing sums finite", error)
        total += w
        if t > 0.0:
            on += 1
            kept += w
    if on == 0 or on == len(parent):
        return list(parent), 0
    scale = total / kept
    return [w * scale if t > 0.0 else 0.0 for w, t in zip(parent, trailing)], len(parent) - on


# ---------------------------------------------------------------- steps 1 and 2
def theme_masses(themes: list[str], weights: list[float], order: list[str]) -> list[float]:
    """W_t per theme of ``order``: the sum of its members' weights in input order (report and sleeve mixing)."""
    out = []
    for t in order:
        s = 0.0
        for th, w in zip(themes, weights):
            if th == t and w > 0.0:
                s += w
        out.append(s)
    return out


def sleeves(matrix: np.ndarray, themes: list[str], weights: list[float], order: list[str],
            mask: np.ndarray) -> np.ndarray:
    """Step 1: one row per theme of ``order``, r_t(d) = sum_{k in t, w_k > 0} (w_k / W_t) * matrix[k, d] (members in input
    order), 0 where ``mask`` is false. ``matrix`` rows are the members' signed factor series s_k f_k (flat 0)."""
    m = np.asarray(matrix, dtype=np.float64)
    keep = np.asarray(mask, dtype=bool)
    mass = theme_masses(themes, weights, order)
    rows = []
    for t, total in zip(order, mass):
        r = np.zeros(m.shape[1], dtype=np.float64)
        for k, (th, w) in enumerate(zip(themes, weights)):
            if th == t and w > 0.0:
                r = r + (w / total) * m[k]
        rows.append(np.where(keep, r, 0.0))
    return np.vstack(rows)


def block_starts(decisions: int, lookback: int = LOOKBACK, lag: int = LAG, step: int = STEP) -> list[int]:
    """Step 2: j_b = lookback + lag - 1 + b * step while j_b < decisions."""
    return list(range(lookback + lag - 1, decisions, step))


def trailing(series: np.ndarray, j: int, lookback: int = LOOKBACK, lag: int = LAG) -> list[float]:
    """Step 2: T_t(j) = sum of series[t, d] over d = j - lag - lookback + 1 .. j - lag, in decision order."""
    first, last = j - lag - lookback + 1, j - lag
    assert first >= 0, "a block before its window"
    out = []
    for row in np.asarray(series, dtype=np.float64):
        s = 0.0
        for d in range(first, last + 1):
            s += float(row[d])
        out.append(s)
    return out


# ---------------------------------------------------------------- the schedule
class Schedule:
    def __init__(self, block: dict, provenance: dict, summary: dict):
        self.block, self.provenance, self.summary = block, provenance, summary


def schedule(ids: list[str], themes: list[str], weights, matrix: np.ndarray, sessions, mask: np.ndarray,
             parent_rule: str, error: type[Exception] = TsmomError) -> Schedule:
    """theme-tsmom-v1 over the parent fit's members (input order: the fitter's active order) and final weights."""
    weights = [float(w) for w in weights]
    m = np.asarray(matrix, dtype=np.float64)
    sessions = np.asarray(sessions)
    _require(len(ids) == len(themes) == len(weights) == m.shape[0] and ids, f"{RULE_ID}: one factor series and one "
                                                                            "weight per member", error)
    _require(all(np.isfinite(w) and w >= 0.0 for w in weights), f"{RULE_ID}: weights must be finite and >= 0", error)
    _require(m.shape[1] == sessions.shape[0] == np.asarray(mask).shape[0], f"{RULE_ID}: one session and one mask entry "
                                                                           "per decision", error)
    _require(bool(np.all(np.diff(sessions.astype(np.int64)) > 0)), f"{RULE_ID}: decision sessions must increase", error)
    order = sorted({t for t, w in zip(themes, weights) if w > 0.0})
    _require(0 < len(order) <= MAX_THEMES, f"{RULE_ID}: 1..{MAX_THEMES} weighted themes", error)
    starts = block_starts(m.shape[1])
    _require(0 < len(starts) <= MAX_BLOCKS, f"{RULE_ID}: the role has {m.shape[1]} decisions; the first block needs "
                                            f"more than {LOOKBACK + LAG - 1}", error)
    series = sleeves(m, themes, weights, order, mask)
    parent = theme_masses(themes, weights, order)
    blocks, detail, on_count = [], [], [0] * len(order)
    off_total = 0
    for j in starts:
        t = trailing(series, j)
        w, off = masses(parent, t, error)
        off_total += off
        for i, x in enumerate(t):
            on_count[i] += 1 if x > 0.0 else 0
        blocks.append({"from_session": int(sessions[j]), "trailing": t})
        detail.append({"decision_index": j, "from_session": int(sessions[j]), "themes_off": off,
                       "masses_report_only": dict(zip(order, w))})
    block = {"rule": RULE_ID, "lookback": LOOKBACK, "lag": LAG, "step": STEP, "themes": order, "blocks": blocks}
    provenance = {
        "rule": RULE_ID, "parent_rule": parent_rule,
        "registration": "task-YCOMB-report.md (platform v8 Y, lane YCOMB, rule Y-2), declared before any cell read",
        "rule_text": list(RULE_TEXT), "series": SERIES, "lookback": LOOKBACK, "lag": LAG, "step": STEP,
        "decisions": int(m.shape[1]), "train_decisions": int(np.asarray(mask, dtype=bool).sum()),
        "first_block_decision": starts[0], "blocks": len(starts), "theme_order": order,
        "parent_masses_report_only": dict(zip(order, parent)),
        "theme_on_fraction": {t: on_count[i] / len(starts) for i, t in enumerate(order)},
        "theme_blocks_off": off_total, "block_detail": detail,
        "runner": (f"atx-equity-strategy-ic {BLOCK} {{rule: {RULE_ID}, lookback, lag, step, themes, blocks}}: per block "
                   "the runner re-applies step 3 to the recorded trailing sums with the parent's W_theme summed from the "
                   "pinned weights, and the composition re-ranks each theme per date unchanged, times the mass in force "
                   "(strategy_ic_theme_tsmom.cpp, IcComposition::schedule_theme_masses)"),
        "module_sha256": module_sha256()}
    summary = {"parent_rule": parent_rule, "blocks": len(starts), "theme_blocks_off": off_total,
               "theme_on_fraction": provenance["theme_on_fraction"]}
    return Schedule(block, provenance, summary)


def attach(document: dict, fit: Schedule) -> dict:
    """The weights document of the rule: the parent's, plus the top-level theme_schedule block and provenance.theme_tsmom
    (provenance.rule stays the parent's: it is the rule that writes theme_standardise, finding R6B-C-5)."""
    document[BLOCK] = dict(fit.block)
    document["provenance"]["theme_tsmom"] = fit.provenance
    return document


# ---------------------------------------------------------------- the fitter hooks
def add_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--theme-tsmom", default=None, choices=(RULE_ID,),
                        help="v8 Y (lane YCOMB): theme-tsmom-v1 walk-forward theme schedule on the composition "
                             f"({', '.join(PARENT_RULES)}, with or without --theme-erc): every 21 decisions a theme whose "
                             "sleeve's trailing 252-decision return is not positive gets mass 0; absent: output bytes "
                             "unchanged")


def check_args(args, prior: bool, pooled: bool, error: type[Exception] = TsmomError) -> None:
    """Refused before any compute: the flag needs the prior path and a PARENT_RULES composition, and is refused with
    --theme-resid and under --era."""
    if getattr(args, "theme_tsmom", None) is None:
        return
    _require(prior and getattr(args, "composition", None) in PARENT_RULES and
             getattr(args, "theme_resid", None) is None and not pooled, f"{RULE_ID}: {PARENT_ONLY}", error)
