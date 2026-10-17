#!/usr/bin/env python3
"""Fitter flag ``--engine-fit`` (P9 lane D2, DEC-7 / CM rows 1-5b, ruling D2-1): the fitter runs the C++ admission
screen (``atx-research-admission screen``, lane B1) and the C++ composition fit (``atx-research-composition fit``, lane
D2) on the K-P9-4 factor series of the same role and library, and refuses unless both agree with its own Python values
under ruling P12's tolerance. On agreement the weights file carries the C++ fit's values: the fitter composes the
document a second time with every fitted number (ICs, shrunk ICs, shares, theme means, covariance, ERC shares and
contributions, uncapped and capped weights, cap passes, tsmom parent masses, trailing sums and block masses) taken
from composition_fit.json in place of the Python rule functions' own (``kernels``; the composition modules' ``engine``
arguments), so every number derived from them (theme tables, the blend diagnostic, the weighted turnover) is derived
from the C++ values. The agreement goes into the stdout summary and the document (``provenance.engine_fit``). The
admission table stays the Python screen's (B1's comparator holds it to P12). Flag absent: nothing here runs and the
output bytes are the Python fitter's. A C++ / Python gap beyond P12 is a refusal, not a tolerance change.

    --engine-fit --engine-factors DIR --admission-exe EXE --composition-exe EXE

DIR is the ``atx-equity-strategy-targets factors`` output for the --train role and the library in roster order
(atx.factor-series/v1: manifest.json, factor.f64 decisions x candidates, tau.f64). The checks, in order:

1. factor series: the manifest's role SHA-256, candidate ids (library order) and decision sessions equal the fitter's;
   the flat (NaN) decisions equal exactly; f and tau to FACTOR_TOLERANCE absolute (K-P9-4).
2. admission: the C++ screen of DIR with the fitter's admission.json as its candidates file (only the metadata keys
   are read); admission.csv equals the fitter's V4 columns under Ruling P12 (every non-float cell byte for byte, the
   float cells to P12_TOLERANCE x max(1, |x|)); the admitted order, counts and sign conflicts equal exactly.
3. fit: the C++ fit of the fitted members (admission order) with the fitter's tiers, tier score table, aim gains and
   flags; every decision (theme order, capped members, tier re-grades, blocks off, residual order) equal exactly,
   every float to P12_TOLERANCE x max(1, |x|).

Supported: the single-window prior path with ew-theme-std-v1 / ew-theme-std-aim-v1 / ic-shrink-v1 / ic-shrink-aim-v1,
with or without --theme-erc, --theme-tsmom, --theme-resid (with or without --theme-resid-parent, either side fitted
with or without the flag: composition_resid.parent_check compares under ruling P12 then, ruling D2-R2) and --two-speed
(two-speed writes a spec block only: nothing to compare), and ew-theme-aim-v2 (P9 lane D3W-PB; its parent alone: the
modifiers ride on a theme block it does not write). Refused before any compute: another composition (the ew-theme-v1 /
ew-theme-aim-v1 / ew-theme-v6 rules have no C++ fit), the train orientation or --era.

P9 lane D3W-PB (ew-theme-aim-v2): the C++ fit (atx-research-composition, fit.hpp) is theme_gain_weights on the aim
gains, and composition_fit.json's parent block adds member_gains and theme_weights; check 3 holds every value to the
fitter's own (composition_rules.theme_gain_weights and ew_theme_aim_v2 on the same members, themes and gains) under
ruling P12. The fitter's rule function for it takes no C++ values (it sits inside the fitter's producer fingerprint
closure, which this lane does not edit), so ruling D2-1 is met by check_written: the composed weights must equal the C++
final weights bit for bit, else the fit is refused (a difference inside P12 included) and nothing is published. The
C++ refuses --walk-forward with it (its weights read no factor value, only the whole-TRAIN gains); that refusal reaches
the fitter as the executable's exit.

Under the flag the ic-shrink block states that its ICs are the C++ fit's and records their largest difference from
admission.json's train_mean (ruling D2-S4); provenance.engine_fit names the producing code (the two executables'
SHA-256 and this module's fingerprint of verify / kernels / check_written).

P9 lane D3W (contract K-P9-8, rulings D3-1 / D3-2): the walk-forward refit, under --engine-fit only,

    --walk-forward expanding --walk-forward-lag N --walk-forward-step N [--walk-forward-min-decisions N]

(lag and step required with it, min-decisions default 21; none of the three without it; refused with --theme-tsmom,
--theme-resid and --era: their schedule, order and pool are fitted on the whole TRAIN window). Nothing inside the
fitter's fit / fit_prior (its producer_fingerprint closure) changes: ``verify`` runs the C++ fit with
``--walk-forward expanding --lag N --step N --min-decisions N`` (one executable call; the comparison of check 3 reads
only the full-TRAIN blocks) and keeps the C++ rows on the fitter's args; the fitter's main() then calls
``run_post_step``, which runs the unchanged fit into the stage directory ``<output>.engine-stage`` (a stale stage is
refused), turns its weights document into schema atx.dsl-composition-weights/v3 (``weights_v3``: the document as
published, plus the top-level walk_forward block {rule, window, lag, step, min_decisions, rows: [{from_session,
weights: {library id: weight}}]} the IC runner reads, and provenance.walk_forward), publishes the directory atomically
at --output and removes the stage; the summary names the published files. Before the first row no fit exists: the
IC runner holds the book flat there (ruling D3-1). Flag absent: nothing of this runs.

P9 lane D3W (ruling ALCOMB-7): --engine-fit with --mom-volman mom-volman-v1. The C++ fit records the price_momentum
sleeve (spec key mom_volman, written only then; composition_fit.json mom_volman {theme, sleeve}); check 3 holds it to
the Python sleeve (composition_theme_tsmom.sleeves on the fitter's final weights, signed zero-filled series and TRAIN
mask, as composition_mom_volman.schedule records it) under ruling P12, and ``verify`` keeps it on the fitter's args.
The fitter's fit_prior (its fingerprint closure) is not edited, so it still writes the module's sleeve on the C++
weights; ruling D2-1 (the C++ values are written) is met by the same post-step of main(): ``engine_sleeve`` holds the
written sleeve to the C++ one under ruling P12 and writes the C++ sleeve in its place (provenance.mom_volman
sleeve_source). With both flags the sleeve step runs first. fit() alone under either flag is refused.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
from typing import Callable, NamedTuple

import numpy as np

import composition_mom_volman  # this directory: mom-volman-v1's block names (P9 D3W, ruling ALCOMB-7)
import composition_rules  # this directory: module_fingerprint (review S-3)
import composition_theme_tsmom  # this directory: the sleeve formula (P9 D3W, ruling ALCOMB-7)
from engine_tools import research_window as rw  # this directory: TRAIN (research_window.json), as the fitter

AIM_V2 = composition_rules.AIM_V2_RULE_ID  # P9 lane D3W-PB: atx-engine ew_theme_std.hpp ew_theme_aim_v2_rule
SUPPORTED = ("ew-theme-std-v1", "ew-theme-std-aim-v1", "ic-shrink-v1", "ic-shrink-aim-v1", AIM_V2)
STD_COMPOSITIONS = ("ew-theme-std-v1", "ew-theme-std-aim-v1")
GAIN_PARENTS = STD_COMPOSITIONS + (AIM_V2,)  # the parents whose weights are theme_gain_weights'
FACTOR_SCHEMA = "atx.factor-series/v1"
FIT_SPEC_SCHEMA = "atx.composition-fit-spec/v1"
FIT_SCHEMA = "atx.composition-fit/v1"
FIT_FILE = "composition_fit.json"
FACTOR_TOLERANCE = 1e-12  # K-P9-4: absolute, as test_factor_series_admission.compare_factor_series
P12_TOLERANCE = 1e-12     # Ruling P12: relative to max(1, |reference|)
ADMISSION_FLOAT_COLUMNS = ("redundant_rho", "tau", "train_mean", "train_sharpe", "hac_t", "max_abs_rho")
EXE_SECONDS = 3600
PROVENANCE_KEY = "engine_fit"  # the weights file's provenance.engine_fit (flag present only)
WRITTEN = "atx-research-composition fit (C++): every fitted number of the weights file; the admission table: Python"
REFUSED = (f"--engine-fit runs the single-window prior path with --composition {', '.join(SUPPORTED)} (with or without "
           "--theme-erc / --theme-tsmom / --theme-resid [--theme-resid-parent] / --two-speed / --mom-volman, where the "
           "composition takes them) and needs --engine-factors, --admission-exe and --composition-exe; refused under "
           "--era")
PRODUCERS = ("verify", "kernels", "check_written")  # the code that turns the executables' output into written values
# P9 lane D3W (K-P9-8): walk-forward-v1 (atx-engine fit.hpp walk_forward_rule) and the v3 document.
WALK_FORWARD_RULE = "walk-forward-v1"
WALK_FORWARD_WINDOWS = ("expanding",)
WALK_FORWARD_MIN_DECISIONS = 21            # ruling D3-1: the registered value (one step)
POST_STAGE = ".engine-stage"               # <output>.engine-stage: the unchanged fit's directory before the post-step
POST_ARMED = "_engine_post_step"           # args attribute: set by run_post_step (fit alone is refused)
WALK_FORWARD_FIT = "_walk_forward_fit"     # args attribute: the C++ rows verify keeps for run_post_step
VOLMAN_FIT = "_mom_volman_fit"             # args attribute: the C++ sleeve verify keeps for run_post_step
WEIGHTS_SCHEMA_V3 = "atx.dsl-composition-weights/v3"
VOLMAN_SOURCE = ("atx-research-composition fit (C++): the price_momentum sleeve of the C++ final weights (ruling "
                 "ALCOMB-7), held to the Python sleeve under ruling P12 before it is written")
POST_ONLY = ("--walk-forward and --mom-volman under --engine-fit write through the fitter's main() (run_post_step: "
             "the C++ walk-forward rows and sleeve); fit() alone would publish without them")
WALK_FORWARD_REFUSED = ("--walk-forward runs under --engine-fit (the rows are the C++ fit's, rulings D3-1 / ALCOMB-7) "
                        "and is refused with --theme-tsmom, --theme-resid and --era (their schedule, order and pool "
                        "are fitted on the whole TRAIN window)")
WALK_FORWARD_VALUES = ("walk_forward_lag", "walk_forward_step", "walk_forward_min_decisions")


class EngineFitError(ValueError):
    """A loud refusal (the fitter passes its own FitError instead)."""


class FitterState(NamedTuple):
    """What fit_prior holds once its document is complete. ``library_ids``, ``factors`` (candidates x decisions, NaN
    flat) and ``taus`` in library order; ``members`` the fitted ids in admission order with their ``themes``, ``signs``
    (prior signs), source ``tiers`` (None for ic-shrink), ``gains`` (None without aim) and ``ics`` (the admission rows'
    train_mean); ``parent`` the rule's fit object, ``erc`` / ``tsmom`` the modifiers' (None when off), ``resid_order``
    the theme_residualise order (None when off) and ``weights`` the final member weights."""
    library_ids: list
    factors: np.ndarray
    taus: list
    decision_sessions: np.ndarray
    role_sha256: str
    screen: str
    admission_bytes: bytes
    admission_csv: bytes
    admitted: list
    counts: dict
    sign_conflicts: list
    composition: str
    members: list
    themes: list
    signs: list
    tiers: list | None
    tier_scores: dict | None
    gains: list | None
    ics: list
    parent: object
    erc: object
    tsmom: object
    resid_order: list | None
    registered_themes: list
    weights: np.ndarray
    train_decisions: int


def _require(condition, message: str, error: type[Exception]) -> None:
    if not condition:
        raise error(f"--engine-fit: {message}")


def add_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--engine-fit", action="store_true",
                        help="P9 D2 (ruling D2-1): also run the C++ admission screen and composition fit on "
                             "--engine-factors, refuse unless both agree with the Python values (decisions exactly, "
                             "floats to 1e-12 relative), then write the C++ fit's values; the agreement is in the "
                             "summary and provenance.engine_fit; absent: output bytes unchanged")
    parser.add_argument("--engine-factors", type=Path, default=None,
                        help="--engine-fit: the K-P9-4 factor series directory of the --train role and the library "
                             "(atx-equity-strategy-targets factors)")
    parser.add_argument("--admission-exe", type=Path, default=None, help="--engine-fit: atx-research-admission")
    parser.add_argument("--composition-exe", type=Path, default=None, help="--engine-fit: atx-research-composition")
    parser.add_argument("--walk-forward", default=None, choices=WALK_FORWARD_WINDOWS,
                        help="P9 D3W (K-P9-8): with --engine-fit, also refit walk-forward: rows every "
                             "--walk-forward-step decisions from the first whose window (the TRAIN decisions <= d - "
                             "--walk-forward-lag) holds --walk-forward-min-decisions, each the C++ fit on its window; "
                             "the weights file becomes atx.dsl-composition-weights/v3 with the walk_forward rows; "
                             "absent: output bytes unchanged")
    parser.add_argument("--walk-forward-lag", type=int, default=None, help="--walk-forward: lag >= 2 (registered 2)")
    parser.add_argument("--walk-forward-step", type=int, default=None,
                        help="--walk-forward: step >= 1 (registered 21)")
    parser.add_argument("--walk-forward-min-decisions", type=int, default=None,
                        help="--walk-forward: min_decisions >= 2 (default and registered "
                             f"{WALK_FORWARD_MIN_DECISIONS})")


def walk_forward(args) -> bool:
    """P9 D3W: whether the fitter runs walk-forward (--walk-forward given)."""
    return getattr(args, "walk_forward", None) is not None


def _require_wf(condition, message: str, error: type[Exception]) -> None:
    if not condition:
        raise error(f"--walk-forward: {message}")


def walk_forward_options(args) -> tuple[int, int, int]:
    """(lag, step, min_decisions) of the walk-forward flags (check_args has run)."""
    least = getattr(args, "walk_forward_min_decisions", None)
    return (int(args.walk_forward_lag), int(args.walk_forward_step),
            WALK_FORWARD_MIN_DECISIONS if least is None else int(least))


def check_walk_forward(args, pooled: bool, error: type[Exception]) -> None:
    """P9 D3W: refused before any compute (WALK_FORWARD_REFUSED); flag absent: its values are refused alone."""
    if not walk_forward(args):
        _require_wf(all(getattr(args, name, None) is None for name in WALK_FORWARD_VALUES),
                    "--walk-forward-lag / --walk-forward-step / --walk-forward-min-decisions are read only with "
                    "--walk-forward", error)
        return
    _require_wf(getattr(args, "engine_fit", False) and getattr(args, "theme_tsmom", None) is None and
                getattr(args, "theme_resid", None) is None and not pooled, WALK_FORWARD_REFUSED, error)
    _require_wf(getattr(args, "walk_forward_lag", None) is not None and
                getattr(args, "walk_forward_step", None) is not None,
                "--walk-forward needs --walk-forward-lag and --walk-forward-step", error)
    lag, step, least = walk_forward_options(args)
    _require_wf(lag >= 2 and step >= 1 and least >= 2,
                f"lag >= 2 (contract K-P9-8: fitted on decisions <= d - 2), step >= 1 and min_decisions >= 2; "
                f"got lag {lag}, step {step}, min_decisions {least}", error)


def mom_volman(args) -> bool:
    """P9 D3W (ruling ALCOMB-7): --engine-fit with --mom-volman (the C++ sleeve is written by the post-step)."""
    return bool(getattr(args, "engine_fit", False)) and getattr(args, "mom_volman", None) is not None


def post_step(args) -> bool:
    """P9 D3W: whether the fitter's main() runs the fit through run_post_step (--walk-forward, or --engine-fit with
    --mom-volman)."""
    return walk_forward(args) or mom_volman(args)


def check_args(args, prior: bool, pooled: bool, error: type[Exception] = EngineFitError) -> None:
    """Refused before any compute (REFUSED); flag absent: nothing is checked and nothing changes. P9 D3W: the
    walk-forward flags first (check_walk_forward), then fit() alone under a post-step flag (POST_ONLY)."""
    check_walk_forward(args, pooled, error)
    _require(not post_step(args) or getattr(args, POST_ARMED, None) is not None, POST_ONLY, error)
    if not getattr(args, "engine_fit", False):
        _require(getattr(args, "engine_factors", None) is None and getattr(args, "admission_exe", None) is None and
                 getattr(args, "composition_exe", None) is None,
                 "--engine-factors / --admission-exe / --composition-exe are read only with --engine-fit", error)
        return
    _require(prior and not pooled and getattr(args, "composition", None) in SUPPORTED, REFUSED, error)
    for name in ("engine_factors", "admission_exe", "composition_exe"):
        _require(getattr(args, name, None) is not None, REFUSED, error)
    _require(Path(args.engine_factors, "manifest.json").is_file(),
             f"--engine-factors {args.engine_factors}: no manifest.json", error)
    for name in ("admission_exe", "composition_exe"):
        _require(Path(getattr(args, name)).is_file(), f"--{name.replace('_', '-')} {getattr(args, name)}: no such file",
                 error)


# ---------------------------------------------------------------- 1. factor series
def check_factor_series(directory: Path, state: FitterState, error: type[Exception]) -> dict:
    """Check 1: the series of DIR against the fitter's. Returns the worst |f| and |tau| differences. P9 D3W (ruling
    D2-9, review S-d): every read of DIR is the refusal ``error`` when it fails (a missing or unreadable file, a
    manifest that is not JSON, a candidate row that is not an object), as the executables' outputs are."""
    raw = _read(Path(directory, "manifest.json"), "the factor series' manifest.json", error)
    manifest = _json(raw, "the factor series' manifest.json", error)
    _require(isinstance(manifest, dict) and manifest.get("schema") == FACTOR_SCHEMA,
             f"{directory}: not an {FACTOR_SCHEMA} manifest", error)
    rows = manifest.get("candidates", [])
    _require(isinstance(rows, list) and all(isinstance(c, dict) for c in rows),
             f"{directory}: the manifest's candidates must be a list of objects", error)
    ids = [c.get("id") for c in rows]
    _require(ids == list(state.library_ids), f"factor series candidates {ids} are not the library {state.library_ids}",
             error)
    _require(manifest.get("role_manifest_sha256") == state.role_sha256,
             f"factor series role {manifest.get('role_manifest_sha256')} is not --train-sha256 {state.role_sha256}",
             error)
    sessions = [int(s) for s in manifest.get("decision_sessions_ns", [])]
    _require(sessions == [int(s) for s in state.decision_sessions],
             "factor series decision sessions are not the fitter's decisions", error)
    t, k = len(sessions), len(ids)
    factor = np.frombuffer(_read(Path(directory, "factor.f64"), "the factor series' factor.f64", error), dtype="<f8")
    tau = np.frombuffer(_read(Path(directory, "tau.f64"), "the factor series' tau.f64", error), dtype="<f8")
    _require(factor.size == t * k and tau.size == k, "factor.f64 / tau.f64 extent", error)
    cpp = factor.reshape(t, k).T
    mine = np.asarray(state.factors, dtype=np.float64)
    _require(mine.shape == cpp.shape, f"factor shape {mine.shape} vs {cpp.shape}", error)
    flat = [state.library_ids[c] for c in range(k) if not np.array_equal(np.isnan(mine[c]), np.isnan(cpp[c]))]
    _require(not flat, f"flat decisions differ for {flat}", error)
    live = np.isfinite(mine)
    worst_f = float(np.max(np.abs(mine[live] - cpp[live]))) if live.any() else 0.0
    worst_tau = float(np.max(np.abs(np.asarray(state.taus, dtype=np.float64) - tau))) if k else 0.0
    _require(worst_f <= FACTOR_TOLERANCE and worst_tau <= FACTOR_TOLERANCE,
             f"factor series beyond {FACTOR_TOLERANCE}: max |f - f_cpp| {worst_f!r}, max |tau - tau_cpp| "
             f"{worst_tau!r}", error)
    return {"factors_manifest_sha256": hashlib.sha256(raw).hexdigest(), "candidates": k, "decisions": t,
            "max_abs_factor_diff": worst_f, "max_abs_tau_diff": worst_tau}


