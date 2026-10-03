#!/usr/bin/env python3
"""Fitter flag ``--engine-fit`` (P9 lane D2, DEC-7 / CM rows 1-5b): the fitter runs the C++ admission screen
(``atx-research-admission screen``, lane B1) and the C++ composition fit (``atx-research-composition fit``, lane D2) on
the K-P9-4 factor series of the same role and library, and refuses unless both agree with its own Python values.
The output bytes stay the Python fitter's (byte-identical to the run without the flag); the agreement is recorded in
the stdout summary only (``engine_fit``). Retiring the Python mirrors is a later slice (the prepared deletion commit).

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
with or without --theme-erc, --theme-tsmom, --theme-resid and --two-speed (two-speed writes a spec block only: nothing
to compare). Refused before any compute: another composition, the train orientation or --era.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import tempfile
from typing import NamedTuple

import numpy as np

SUPPORTED = ("ew-theme-std-v1", "ew-theme-std-aim-v1", "ic-shrink-v1", "ic-shrink-aim-v1")
STD_COMPOSITIONS = ("ew-theme-std-v1", "ew-theme-std-aim-v1")
FACTOR_SCHEMA = "atx.factor-series/v1"
FIT_SPEC_SCHEMA = "atx.composition-fit-spec/v1"
FIT_SCHEMA = "atx.composition-fit/v1"
FIT_FILE = "composition_fit.json"
FACTOR_TOLERANCE = 1e-12  # K-P9-4: absolute, as test_factor_series_admission.compare_factor_series
P12_TOLERANCE = 1e-12     # Ruling P12: relative to max(1, |reference|)
ADMISSION_FLOAT_COLUMNS = ("redundant_rho", "tau", "train_mean", "train_sharpe", "hac_t", "max_abs_rho")
EXE_SECONDS = 3600
REFUSED = (f"--engine-fit runs the single-window prior path with --composition {', '.join(SUPPORTED)} (with or without "
           "--theme-erc / --theme-tsmom / --theme-resid / --two-speed) and needs --engine-factors, --admission-exe and "
           "--composition-exe; refused under --era")


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
                        help="P9 D2: also run the C++ admission screen and composition fit on --engine-factors and "
                             "refuse unless both agree with the Python values (decisions exactly, floats to 1e-12 "
                             "relative); the output bytes are the Python fitter's either way, the agreement is in the "
                             "stdout summary (engine_fit)")
    parser.add_argument("--engine-factors", type=Path, default=None,
                        help="--engine-fit: the K-P9-4 factor series directory of the --train role and the library "
                             "(atx-equity-strategy-targets factors)")
    parser.add_argument("--admission-exe", type=Path, default=None, help="--engine-fit: atx-research-admission")
    parser.add_argument("--composition-exe", type=Path, default=None, help="--engine-fit: atx-research-composition")


def check_args(args, prior: bool, pooled: bool, error: type[Exception] = EngineFitError) -> None:
    """Refused before any compute (REFUSED); flag absent: nothing is checked and nothing changes."""
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
    """Check 1: the series of DIR against the fitter's. Returns the worst |f| and |tau| differences."""
    raw = Path(directory, "manifest.json").read_bytes()
    manifest = json.loads(raw)
    _require(isinstance(manifest, dict) and manifest.get("schema") == FACTOR_SCHEMA,
             f"{directory}: not an {FACTOR_SCHEMA} manifest", error)
    ids = [c.get("id") for c in manifest.get("candidates", [])]
    _require(ids == list(state.library_ids), f"factor series candidates {ids} are not the library {state.library_ids}",
             error)
    _require(manifest.get("role_manifest_sha256") == state.role_sha256,
             f"factor series role {manifest.get('role_manifest_sha256')} is not --train-sha256 {state.role_sha256}",
             error)
    sessions = [int(s) for s in manifest.get("decision_sessions_ns", [])]
    _require(sessions == [int(s) for s in state.decision_sessions],
             "factor series decision sessions are not the fitter's decisions", error)
    t, k = len(sessions), len(ids)
    factor = np.frombuffer(Path(directory, "factor.f64").read_bytes(), dtype="<f8")
    tau = np.frombuffer(Path(directory, "tau.f64").read_bytes(), dtype="<f8")
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
    if len(a) != len(b) or a[:1] != b[:1]:
        return [f"shape or header differs: {len(a)} vs {len(b)} lines"], math.inf
    header, out, worst = a[0].split(","), [], 0.0
    for n, (ra, rb) in enumerate(zip(a[1:], b[1:]), start=1):
        ca, cb = ra.split(","), rb.split(",")
        if len(ca) != len(cb):
            out.append(f"line {n}: {len(ca)} vs {len(cb)} cells")
            continue
        for col, x, y in zip(header, ca, cb):
            if col in ADMISSION_FLOAT_COLUMNS and x and y:
                rel = abs(float(x) - float(y)) / max(1.0, abs(float(x)))
                worst = max(worst, rel)
                if rel > tolerance:
                    out.append(f"line {n} {col}: {x} vs {y}")
            elif x != y:
                out.append(f"line {n} {col}: {x!r} vs {y!r}")
    return out, worst


def run_exe(argv: list, error: type[Exception]) -> None:
    done = subprocess.run([str(a) for a in argv], capture_output=True, text=True, timeout=EXE_SECONDS, check=False)
    _require(done.returncode == 0, f"{Path(argv[0]).name} {argv[1]} exited {done.returncode}: "
                                   f"{done.stderr.strip()[-2000:]}", error)