# ---------------------------------------------------------------- 2. admission
def compare_admission_csv(reference: bytes, candidate: bytes, tolerance: float = P12_TOLERANCE) -> tuple[list, float]:
    """Ruling P12 on two admission.csv files: every non-float cell byte for byte, the float cells to
    tolerance x max(1, |reference|). Returns (differences, worst relative float difference)."""
    a, b = reference.decode("utf-8").splitlines(), candidate.decode("utf-8").splitlines()
    if not a or len(a) != len(b) or a[:1] != b[:1]:
        return [f"shape or header differs: {len(a)} vs {len(b)} lines"], math.inf
    header, out, worst = a[0].split(","), [], 0.0
    for n, (ra, rb) in enumerate(zip(a[1:], b[1:]), start=1):
        ca, cb = ra.split(","), rb.split(",")
        if len(ca) != len(header) or len(cb) != len(header):  # review S-1: no row is truncated by zip
            out.append(f"line {n}: {len(ca)} vs {len(cb)} cells under a {len(header)}-column header")
            continue
        for col, x, y in zip(header, ca, cb):
            if col in ADMISSION_FLOAT_COLUMNS and x and y:
                try:
                    fx, fy = float(x), float(y)
                except ValueError:
                    out.append(f"line {n} {col}: {x!r} vs {y!r} (not numbers)")
                    continue
                if not (math.isfinite(fx) and math.isfinite(fy)):  # review S-1: NaN / inf must match exactly
                    if x != y:
                        out.append(f"line {n} {col}: {x!r} vs {y!r}")
                    continue
                rel = abs(fx - fy) / max(1.0, abs(fx))
                worst = max(worst, rel)
                if rel > tolerance:
                    out.append(f"line {n} {col}: {x} vs {y}")
            elif x != y:
                out.append(f"line {n} {col}: {x!r} vs {y!r}")
    return out, worst


def _read(path: Path, what: str, error: type[Exception]) -> bytes:
    """An executable's output file, or the refusal naming it (review S-2)."""
    try:
        return Path(path).read_bytes()
    except OSError as exc:
        raise error(f"--engine-fit: {what}: {exc}") from exc


def _json(raw: bytes, what: str, error: type[Exception]):
    try:
        return json.loads(raw)
    except (UnicodeDecodeError, ValueError) as exc:
        raise error(f"--engine-fit: {what}: not JSON ({exc})") from exc


def file_sha256(path: Path, error: type[Exception]) -> str:
    return hashlib.sha256(_read(path, f"{Path(path).name}", error)).hexdigest()


def run_exe(argv: list, error: type[Exception]) -> None:
    name = f"{Path(argv[0]).name} {argv[1]}"
    try:
        done = subprocess.run([str(a) for a in argv], capture_output=True, text=True, timeout=EXE_SECONDS,
                              check=False)
    except subprocess.TimeoutExpired as exc:  # review S-2: a refusal, not a traceback
        raise error(f"--engine-fit: {name} ran over {EXE_SECONDS} s") from exc
    except OSError as exc:
        raise error(f"--engine-fit: {name} did not start: {exc}") from exc
    _require(done.returncode == 0, f"{name} exited {done.returncode}: {done.stderr.strip()[-2000:]}", error)