def check_admission(args, state: FitterState, work: Path, error: type[Exception]) -> dict:
    """Check 2: the C++ screen of DIR equals the fitter's admission table."""
    candidates = work / "admission_candidates.json"
    candidates.write_bytes(state.admission_bytes)  # the fitter's admission.json: only its metadata keys are read
    out = work / "admission"
    run_exe([args.admission_exe, "screen", "--factors", args.engine_factors, "--candidates", candidates,
             "--output", out, "--screen", state.screen], error)
    csv = (out / "admission.csv").read_bytes()
    differ, worst = compare_admission_csv(state.admission_csv, csv)
    _require(not differ, f"admission.csv differs from the fitter's (Ruling P12): {differ[:20]}", error)
    manifest = json.loads((out / "manifest.json").read_bytes())
    _require(manifest.get("admitted") == list(state.admitted),
             f"admitted order {manifest.get('admitted')} vs {state.admitted}", error)
    _require(manifest.get("sign_conflicts") == list(state.sign_conflicts),
             f"sign conflicts {manifest.get('sign_conflicts')} vs {state.sign_conflicts}", error)
    counts = manifest.get("counts", {})
    _require(all(counts.get(s, 0) == n for s, n in state.counts.items()) and
             all(state.counts.get(s, 0) == n for s, n in counts.items()),
             f"admission counts {counts} vs {state.counts}", error)
    return {"admission_csv_sha256": hashlib.sha256(csv).hexdigest(),
            "admission_manifest_sha256": hashlib.sha256((out / "manifest.json").read_bytes()).hexdigest(),
            "admission_max_rel_float_diff": worst, "admitted": len(state.admitted)}


# ---------------------------------------------------------------- 3. fit
def fit_spec(state: FitterState) -> dict:
    """The atx.composition-fit-spec/v1 of the fitted members: the C++ re-derives tiers (rule 5 re-grades applied to the
    source tiers), scores, ICs, shares, the ERC, the tsmom schedule and the residual order itself."""
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
    return spec


def expected_fit(state: FitterState) -> dict:
    """The Python values in composition_fit.json's layout (test_composition_fit_fixture.run_case, on the fitter's own
    fit objects): member-ordered lists, the theme order of each block."""
    ids, parent = list(state.members), state.parent
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
    return doc


def _number(x) -> bool:
    return isinstance(x, (int, float)) and not isinstance(x, bool)


def compare_values(want, got, where: str = "$", tolerance: float = P12_TOLERANCE, out: list | None = None,
                   worst: list | None = None) -> tuple[list, float]:
    """Differences of ``got`` (the C++ JSON) against ``want`` (the Python values): every string, bool, null, integer,
    key and length exactly (the decisions), every float to tolerance x max(1, |want|). Returns (differences, worst
    relative float difference)."""
    out = [] if out is None else out
    worst = [0.0] if worst is None else worst
    if _number(want) and _number(got):
        if isinstance(want, int) and isinstance(got, int):
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
            compare_values(want[key], got[key], f"{where}.{key}", tolerance, out, worst)
    elif isinstance(want, (list, tuple)) and isinstance(got, list):
        if len(want) != len(got):
            out.append(f"{where}: length {len(want)} vs {len(got)}")
        for i, (a, b) in enumerate(zip(want, got)):
            compare_values(a, b, f"{where}[{i}]", tolerance, out, worst)
    elif type(want) is not type(got) or want != got:
        out.append(f"{where}: {want!r} vs {got!r}")
    return out, worst[0]


def check_fit(args, state: FitterState, work: Path, error: type[Exception]) -> dict:
    """Check 3: the C++ fit of the fitted members equals the fitter's values."""
    spec_bytes = (json.dumps(fit_spec(state), sort_keys=True, indent=1, allow_nan=False) + "\n").encode("utf-8")
    spec = work / "composition_fit_spec.json"
    spec.write_bytes(spec_bytes)
    out = work / "composition"
    run_exe([args.composition_exe, "fit", "--factors", args.engine_factors, "--spec", spec, "--output", out], error)
    raw = (out / FIT_FILE).read_bytes()
    got = json.loads(raw)
    _require(got.get("schema") == FIT_SCHEMA and got.get("status") == "complete", f"{FIT_FILE}: not a complete "
                                                                                  f"{FIT_SCHEMA} document", error)
    want = expected_fit(state)
    differ, worst = compare_values(want, {k: got.get(k) for k in want})
    _require(not differ, f"the C++ fit differs from the fitter's ({len(differ)}): {differ[:20]}", error)
    return {"composition_fit_sha256": hashlib.sha256(raw).hexdigest(),
            "fit_spec_sha256": hashlib.sha256(spec_bytes).hexdigest(), "fit_max_rel_float_diff": worst,
            "blocks": sorted(k for k in ("theme_erc", "theme_tsmom", "theme_resid") if k in want)}


def verify(args, state: FitterState, error: type[Exception] = EngineFitError) -> dict:
    """Checks 1-3 in order (the first refusal stops); the summary block on agreement. Nothing is written beside the
    output: the C++ outputs live in a temporary directory and are named by their SHA-256."""
    summary = {"status": "agree", "factor_tolerance_abs": FACTOR_TOLERANCE, "p12_tolerance_rel": P12_TOLERANCE}
    summary.update(check_factor_series(Path(args.engine_factors), state, error))
    with tempfile.TemporaryDirectory(prefix="engine-fit-") as tmp:
        work = Path(tmp)
        summary.update(check_admission(args, state, work, error))
        summary.update(check_fit(args, state, work, error))
    return summary