def check_admission(args, state: FitterState, work: Path, error: type[Exception]) -> dict:
    """Check 2: the C++ screen of DIR equals the fitter's admission table."""
    candidates = work / "admission_candidates.json"
    candidates.write_bytes(state.admission_bytes)  # the fitter's admission.json: only its metadata keys are read
    out = work / "admission"
    run_exe([args.admission_exe, "screen", "--factors", args.engine_factors, "--candidates", candidates,
             "--output", out, "--screen", state.screen], error)
    csv = _read(out / "admission.csv", "the screen's admission.csv", error)
    differ, worst = compare_admission_csv(state.admission_csv, csv)
    _require(not differ, f"admission.csv differs from the fitter's (Ruling P12): {differ[:20]}", error)
    manifest_raw = _read(out / "manifest.json", "the screen's manifest.json", error)
    manifest = _json(manifest_raw, "the screen's manifest.json", error)
    _require(isinstance(manifest, dict), "the screen's manifest.json is not an object", error)
    _require(manifest.get("admitted") == list(state.admitted),
             f"admitted order {manifest.get('admitted')} vs {state.admitted}", error)
    _require(manifest.get("sign_conflicts") == list(state.sign_conflicts),
             f"sign conflicts {manifest.get('sign_conflicts')} vs {state.sign_conflicts}", error)
    counts = manifest.get("counts")
    _require(isinstance(counts, dict) and all(counts.get(s, 0) == n for s, n in state.counts.items()) and
             all(state.counts.get(s, 0) == n for s, n in counts.items()),
             f"admission counts {counts} vs {state.counts}", error)
    return {"admission_csv_sha256": hashlib.sha256(csv).hexdigest(),
            "admission_manifest_sha256": hashlib.sha256(manifest_raw).hexdigest(),
            "admission_max_rel_float_diff": worst, "admitted": len(state.admitted)}


# ---------------------------------------------------------------- 3. fit
def fit_spec(state: FitterState, volman: bool = False) -> dict:
    """The atx.composition-fit-spec/v1 of the fitted members: the C++ re-derives tiers (rule 5 re-grades applied to the
    source tiers), scores, ICs, shares, the ERC, the tsmom schedule and the residual order itself. P9 D3W: with
    ``volman`` the key mom_volman (the C++ records the price_momentum sleeve); absent: the spec bytes as before."""
    members = []
    for k, cid in enumerate(state.members):
        row = {"id": cid, "theme": state.themes[k], "sign": int(state.signs[k])}
        if state.tiers is not None:
            row["tier"] = state.tiers[k]
        if state.gains is not None:
            row["gain"] = float(state.gains[k])
        members.append(row)
    spec = {"schema": FIT_SPEC_SCHEMA, "composition": state.composition, "members": members,
            "theme_erc": state.erc is not None, "theme_tsmom": state.tsmom is not None,
            "theme_resid": state.resid_order is not None}
    if state.tier_scores is not None:
        spec["tier_scores"] = {g: float(s) for g, s in state.tier_scores.items()}
    if state.resid_order is not None:
        spec["registered_themes"] = list(state.registered_themes)
    if volman:
        spec["mom_volman"] = True
    return spec


def volman_sleeve(state: FitterState) -> list:
    """P9 D3W (ruling ALCOMB-7): the Python side of the C++ sleeve, the sleeve composition_mom_volman.schedule records:
    composition_theme_tsmom.sleeves' price_momentum row over the fitted members (fitter order) with the final weights,
    the prior-signed zero-filled factor series and the TRAIN mask of the decision sessions (research_window)."""
    index = {cid: c for c, cid in enumerate(state.library_ids)}
    factors = np.asarray(state.factors, dtype=np.float64)
    zero = np.where(np.isnan(factors), 0.0, factors)
    matrix = np.vstack([sign * zero[index[cid]] for cid, sign in zip(state.members, state.signs)])
    sessions = np.asarray(state.decision_sessions, dtype=np.int64)
    mask = (sessions >= rw.TRAIN_BEGIN_NS) & (sessions < rw.TRAIN_END_NS)
    row = composition_theme_tsmom.sleeves(matrix, list(state.themes), [float(w) for w in state.weights],
                                          [composition_mom_volman.THEME], mask)[0]
    return [float(x) for x in row]


def expected_fit(state: FitterState, volman: bool = False) -> dict:
    """The Python values in composition_fit.json's layout (test_composition_fit_fixture.run_case, on the fitter's own
    fit objects): member-ordered lists, the theme order of each block; with ``volman`` the mom_volman block."""
    ids, parent = list(state.members), state.parent
    if state.composition == AIM_V2:
        return _expected_aim_v2(state)
    prov = parent.provenance
    if state.composition in STD_COMPOSITIONS:
        out = {"member_tiers": [prov["member_tiers"][i] for i in ids],
               "member_scores": [prov["member_scores"][i] for i in ids], "tier_regrades": prov["tier_regrades"]}
    else:
        out = {"ic": [float(x) for x in state.ics], "shrunk_ic": [prov["shrunk_ic"][i] for i in ids],
               "within_theme_shares": [prov["within_theme_shares"][i] for i in ids],
               "theme_mean_ic": prov["theme_mean_ic"], "equal_share_themes": prov["equal_share_themes"]}
    out.update(rule=state.composition, weights=[float(w) for w in parent.weights],
               weights_before_cap=[prov["weights_before_cap"][i] for i in ids], cap=prov["cap"],
               cap_iterations=prov["cap_iterations"])
    doc = {"composition": state.composition, "members": ids, "member_themes": list(state.themes),
           "train_decisions": state.train_decisions, "decisions": len(state.decision_sessions), "parent": out,
           "weights": [float(w) for w in state.weights]}
    if state.erc is not None:
        p = state.erc.provenance
        order = p["theme_order"]
        doc["theme_erc"] = {"within_theme_shares": [p["within_theme_shares"][i] for i in ids], "theme_order": order,
                            "covariance": p["covariance"], "decisions": p["decisions"],
                            "theme_shares": [p["theme_shares"][t] for t in order],
                            "risk_contributions": [p["risk_contributions"][t] for t in order],
                            "dispersion": p["dispersion"], "cap": p["cap"],
                            "weights_before_cap": [p["weights_before_cap"][i] for i in ids],
                            "weights": [float(w) for w in state.erc.weights], "cap_iterations": p["cap_iterations"]}
    if state.tsmom is not None:
        b, p = state.tsmom.block, state.tsmom.provenance
        doc["theme_tsmom"] = {
            "theme_order": b["themes"], "parent_masses": [p["parent_masses_report_only"][t] for t in b["themes"]],
            "blocks": [{"decision_index": d["decision_index"], "from_session": blk["from_session"],
                        "trailing": blk["trailing"], "masses": [d["masses_report_only"][t] for t in b["themes"]],
                        "themes_off": d["themes_off"]} for blk, d in zip(b["blocks"], p["block_detail"])],
            "theme_blocks_off": p["theme_blocks_off"],
            "theme_on_count": [round(p["theme_on_fraction"][t] * p["blocks"]) for t in b["themes"]]}
    if state.resid_order is not None:
        doc["theme_resid"] = {"order": list(state.resid_order)}
    if volman:
        doc["mom_volman"] = {"theme": composition_mom_volman.THEME, "sleeve": volman_sleeve(state)}
    return doc


def _expected_aim_v2(state: FitterState) -> dict:
    """P9 lane D3W-PB: the Python values of an ew-theme-aim-v2 fit in composition_fit.json's layout. The fitter's rule
    keeps no fit object (state.parent is None), so its own functions run again on the same members, themes and gains:
    composition_rules.theme_gain_weights (uncapped weights, passes, cap) and ew_theme_aim_v2 (the theme table's
    aim_theme_weight); the final weights are the ones the fitter wrote."""
    ids, themes, gains = list(state.members), list(state.themes), [float(g) for g in state.gains]
    weights, uncapped, passes, cap = composition_rules.theme_gain_weights(themes, gains)
    _, table = composition_rules.ew_theme_aim_v2(ids, themes, gains)
    out = {"rule": AIM_V2, "weights": [float(w) for w in weights], "weights_before_cap": [float(w) for w in uncapped],
           "cap": cap, "cap_iterations": [{"capped": [ids[k] for k in it["capped"]],
                                           "excess_by_theme": it["excess_by_theme"]} for it in passes],
           "member_gains": gains, "theme_weights": {t: table[t]["aim_theme_weight"] for t in sorted(table)}}
    return {"composition": AIM_V2, "members": ids, "member_themes": themes, "train_decisions": state.train_decisions,
            "decisions": len(state.decision_sessions), "parent": out, "weights": [float(w) for w in state.weights]}


def _number(x) -> bool:
    return isinstance(x, (int, float)) and not isinstance(x, bool)


def compare_values(want, got, where: str = "$", tolerance: float = P12_TOLERANCE, out: list | None = None,
                   worst: list | None = None, strict: bool = False) -> tuple[list, float]:
    """Differences of ``got`` (the C++ JSON) against ``want`` (the Python values): every string, bool, null, integer,
    key and length exactly (the decisions), every float to tolerance x max(1, |want|). Returns (differences, worst
    relative float difference). P9 D3W (ruling D2-9, review S-c): ``strict`` also makes an integer against a float a
    difference, equal or not (composition_resid.parent_check: both sides are the fitter's canonical JSON, so a type
    flip is a code change); check 3 keeps the default."""
    out = [] if out is None else out
    worst = [0.0] if worst is None else worst
    if _number(want) and _number(got):
        if strict and isinstance(want, int) != isinstance(got, int):
            out.append(f"{where}: {want!r} vs {got!r} (an integer against a float)")
        elif isinstance(want, int) and isinstance(got, int):
            if want != got:
                out.append(f"{where}: {want!r} vs {got!r}")
        elif not (math.isfinite(float(want)) and math.isfinite(float(got))):
            if not (float(want) == float(got)):
                out.append(f"{where}: {want!r} vs {got!r}")
        else:
            rel = abs(float(want) - float(got)) / max(1.0, abs(float(want)))
            worst[0] = max(worst[0], rel)
            if rel > tolerance:
                out.append(f"{where}: {want!r} vs {got!r}")
    elif isinstance(want, dict) and isinstance(got, dict):
        if sorted(want) != sorted(got):
            out.append(f"{where}: keys {sorted(want)} vs {sorted(got)}")
        for key in sorted(set(want) & set(got)):
            compare_values(want[key], got[key], f"{where}.{key}", tolerance, out, worst, strict)
    elif isinstance(want, (list, tuple)) and isinstance(got, list):
        if len(want) != len(got):
            out.append(f"{where}: length {len(want)} vs {len(got)}")
        for i, (a, b) in enumerate(zip(want, got)):
            compare_values(a, b, f"{where}[{i}]", tolerance, out, worst, strict)
    elif type(want) is not type(got) or want != got:
        out.append(f"{where}: {want!r} vs {got!r}")
    return out, worst[0]


def pure_relative(want, got) -> float:
    """The largest |a - b| / |a| over the finite non-zero floats a of ``want`` (review S-8, report only: ruling P12's
    max(1, |a|) floor holds a value far below 1, a covariance or an IC, to an absolute 1e-12)."""
    if _number(want) and _number(got):
        a, b = float(want), float(got)
        return abs(a - b) / abs(a) if a != 0.0 and math.isfinite(a) and math.isfinite(b) else 0.0
    if isinstance(want, dict) and isinstance(got, dict):
        return max([pure_relative(want[k], got[k]) for k in set(want) & set(got)], default=0.0)
    if isinstance(want, (list, tuple)) and isinstance(got, list):
        return max([pure_relative(a, b) for a, b in zip(want, got)], default=0.0)
    return 0.0


def check_fit(args, state: FitterState, work: Path, error: type[Exception]) -> tuple[dict, dict]:
    """Check 3: the C++ fit of the fitted members equals the fitter's values. Returns (summary keys, C++ document)."""
    volman = mom_volman(args)
    spec_bytes = (json.dumps(fit_spec(state, volman), sort_keys=True, indent=1, allow_nan=False) +
                  "\n").encode("utf-8")
    spec = work / "composition_fit_spec.json"
    spec.write_bytes(spec_bytes)
    out = work / "composition"
    argv = [args.composition_exe, "fit", "--factors", args.engine_factors, "--spec", spec, "--output", out]
    if walk_forward(args):  # P9 D3W: the rows come from the same call; the checks below read the full-TRAIN blocks
        lag, step, least = walk_forward_options(args)
        argv += ["--walk-forward", args.walk_forward, "--lag", lag, "--step", step, "--min-decisions", least]
    run_exe(argv, error)
    raw = _read(out / FIT_FILE, f"the fit's {FIT_FILE}", error)
    got = _json(raw, f"the fit's {FIT_FILE}", error)
    _require(isinstance(got, dict) and got.get("schema") == FIT_SCHEMA and got.get("status") == "complete",
             f"{FIT_FILE}: not a complete {FIT_SCHEMA} document", error)
    want = expected_fit(state, volman)
    differ, worst = compare_values(want, {k: got.get(k) for k in want})
    _require(not differ, f"the C++ fit differs from the fitter's ({len(differ)}): {differ[:20]}", error)
    by_block = {k: pure_relative(want[k], got[k])
                for k in ("parent", "theme_erc", "theme_tsmom", "mom_volman", "weights") if k in want}
    if walk_forward(args):
        setattr(args, WALK_FORWARD_FIT, walk_forward_rows(args, got, state, raw, error))
    if volman:  # check 3 held it to the Python sleeve: same length, every value within P12
        setattr(args, VOLMAN_FIT, {"sleeve": [float(x) for x in got["mom_volman"]["sleeve"]]})
    return ({"composition_fit_sha256": hashlib.sha256(raw).hexdigest(),
             "fit_spec_sha256": hashlib.sha256(spec_bytes).hexdigest(), "fit_max_rel_float_diff": worst,
             "fit_max_pure_rel_diff_by_block": by_block,
             "blocks": sorted(k for k in ("theme_erc", "theme_tsmom", "theme_resid", "mom_volman") if k in want)},
            got)


# ---------------------------------------------------------------- the C++ values written (ruling D2-1)
class EngineKernels(NamedTuple):
    """The C++ fit's values in the shapes the composition modules' ``engine`` arguments take. ``parent``: for
    ew-theme-std rules and ew-theme-aim-v2 theme_gain_weights' (weights, uncapped, passes, cap; ew-theme-aim-v2's rule
    function takes no engine argument, so check_written holds its weights to these bit for bit), for ic-shrink rules
    a dict (ic, shrunk,
    shares, means, equal, uncapped, weights, passes, and ic_admission_max_abs_diff: max |C++ IC - admission.json
    train_mean|, ruling D2-S4); ``erc`` (theme_erc): (within-theme shares, covariance, rule_weights' dict) or None;
    ``tsmom`` (schedule): {parent, blocks: [(trailing, masses, themes off)]} or None; ``weights`` the final member
    weights (fitter member order)."""
    parent: object
    erc: tuple | None
    tsmom: dict | None
    weights: list


def _floats(values) -> list:
    return [float(x) for x in values]


def _passes(rows: list, index: dict) -> list:
    """composition_fit.json cap_iterations ({capped: ids, excess_by_theme}) as composition_rules.member_cap's passes
    ({capped: member indices ascending, excess_by_theme: {theme: excess} ascending})."""
    return [{"capped": sorted(index[cid] for cid in p["capped"]),
             "excess_by_theme": {t: float(p["excess_by_theme"][t]) for t in sorted(p["excess_by_theme"])}}
            for p in rows]


def kernels(got: dict, state: FitterState, error: type[Exception] = EngineFitError) -> EngineKernels:
    """The C++ document ``got`` (check_fit agreed with it) in the composition modules' engine shapes. A missing or
    malformed value is the refusal ``error`` (review S-2); so is an ic-shrink IC further than ruling P12 from the
    admission table's train_mean (ruling D2-S4; check_fit already holds it there)."""
    try:
        engine = _kernels(got, state)
    except (KeyError, TypeError, ValueError, IndexError, AttributeError) as exc:
        raise error(f"--engine-fit: {FIT_FILE} lacks a value the weights file needs: {exc!r}") from exc
    if isinstance(engine.parent, dict):
        diff = max(abs(c - float(a)) for c, a in zip(engine.parent["ic"], state.ics))
        _require(len(engine.parent["ic"]) == len(state.ics) and diff <= P12_TOLERANCE,
                 f"the C++ ICs are {diff!r} from admission.json's train_mean (ruling P12: {P12_TOLERANCE})", error)
        engine.parent["ic_admission_max_abs_diff"] = diff
    return engine


def _kernels(got: dict, state: FitterState) -> EngineKernels:
    index = {cid: k for k, cid in enumerate(state.members)}
    p = got["parent"]
    weights = np.asarray(_floats(p["weights"]), dtype=np.float64)
    uncapped = np.asarray(_floats(p["weights_before_cap"]), dtype=np.float64)
    passes = _passes(p["cap_iterations"], index)
    if state.composition in GAIN_PARENTS:
        parent: object = (weights, uncapped, passes, float(p["cap"]))
    else:
        parent = {"ic": _floats(p["ic"]), "shrunk": _floats(p["shrunk_ic"]),
                  "shares": _floats(p["within_theme_shares"]),
                  "means": {t: float(x) for t, x in p["theme_mean_ic"].items()},
                  "equal": list(p["equal_share_themes"]), "uncapped": uncapped, "weights": weights, "passes": passes}
    erc = None
    if state.erc is not None:
        e = got["theme_erc"]
        erc = (_floats(e["within_theme_shares"]), [_floats(row) for row in e["covariance"]],
               {"theme_shares": _floats(e["theme_shares"]), "contributions": _floats(e["risk_contributions"]),
                "dispersion": float(e["dispersion"]),
                "uncapped": np.asarray(_floats(e["weights_before_cap"]), dtype=np.float64),
                "weights": np.asarray(_floats(e["weights"]), dtype=np.float64),
                "passes": _passes(e["cap_iterations"], index), "cap": float(e["cap"])})
    tsmom = None
    if state.tsmom is not None:
        s = got["theme_tsmom"]
        tsmom = {"parent": _floats(s["parent_masses"]),
                 "blocks": [(_floats(b["trailing"]), _floats(b["masses"]), int(b["themes_off"])) for b in s["blocks"]]}
    return EngineKernels(parent, erc, tsmom, _floats(got["weights"]))


def verify(args, state: FitterState, error: type[Exception] = EngineFitError) -> tuple[dict, EngineKernels]:
    """Checks 1-3 in order (the first refusal stops). On agreement: (the summary block, which the fitter also writes as
    provenance.engine_fit; the C++ values to write, ``kernels``). Nothing is written beside the output: the C++ outputs
    live in a temporary directory and are named by their SHA-256."""
    summary = {"status": "agree", "written": WRITTEN, "factor_tolerance_abs": FACTOR_TOLERANCE,
               "p12_tolerance_rel": P12_TOLERANCE,
               # review S-3: the code that produced the written values
               "admission_exe_sha256": file_sha256(Path(args.admission_exe), error),
               "composition_exe_sha256": file_sha256(Path(args.composition_exe), error),
               "producer_fingerprint": producer_fingerprint()}
    summary.update(check_factor_series(Path(args.engine_factors), state, error))
    with tempfile.TemporaryDirectory(prefix="engine-fit-") as tmp:
        work = Path(tmp)
        summary.update(check_admission(args, state, work, error))
        fit_keys, got = check_fit(args, state, work, error)
        summary.update(fit_keys)
    return summary, kernels(got, state, error)


def producer_fingerprint() -> str:
    """provenance.engine_fit's code identity (review S-3): composition_rules.module_fingerprint of PRODUCERS, the
    functions that turn the executables' output into the values written."""
    return composition_rules.module_fingerprint(__file__, PRODUCERS)


def check_written(written_weights, engine: EngineKernels, error: type[Exception] = EngineFitError) -> None:
    """The weights the second composition wrote are the C++ fit's final weights bit for bit (fitter member order)."""
    _require([float(w) for w in written_weights] == engine.weights,
             "the composed weights are not the C++ fit's final weights", error)


# ---------------------------------------------------------------- P9 D3W: walk-forward (K-P9-8, ruling D3-1)
WALK_FORWARD_ROW_KEYS = ("decision_index", "from_session", "fit_decisions", "last_fit_index", "last_fit_session",
                         "weights")
WALK_FORWARD_MAX_ROWS = 4096  # atx-engine fit.hpp walk_forward_max_rows


def _count(x) -> bool:
    return isinstance(x, int) and not isinstance(x, bool) and x >= 0


def walk_forward_rows(args, got: dict, state: FitterState, raw: bytes, error: type[Exception]) -> dict:
    """The C++ fit's walk_forward block (check 3 agreed with its full-TRAIN blocks), checked for shape: the rule, the
    options asked for, 1..4096 rows of WALK_FORWARD_ROW_KEYS at strictly increasing decisions, each row's weights one
    finite value >= 0 per fitted member (state.members order) weighting something. Kept for weights_v3 with the
    members, their themes and the C++ document's SHA-256."""
    block = got.get("walk_forward")
    lag, step, least = walk_forward_options(args)
    _require_wf(isinstance(block, dict) and block.get("rule") == WALK_FORWARD_RULE and
                block.get("window") == args.walk_forward and
                [block.get("lag"), block.get("step"), block.get("min_decisions")] == [lag, step, least] and
                isinstance(block.get("rows"), list) and 1 <= len(block["rows"]) <= WALK_FORWARD_MAX_ROWS,
                f"{FIT_FILE} carries no {WALK_FORWARD_RULE} block for window {args.walk_forward}, lag {lag}, step "
                f"{step}, min_decisions {least}", error)
    before = -1
    for n, row in enumerate(block["rows"]):
        _require_wf(isinstance(row, dict) and sorted(row) == sorted(WALK_FORWARD_ROW_KEYS) and
                    all(_count(row[k]) for k in WALK_FORWARD_ROW_KEYS[:5]) and row["decision_index"] > before and
                    isinstance(row["weights"], list) and len(row["weights"]) == len(state.members) and
                    all(_number(w) and math.isfinite(w) and w >= 0 for w in row["weights"]) and
                    any(w > 0 for w in row["weights"]),
                    f"{FIT_FILE} walk_forward row {n} is malformed", error)
        before = row["decision_index"]
    return {"members": list(state.members), "member_themes": list(state.themes), "block": block,
            "composition_fit_sha256": hashlib.sha256(raw).hexdigest()}


def weights_v3(document: dict, rows: dict) -> dict:
    """The published weights document as atx.dsl-composition-weights/v3 (the IC runner's composition_walk_forward):
    a copy with the schema v3, the top-level walk_forward {rule, window, lag, step, min_decisions, rows:
    [{from_session, weights: {every library id: weight}}]} (a library candidate the fit did not take part in: 0) and
    provenance.walk_forward (each row's window: decision index, fitted decisions, last fitted decision). The full-TRAIN
    weights, signs and blocks stay as written. Under theme_standardise a candidate a row weights beyond the top-level
    weights gets its fitted theme (the C++ fit's member theme) in themes. P9 lane D3W-PB (review D3W S-8): the file is
    not re-checked here; the IC runner owns the refusal (strategy_ic_rules.cpp lift_walk_forward_candidates: such a
    candidate's sign, and its theme among the top-level weighted themes), and the C++ fit cannot produce a row that
    meets it (members are signed +1, and every theme of a fit carries mass: the 1/T theme shares survive the member cap,
    ic-shrink's equal-share fall-back and theme-erc-v1's positive shares)."""
    out = json.loads(json.dumps(document))
    block, members = rows["block"], rows["members"]
    library = list(document["weights"])  # every library candidate (the fitter writes one weight per candidate)
    v3_rows = []
    for row in block["rows"]:
        weights = dict.fromkeys(library, 0.0)
        weights.update({cid: float(w) for cid, w in zip(members, row["weights"])})
        v3_rows.append({"from_session": int(row["from_session"]), "weights": weights})
    themes = out.get("theme_standardise", {}).get("themes")
    if themes is not None:
        theme_of = dict(zip(members, rows["member_themes"]))
        lifted = sorted({cid for row in v3_rows for cid, w in row["weights"].items()
                         if w > 0 and not document["weights"][cid] > 0})
        themes.update({cid: theme_of[cid] for cid in lifted})
    out["schema"] = WEIGHTS_SCHEMA_V3
    out["walk_forward"] = {"rule": block["rule"], "window": block["window"], "lag": block["lag"],
                           "step": block["step"], "min_decisions": block["min_decisions"], "rows": v3_rows}
    out["provenance"]["walk_forward"] = {
        "rule": block["rule"], "composition_fit_sha256": rows["composition_fit_sha256"],
        "producer": "atx-research-composition fit --walk-forward (C++): row j is the composition fit on the TRAIN "
                    "decisions d <= j - lag, in force from decision j's session; the IC runner holds the book flat "
                    "before the first row (ruling D3-1)",
        "rows": [{k: row[k] for k in WALK_FORWARD_ROW_KEYS[:5]} for row in block["rows"]]}
    return out


def engine_sleeve(document: dict, volman: dict, error: type[Exception] = EngineFitError) -> dict:
    """P9 D3W (ruling ALCOMB-7, D2-1): the published weights document with the C++ sleeve written in place of the one
    fit_prior wrote (the module's sleeve on the C++ weights): a copy whose theme_schedule.sleeve is the C++ sleeve and
    provenance.mom_volman.sleeve_source VOLMAN_SOURCE. Refused unless the document carries the mom-volman-v1 block of
    the same length and the written sleeve agrees with the C++ one under ruling P12."""
    block = document.get(composition_mom_volman.BLOCK)
    _require(isinstance(block, dict) and block.get("rule") == composition_mom_volman.RULE_ID and
             isinstance(block.get("sleeve"), list) and len(block["sleeve"]) == len(volman["sleeve"]),
             f"the weights document carries no {composition_mom_volman.RULE_ID} sleeve of the C++ fit's length", error)
    differ, _ = compare_values(block["sleeve"], volman["sleeve"], "$.theme_schedule.sleeve")
    _require(not differ, f"the written sleeve differs from the C++ fit's (ruling P12): {differ[:20]}", error)
    out = json.loads(json.dumps(document))
    out[composition_mom_volman.BLOCK]["sleeve"] = list(volman["sleeve"])
    out["provenance"]["mom_volman"]["sleeve_source"] = VOLMAN_SOURCE
    return out


class PostStepHooks(NamedTuple):
    """The fitter's own names run_post_step uses (its main() builds this: no module-level name of the fitter is
    added, so nothing its producer fingerprint reaches moves)."""
    error: type[Exception]
    canonical: Callable[[dict], bytes]
    publish: Callable[[Path, dict], None]
    pending: Callable[[Path], Path]
    limit: int
    weights_file: str
    exit_ok: int


def run_post_step(args, fit: Callable, hooks: PostStepHooks, log=None) -> tuple[int, dict]:
    """The fitter under a post-step flag (post_step): refuse an existing --output or a stale stage, run ``fit``
    unchanged into <output>.engine-stage, then publish (hooks.publish: all or nothing) the stage's files at --output
    with the weights file rewritten (engine_sleeve under --mom-volman, then weights_v3 under --walk-forward; a fit that
    published no weights file is published as it is), and remove the stage. Any refusal after the fit removes the
    stage and publishes nothing. The summary names the published output, files and weights SHA-256, and the rows
    (walk_forward)."""
    error, out = hooks.error, Path(args.output)
    stage = out.with_name(out.name + POST_STAGE)
    _require(not out.exists(), f"output exists; refusing overwrite: {out}", error)
    _require(not stage.exists() and not hooks.pending(stage).exists(),
             f"stale or concurrent post-step stage; inspect and remove: {stage}", error)
    setattr(args, POST_ARMED, str(stage))
    args.output = stage
    try:
        code, summary = fit(args, log=log)
    finally:
        args.output = out
    try:
        files = {p.name: p.read_bytes() for p in sorted(stage.iterdir())}
        summary = dict(summary, output=str(out))
        if code == hooks.exit_ok:
            document = json.loads(files[hooks.weights_file])
            if mom_volman(args):
                volman = getattr(args, VOLMAN_FIT, None)
                _require(volman is not None, "the fit kept no C++ sleeve (the C++ fit did not run)", error)
                document = engine_sleeve(document, volman, error)
            if walk_forward(args):
                rows = getattr(args, WALK_FORWARD_FIT, None)
                _require_wf(rows is not None, "the fit kept no walk-forward rows (the C++ fit did not run)", error)
                document = weights_v3(document, rows)
                first, last = rows["block"]["rows"][0], rows["block"]["rows"][-1]
                summary["walk_forward"] = {"rule": WALK_FORWARD_RULE, "rows": len(rows["block"]["rows"]),
                                           "first_from_session": first["from_session"],
                                           "last_from_session": last["from_session"]}
            files[hooks.weights_file] = hooks.canonical(document)
            _require(len(files[hooks.weights_file]) <= hooks.limit,
                     f"output: the weights JSON exceeds the runner's {hooks.limit}-byte bound", error)
            summary["weights_sha256"] = hashlib.sha256(files[hooks.weights_file]).hexdigest()
        summary["files"] = {n: hashlib.sha256(b).hexdigest() for n, b in sorted(files.items())}
        hooks.publish(out, files)
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    return code, summary
