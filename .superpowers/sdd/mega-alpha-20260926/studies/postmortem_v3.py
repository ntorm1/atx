#!/usr/bin/env python3
"""postmortem_v3.py -- T16 post-mortem of FREEZE v3-daily-2026-09-27 (analysis only; the root runs it).

Sections (each runnable alone with --sections; results merge into DIR/postmortem.{json,md}):
  A  reproduce the frozen TRAIN fit from the fitter's work cache (weights, admitted set) and report the
     in-sample combined factor F_t = sum_k w_k s_k f_k,t (Sharpe, by year, each year's P&L share)
  B  walk-forward process estimate inside TRAIN: fit the pipeline on a sub-window, score F on the next
     untouched window (B1..B4 as briefed; B1h/B4h keep an internal HOLD year, protocol-shaped)
  C  construction alternatives on the B1 and fit-2020 -> 2021-22 splits
  D  selection-bias null: demeaned f_k, circular block bootstrap of dates (block 21, same blocks for all
     candidates), full frozen protocol; percentile of the observed statistics in the null
  E  paper-book math check (book level): saved combined -> tied-rank book (plain, and the fitter's
     price-risk-v1 neutral book), forward r[d+L], vs the NAV gross series; metadata-only check that the
     runs used the pinned signs and weights
  F  TRAIN attribution of the in-sample F P&L by candidate / family / year; top-5 concentration

Selection hygiene: A-D and F read TRAIN (2020-2022) only. E reads validation at BOOK level only (the
paper book of the saved validation combined, the NAV gross/net series, sign/weight metadata equality).
E never emits a per-candidate validation number or any candidate id.

Reuses fit_composition_weights (screen_v3, fit_weights, factor_stats, Context, PricePanel, RoleManifest,
load_library, canonical_compact, standalone_turnover) and nav_recon (desired_target, peak_rss_mb,
date_of). The fitter is imported, never modified: it self-hashes (SCRIPT_SHA256) into its work cache,
context and output bytes, so any edit would invalidate the frozen work cache; its screen and weight
functions already take the window as masks, and only fit()'s CLI glue hard-codes FIT/HOLD.

Budget: <= 180 s wall and <= 1536 MiB per invocation. A/B/C/F take seconds; D ~0.11 s per rep at 121 x 754
(reps split-able by --seed, pooled in DIR); E measured on a synthetic 1196 x 5627 world: train 2.4 s /
437 MiB (cached context), validation 18.6 s / 492 MiB (context rebuilt); split with --e-roles if needed.
"""
from __future__ import annotations

import os

for _var in ("OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS", "OMP_NUM_THREADS"):
    os.environ.setdefault(_var, "1")

import argparse  # noqa: E402
import csv  # noqa: E402
import hashlib  # noqa: E402
import importlib.util  # noqa: E402
import json  # noqa: E402
import math  # noqa: E402
import sys  # noqa: E402
import time  # noqa: E402
import traceback  # noqa: E402
import warnings  # noqa: E402
from contextlib import contextmanager  # noqa: E402
from pathlib import Path  # noqa: E402
from types import SimpleNamespace  # noqa: E402

import numpy as np  # noqa: E402

HERE = Path(__file__).resolve().parent
DEFAULT_ROOT = Path("C:/atx-wt/pool-2")
SCHEMA = "atx.v3-postmortem/v1"
SECTIONS = ("A", "B", "C", "D", "E", "F")
ANN = 252
BLOCK = 21
LAGS = (1, 2, 3)
SCENARIO = "modeled-1bn-stale5-v1+swap-fin-v1"
COMBINED_SCHEMA = "atx.dsl-combined-signal/v1"
# Frozen pins (ledger / v3_validation_once.sh); reported as checks, never used to select anything.
FROZEN_WEIGHTS_SHA = "db0a8b9a1b70d6ba81f7af6d1e1a7f8774f3a98ac6e1ecf2566a1610ce764aac"
FROZEN_TRAIN_ROLE_SHA = "210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de"
FROZEN_VAL_ROLE_SHA = "0c757c41a363659664c96359a2d2288e10f792e2b91ab38ca8bf5e064dfbbda7"
PRIOR_SIGN_KEYS = ("prior_sign", "declared_sign", "expected_sign")


def year_ns(year: int) -> int:
    return int(np.datetime64(f"{year}-01-01", "ns").astype(np.int64))


# (id, description, orient years [a, b), HOLD years [a, b) or None, score years [a, b))
# HOLD None = the HOLD-sign test is off (screen_v3 gets hold = orient: HOLD mean == FIT mean > 0 iff
# FIT Sharpe > 0, which orientation already guarantees). Weights are fit on orient | HOLD.
SPLITS_B = (
    ("B1", "fit 2020-21 -> score 2022 (no HOLD test)", (2020, 2022), None, (2022, 2023)),
    ("B2", "fit 2020 -> score 2021", (2020, 2021), None, (2021, 2022)),
    ("B3", "fit 2021 -> score 2022", (2021, 2022), None, (2022, 2023)),
    ("B4", "fit 2021-22 -> score 2020 (backward)", (2021, 2023), None, (2020, 2021)),
    ("B1h", "orient 2020 + HOLD 2021, weights 2020-21 -> score 2022", (2020, 2021), (2021, 2022), (2022, 2023)),
    ("B4h", "orient 2021 + HOLD 2022, weights 2021-22 -> score 2020", (2021, 2022), (2022, 2023), (2020, 2021)),
)
SPLITS_C = (
    ("B1", "fit 2020-21 -> score 2022", (2020, 2022), None, (2022, 2023)),
    ("F20", "fit 2020 -> score 2021-22", (2020, 2021), None, (2021, 2023)),
)
RULES_C = (
    ("C1", "equal weight, all candidates, FIT-oriented signs"),
    ("C2", "equal weight, all candidates, library-declared prior sign"),
    ("C3", "equal weight, admitted"),
    ("C4", "inverse-vol, admitted"),
    ("C5", "mv-shrink-0.9-nonneg-v1, admitted (frozen rule)"),
    ("C6", "family: within-family mean, equal across families (FIT-oriented)"),
)


class PostmortemError(Exception):
    """A loud refusal: an input differs from the frozen run or breaks a contract."""


def need(condition, message: str) -> None:
    if not condition:
        raise PostmortemError(message)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


# ------------------------------------------------------------------------------ imports
def import_fitter(path: Path):
    """fit_composition_weights imported from its file (its SCRIPT_SHA256 is that file's SHA)."""
    name = "fit_composition_weights"
    if name in sys.modules and Path(getattr(sys.modules[name], "__file__", "")).resolve() == Path(path).resolve():
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, str(path))
    need(spec is not None and spec.loader is not None, f"cannot import the fitter from {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def import_nav_recon():
    if str(HERE) not in sys.path:
        sys.path.insert(0, str(HERE))
    import nav_recon  # noqa: E402  (studies/nav_recon.py; module level is imports + constants only)
    return nav_recon


# ------------------------------------------------------------------------------ numerics
def clean(x):
    """JSON-safe copy: numpy scalars to Python, non-finite floats to None."""
    if isinstance(x, dict):
        return {str(k): clean(v) for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [clean(v) for v in x]
    if isinstance(x, np.ndarray):
        return clean(x.tolist())
    if isinstance(x, (bool, np.bool_)):
        return bool(x)
    if isinstance(x, (int, np.integer)):
        return int(x)
    if isinstance(x, (float, np.floating)):
        v = float(x)
        return v if math.isfinite(v) else None
    return x


def sharpe(x) -> float | None:
    """Annualized Sharpe (sqrt 252, ddof 1) of the finite values."""
    x = np.asarray(x, dtype=np.float64)
    x = x[np.isfinite(x)]
    if x.size < 2:
        return None
    sd = float(x.std(ddof=1))
    return float(x.mean()) / sd * math.sqrt(ANN) if sd > 0 else None


def ann_stats(x) -> dict:
    x = np.asarray(x, dtype=np.float64)
    x = x[np.isfinite(x)]
    if x.size == 0:
        return {"days": 0, "mean_ann": None, "vol_ann": None, "sharpe": None, "pnl": 0.0}
    sd = float(x.std(ddof=1)) if x.size > 1 else float("nan")
    return {"days": int(x.size), "mean_ann": float(x.mean()) * ANN, "vol_ann": sd * math.sqrt(ANN),
            "sharpe": sharpe(x), "pnl": float(x.sum())}


def corr(a, b) -> float | None:
    a, b = np.asarray(a, dtype=np.float64), np.asarray(b, dtype=np.float64)
    if a.size < 3 or not (a.std() > 0 and b.std() > 0):
        return None
    return float(np.corrcoef(a, b)[0, 1])


def spearman(a, b, with_p: bool = True) -> tuple[float | None, float | None]:
    """Spearman rho (Pearson of average ranks) and, optionally, scipy's two-sided p-value."""
    from scipy.stats import rankdata, spearmanr
    a, b = np.asarray(a, dtype=np.float64), np.asarray(b, dtype=np.float64)
    if a.size < 3:
        return None, None
    rho = corr(rankdata(a), rankdata(b))
    p = float(spearmanr(a, b)[1]) if (with_p and rho is not None) else None
    return rho, p


def years_of(sessions: np.ndarray) -> np.ndarray:
    return sessions.astype("datetime64[ns]").astype("datetime64[Y]").astype(np.int64) + 1970


def window_mask(sessions: np.ndarray, years: tuple[int, int]) -> np.ndarray:
    return (sessions >= year_ns(years[0])) & (sessions < year_ns(years[1]))


def block_bootstrap_indices(t: int, block: int, rng: np.random.Generator) -> np.ndarray:
    """Circular block bootstrap of t positions: ceil(t/block) uniform starts, blocks wrap mod t."""
    blocks = -(-t // block)
    starts = rng.integers(0, t, size=blocks)
    return ((starts[:, None] + np.arange(block)[None, :]) % t).reshape(-1)[:t]


def percentile_of(observed, null) -> float | None:
    """100 * (P[null < obs] + P[null == obs] / 2) over the finite null values."""
    v = np.array([x for x in null if x is not None and math.isfinite(x)], dtype=np.float64)
    if observed is None or v.size == 0:
        return None
    return float(100.0 * (np.mean(v < observed) + 0.5 * np.mean(v == observed)))


def null_summary(observed, values) -> dict:
    v = np.array([x for x in values if x is not None and math.isfinite(x)], dtype=np.float64)
    out = {"observed": observed, "n": int(v.size), "percentile_of_observed": percentile_of(observed, values)}
    if v.size:
        out.update(mean=float(v.mean()), sd=float(v.std(ddof=1)) if v.size > 1 else None,
                   p05=float(np.quantile(v, 0.05)), p50=float(np.quantile(v, 0.5)),
                   p95=float(np.quantile(v, 0.95)), max=float(v.max()))
    return out


# ------------------------------------------------------------------------------ protocol
@contextmanager
def fitter_thresholds(F, min_fit: int | None, min_common: int | None):
    """Temporarily set the screen's day thresholds (module globals read by F.screen_v3)."""
    old = (F.MIN_FIT_DAYS, F.MIN_COMMON_DAYS)
    try:
        if min_fit is not None:
            F.MIN_FIT_DAYS = int(min_fit)
        if min_common is not None:
            F.MIN_COMMON_DAYS = int(min_common)
        yield
    finally:
        F.MIN_FIT_DAYS, F.MIN_COMMON_DAYS = old


def run_protocol(F, factors: np.ndarray, taus, ids, orient: np.ndarray, hold: np.ndarray | None,
                 weight: np.ndarray, min_fit: int | None = None, min_common: int | None = None) -> dict:
    """The frozen v3 pipeline on explicit masks.

    F.screen_v3 (orientation = sign of the orient-window mean, insufficient/turnover/unstable checks,
    greedy redundancy over orient days), then fit()'s weight glue verbatim: s_k * zero-filled f_k over
    the ``weight`` decisions, degenerate (SD 0) rows dropped, F.fit_weights (mv-shrink-0.9-nonneg-v1).
    ``hold=None`` switches the HOLD-sign test off. With the frozen masks and ``weight`` all True this is
    exactly fit(): same rows, same matrix, same weights.
    """
    hold_mask = orient if hold is None else hold
    with fitter_thresholds(F, min_fit, min_common):
        rows = F.screen_v3(factors, taus, ids, orient, hold_mask)
    signs = [r["s_k"] for r in rows]
    admitted = sorted((k for k, r in enumerate(rows) if r["status"] == "admitted"),
                      key=lambda k: rows[k]["admission_rank"])
    zf = np.where(np.isnan(factors), 0.0, factors)
    active = []
    for k in sorted(admitted):
        sd = F.factor_stats(signs[k] * zf[k][weight])["factor_sd"]
        if sd > 0:
            active.append(k)
    weights = np.zeros(len(ids))
    raw = np.full(len(ids), np.nan)
    error, matrix = None, None
    if not active:
        error = "no admitted candidate with a non-degenerate series"
    else:
        matrix = np.vstack([signs[k] * zf[k][weight] for k in active])
        try:
            w, r = F.fit_weights(matrix)
            weights[active], raw[active] = w, r
        except F.FitError as exc:
            error = str(exc)
    return {"rows": rows, "signs": signs, "admitted": admitted, "active": active, "weights": weights,
            "raw": raw, "error": error, "matrix": matrix, "zf": zf}


def book_series(res: dict) -> np.ndarray:
    """F_t = sum_k w_k s_k f_k,t over every decision (flat days 0)."""
    return (res["weights"] * np.asarray(res["signs"], dtype=np.float64)) @ res["zf"]


def screen_diagnostics(rows: list[dict], with_p: bool = True) -> dict:
    """FIT-vs-HOLD Spearman of oriented Sharpes and the fraction whose sign held in HOLD."""
    pairs = [(r["fit_sharpe"], r["hold_sharpe"]) for r in rows
             if r["fit_sharpe"] is not None and r["hold_sharpe"] is not None]
    rho, p = spearman([a for a, _ in pairs], [b for _, b in pairs], with_p) if pairs else (None, None)
    kept = [r["hold_mean"] > 0 for r in rows if r["s_k"] != 0 and r["hold_mean"] is not None]
    return {"spearman_fit_hold_sharpe": rho, "spearman_p": p, "spearman_n": len(pairs),
            "frac_sign_kept": (sum(kept) / len(kept)) if kept else None, "n_kept": int(sum(kept)),
            "n_oriented": len(kept)}


def protocol_stats(F, factors, taus, ids, fit_mask, hold_mask) -> dict:
    """The four D statistics of the frozen protocol (weights on every decision)."""
    res = run_protocol(F, factors, taus, ids, fit_mask, hold_mask, np.ones(factors.shape[1], dtype=bool))
    diag = screen_diagnostics(res["rows"], with_p=False)
    is_sr = None
    if res["error"] is None:
        is_sr = F.factor_stats(res["weights"][res["active"]] @ res["matrix"])["factor_sharpe_annualized"]
    return {"is_sharpe": is_sr, "admitted": len(res["admitted"]),
            "spearman": diag["spearman_fit_hold_sharpe"], "frac_kept": diag["frac_sign_kept"],
            "no_book": res["error"] is not None}


def scaled_thresholds(F, orient: np.ndarray, frozen_fit_days: int) -> tuple[int, int]:
    """Pro-rate the 250-day minima to the orient window (identical to 250 for the frozen FIT window)."""
    n = int(orient.sum())
    scale = n / max(frozen_fit_days, 1)
    return (min(F.MIN_FIT_DAYS, int(math.ceil(F.MIN_FIT_DAYS * scale))),
            min(F.MIN_COMMON_DAYS, int(math.ceil(F.MIN_COMMON_DAYS * scale))))


# ------------------------------------------------------------------------------ TRAIN inputs
def read_admission_csv(path: Path) -> dict[str, dict]:
    with open(path, newline="", encoding="utf-8") as fh:
        rows = {r["id"]: r for r in csv.DictReader(fh)}
    need(rows, f"{path}: empty admission table")
    return rows


def load_train_panel(F, paths) -> SimpleNamespace:
    """Every candidate's unsigned TRAIN factor record, bound to the frozen fit's provenance."""
    lib_bytes = Path(paths.library).read_bytes()
    lib_sha = sha256_bytes(lib_bytes)
    library = F.load_library(paths.library, lib_sha)
    lib_json = json.loads(lib_bytes)
    cw_bytes = (Path(paths.weights_dir) / "composition_weights.json").read_bytes()
    cw = json.loads(cw_bytes)
    prov = cw["provenance"]
    need(cw.get("library_sha256") == lib_sha, "library differs from the frozen weights' library_sha256")
    role_sha = sha256_file(paths.train_role)
    need(role_sha == cw.get("train_manifest_sha256"), "TRAIN role manifest differs from the frozen weights' pin")
    role = F.RoleManifest(paths.train_role, role_sha)  # TRAIN-only seal
    admission = read_admission_csv(Path(paths.weights_dir) / "admission.csv")
    ids = [c["id"] for c in library]
    need(set(admission) == set(ids), "admission.csv ids differ from the library")
    factors_dir = Path(paths.work_dir) / role_sha / prov["semantics_tag"] / "factors"
    t = role.end - role.begin
    records = []
    for cand in library:
        cid = cand["id"]
        a = admission[cid]
        payload = a["cache_payload_sha256"]
        fields = prov.get("fields_manifest_sha256") if a["cache_entry"] == "fields" else None
        path = factors_dir / (f"{payload}.json" if fields is None else f"{payload}.f-{fields}.json")
        j = json.loads(path.read_bytes())
        body = {k: v for k, v in j.items() if k != "content_sha256"}
        need(j.get("content_sha256") == sha256_bytes(F.canonical_compact(body)), f"factor record seal: {cid}")
        need(j.get("candidate_id") == cid and j.get("dsl_sha256") == cand["dsl_sha256"] and
             j.get("cache_payload_sha256") == payload and j.get("fields_manifest_sha256") == fields and
             j.get("role_manifest_sha256") == role_sha, f"factor record identity: {cid}")
        need(j.get("context_sha256") == prov["context_sha256"] and j.get("script_sha256") == prov["script_sha256"],
             f"factor record is not the frozen fit's (context/script SHA): {cid}")
        need(j.get("decisions") == t and isinstance(j.get("f_unsigned"), list) and len(j["f_unsigned"]) == t,
             f"factor record extent: {cid}")
        records.append(j)
    factors = np.array([[np.nan if v is None else v for v in r["f_unsigned"]] for r in records], dtype=np.float64)
    sessions = role.sessions[role.begin:role.end]
    need(int(sessions[-1]) < F.TRAIN_END_NS, "TRAIN decisions after 2022-12-31")
    return SimpleNamespace(
        ids=ids, families=[c["family"] for c in library], library_json=lib_json, factors=factors,
        taus=[r["tau"] for r in records], sessions=sessions, years=years_of(sessions),
        fit_mask=(sessions >= F.FIT_BEGIN_NS) & (sessions < F.HOLD_BEGIN_NS),
        hold_mask=(sessions >= F.HOLD_BEGIN_NS) & (sessions < F.TRAIN_END_NS),
        admission=admission, cw=cw, cw_sha=sha256_bytes(cw_bytes), prov=prov, role=role, role_sha=role_sha,
        library_sha=lib_sha)


def frozen_series(P) -> np.ndarray:
    """The frozen book's in-sample F_t from composition_weights.json weights and signs."""
    w = np.array([float(P.cw["weights"].get(i, 0.0)) for i in P.ids])
    s = np.array([float(P.cw["signs"].get(i, 0)) for i in P.ids])
    return (w * s) @ np.where(np.isnan(P.factors), 0.0, P.factors)


def by_year(series: np.ndarray, years: np.ndarray, mask: np.ndarray | None = None) -> dict:
    mask = np.ones(series.size, dtype=bool) if mask is None else mask
    total = float(series[mask].sum())
    out = {}
    for y in sorted(set(years[mask].tolist())):
        k = mask & (years == y)
        st = ann_stats(series[k])
        st["pnl_share"] = st["pnl"] / total if total != 0 else None
        out[int(y)] = st
    return out


# ------------------------------------------------------------------------------ section A
def section_a(F, P) -> dict:
    t = P.factors.shape[1]
    res = run_protocol(F, P.factors, P.taus, P.ids, P.fit_mask, P.hold_mask, np.ones(t, dtype=bool))
    frozen_w = np.array([float(P.cw["weights"].get(i, 0.0)) for i in P.ids])
    frozen_raw = {c["id"]: c.get("mv_solution") for c in P.prov.get("candidates", [])}
    raw_diffs = [abs(res["raw"][k] - frozen_raw[P.ids[k]]) for k in res["active"]
                 if frozen_raw.get(P.ids[k]) is not None]
    status_mismatch = [i for i, r in zip(P.ids, res["rows"]) if r["status"] != P.admission[i]["status"]]
    sign_mismatch = [i for i, r in zip(P.ids, res["rows"]) if str(r["s_k"]) != P.admission[i]["s_k"]]
    pinned_sign_mismatch = [i for i, s in zip(P.ids, res["signs"]) if s != 0 and P.cw["signs"].get(i) != s]
    admitted_ids = [P.ids[k] for k in res["admitted"]]
    frozen_admitted = [i for i in P.ids if P.admission[i]["status"] == "admitted"]
    blend = None
    if res["error"] is None:
        blend = F.factor_stats(res["weights"][res["active"]] @ res["matrix"])
    frozen_blend = P.prov.get("blend_in_sample_TRAIN_diagnostic", {})
    series = book_series(res)
    return {
        "reproduction": {
            "weights_max_abs_diff": float(np.max(np.abs(res["weights"] - frozen_w))),
            "weights_bitwise_equal": bool(np.array_equal(res["weights"], frozen_w)),
            "mv_solution_max_abs_diff": max(raw_diffs) if raw_diffs else None,
            "admitted_equal": sorted(admitted_ids) == sorted(frozen_admitted),
            "admitted_count": len(admitted_ids), "frozen_admitted_count": len(frozen_admitted),
            "nonzero_weights": int((res["weights"] > 0).sum()),
            "status_mismatch_ids": status_mismatch, "screen_sign_mismatch_ids": sign_mismatch,
            "pinned_sign_mismatch_ids": pinned_sign_mismatch, "fit_error": res["error"],
            "counts": {s: sum(1 for r in res["rows"] if r["status"] == s) for s in F.ADMISSION_STATUSES},
            "blend_diagnostic_recomputed": blend, "blend_diagnostic_frozen": frozen_blend,
            "fitter_script_sha256_current": F.SCRIPT_SHA256,
            "fitter_script_sha256_frozen": P.prov.get("script_sha256"),
            "fitter_unchanged": F.SCRIPT_SHA256 == P.prov.get("script_sha256"),
            "composition_weights_sha256": P.cw_sha,
            "composition_weights_is_frozen_pin": P.cw_sha == FROZEN_WEIGHTS_SHA,
        },
        "admitted_ids": admitted_ids,
        "in_sample": dict(ann_stats(series), by_year=by_year(series, P.years),
                          first_decision=str(P.sessions[0].astype("datetime64[ns]").astype("datetime64[D]")),
                          last_decision=str(P.sessions[-1].astype("datetime64[ns]").astype("datetime64[D]"))),
        "screen_diagnostics": screen_diagnostics(res["rows"]),
    }


# ------------------------------------------------------------------------------ section B
def split_masks(P, orient_y, hold_y, score_y):
    orient = window_mask(P.sessions, orient_y)
    hold = window_mask(P.sessions, hold_y) if hold_y is not None else None
    weight = orient | hold if hold is not None else orient
    return orient, hold, weight, window_mask(P.sessions, score_y)


def section_b(F, P) -> dict:
    frozen_fit_days = int(P.fit_mask.sum())
    frozen_admitted = {i for i in P.ids if P.admission[i]["status"] == "admitted"}
    out = {"splits": [], "notes": [
        "IS = F over the weight window (orient | HOLD), OOS = F over the untouched score window.",
        "HOLD None: HOLD-sign test off (screen_v3 gets hold = orient). tau_k is the full-TRAIN value from the "
        "work cache (structural; the tau screen rejects nothing at 0.70 in v3).",
        "Day minima (250 live orient days, 250 common redundancy days) are pro-rated to the orient window "
        "length (exactly 250 for a 2020-21 orient window)."]}
    for sid, desc, orient_y, hold_y, score_y in SPLITS_B:
        orient, hold, weight, score = split_masks(P, orient_y, hold_y, score_y)
        min_fit, min_common = scaled_thresholds(F, orient, frozen_fit_days)
        res = run_protocol(F, P.factors, P.taus, P.ids, orient, hold, weight, min_fit, min_common)
        series = book_series(res)
        adm = [P.ids[k] for k in res["admitted"]]
        out["splits"].append({
            "id": sid, "description": desc, "orient_years": orient_y, "hold_years": hold_y, "score_years": score_y,
            "orient_days": int(orient.sum()), "weight_days": int(weight.sum()), "score_days": int(score.sum()),
            "min_fit_days": min_fit, "min_common_days": min_common,
            "counts": {s: sum(1 for r in res["rows"] if r["status"] == s) for s in F.ADMISSION_STATUSES},
            "admitted": len(adm), "weighted": int((res["weights"] > 0).sum()), "fit_error": res["error"],
            "overlap_with_frozen_admitted": len(frozen_admitted & set(adm)), "admitted_ids": adm,
            "is": ann_stats(series[weight]), "oos": ann_stats(series[score])})
    return out


# ------------------------------------------------------------------------------ section C
def library_prior_signs(P) -> tuple[np.ndarray | None, str]:
    rows = P.library_json.get("candidates", [])
    prior = np.zeros(len(P.ids))
    found = 0
    for k, row in enumerate(rows):
        for key in PRIOR_SIGN_KEYS:
            v = row.get(key)
            if type(v) is int and v in (-1, 1):
                prior[k] = v
                found += 1
                break
    policies = sorted({str(r.get("sign_policy")) for r in rows})
    if not found:
        return None, (f"skipped: the library declares no prior sign (no {'/'.join(PRIOR_SIGN_KEYS)} field; "
                      f"sign_policy values {policies} are TRAIN-estimated)")
    return prior, f"{found} candidates carry a declared prior sign"


def section_c(F, P) -> dict:
    frozen_fit_days = int(P.fit_mask.sum())
    zf = np.where(np.isnan(P.factors), 0.0, P.factors)
    prior, prior_note = library_prior_signs(P)
    fam_of = np.array(P.families)
    out = {"rules": [{"id": r, "description": d} for r, d in RULES_C], "splits": [], "notes": [prior_note]}
    for sid, desc, orient_y, hold_y, score_y in SPLITS_C:
        orient, hold, weight, score = split_masks(P, orient_y, hold_y, score_y)
        min_fit, min_common = scaled_thresholds(F, orient, frozen_fit_days)
        res = run_protocol(F, P.factors, P.taus, P.ids, orient, hold, weight, min_fit, min_common)
        s_fit = np.asarray(res["signs"], dtype=np.float64)
        adm = np.zeros(len(P.ids), dtype=bool)
        adm[res["admitted"]] = True
        books: dict[str, tuple[np.ndarray, np.ndarray] | str] = {}
        nz = s_fit != 0
        books["C1"] = (nz / max(nz.sum(), 1), s_fit)
        books["C2"] = prior_note if prior is None else ((prior != 0) / max((prior != 0).sum(), 1), prior)
        books["C3"] = (adm / max(adm.sum(), 1), s_fit) if adm.any() else "nothing admitted"
        if adm.any():
            sd = np.array([float(np.std(s_fit[k] * zf[k][weight], ddof=1)) if adm[k] else 0.0
                           for k in range(len(P.ids))])
            inv = np.where(adm & (sd > 0), 1.0 / np.where(sd > 0, sd, 1.0), 0.0)
            books["C4"] = (inv / inv.sum(), s_fit) if inv.sum() > 0 else "degenerate"
        else:
            books["C4"] = "nothing admitted"
        books["C5"] = (res["weights"], s_fit) if res["error"] is None else f"no book: {res['error']}"
        fams = sorted({f for f, keep in zip(fam_of, nz) if keep})
        w6 = np.zeros(len(P.ids))
        for f in fams:
            members = (fam_of == f) & nz
            w6[members] = 1.0 / (len(fams) * members.sum())
        books["C6"] = (w6, s_fit) if fams else "no oriented candidate"
        cells = {}
        for rid, _ in RULES_C:
            b = books[rid]
            if isinstance(b, str):
                cells[rid] = {"skipped": b}
                continue
            w, s = b
            series = (w * s) @ zf
            cells[rid] = {"n": int((w > 0).sum()), "is_sharpe": sharpe(series[weight]),
                          "oos_sharpe": sharpe(series[score]), "is_mean_ann": float(series[weight].mean()) * ANN,
                          "oos_mean_ann": float(series[score].mean()) * ANN}
        out["splits"].append({"id": sid, "description": desc, "orient_years": orient_y, "score_years": score_y,
                              "min_fit_days": min_fit, "min_common_days": min_common,
                              "admitted": int(adm.sum()), "families": len(fams), "cells": cells})
    return out


# ------------------------------------------------------------------------------ section D
def section_d(F, P, reps: int, seed: int, deadline: float) -> dict:
    """Null runs for one seed; pooled with other seeds' runs by merge_d()."""
    observed = protocol_stats(F, P.factors, P.taus, P.ids, P.fit_mask, P.hold_mask)
    with warnings.catch_warnings():  # an all-flat row has no mean: it stays all-NaN
        warnings.simplefilter("ignore", RuntimeWarning)
        x = P.factors - np.nanmean(P.factors, axis=1, keepdims=True)  # zero mean over live days; NaN stays
    t = x.shape[1]
    samples = {"is_sharpe": [], "admitted": [], "spearman": [], "frac_kept": [], "no_book": []}
    slowest, stopped = 0.0, False
    for rep in range(reps):
        if time.perf_counter() + 1.5 * slowest > deadline:
            stopped = True
            break
        tick = time.perf_counter()
        idx = block_bootstrap_indices(t, BLOCK, np.random.default_rng([seed, rep]))
        st = protocol_stats(F, x[:, idx], P.taus, P.ids, P.fit_mask, P.hold_mask)
        for key in samples:
            samples[key].append(st[key])
        slowest = max(slowest, time.perf_counter() - tick)
    return {"observed": observed, "run": {"seed": seed, "reps_requested": reps, "reps_done": len(samples["admitted"]),
                                          "stopped_early_for_budget": stopped, "block": BLOCK,
                                          "seconds_per_rep_max": slowest, "samples": samples}}


def merge_d(previous: dict | None, new: dict) -> dict:
    """Pool this seed's run with earlier seeds' runs (same seed replaces; never double counted).

    Earlier runs are discarded when their observed statistics differ from this run's (inputs changed).
    """
    runs = dict((previous or {}).get("runs", {}))
    discarded = []
    if previous is not None and previous.get("observed") != clean(new["observed"]):
        discarded, runs = sorted(runs, key=int), {}
    runs[str(new["run"]["seed"])] = new["run"]
    pooled = {k: [] for k in ("is_sharpe", "admitted", "spearman", "frac_kept", "no_book")}
    for key in sorted(runs, key=int):
        for k in pooled:
            pooled[k].extend(runs[key]["samples"][k])
    obs = new["observed"]
    no_book = sum(1 for v in pooled["no_book"] if v)
    is0 = [0.0 if v is None else v for v in pooled["is_sharpe"]]
    return {
        "observed": obs, "runs": runs, "reps_total": len(pooled["admitted"]), "seeds": sorted(runs, key=int),
        "no_book_reps": no_book, "discarded_seeds_inputs_changed": discarded,
        "null": {"is_sharpe": null_summary(obs["is_sharpe"], pooled["is_sharpe"]),
                 "is_sharpe_no_book_as_0": null_summary(obs["is_sharpe"], is0),
                 "admitted": null_summary(obs["admitted"], [float(v) for v in pooled["admitted"]]),
                 "spearman": null_summary(obs["spearman"], pooled["spearman"]),
                 "frac_kept": null_summary(obs["frac_kept"], pooled["frac_kept"])},
        "method": ("each f_k demeaned over its live TRAIN days; circular block bootstrap of decision dates "
                   f"(block {BLOCK}, one index vector for all candidates, NaN flats travel with dates); frozen "
                   "protocol: FIT-mean orientation, tau, HOLD-sign screen, greedy redundancy, mv-shrink on all "
                   "decisions; rep r of seed S uses default_rng([S, r])")}


# ------------------------------------------------------------------------------ section E
def make_any_role(F):
    class AnyRole(F.RoleManifest):
        """RoleManifest without the TRAIN-only seal (book-level validation check). Same receipts."""

        def __init__(self, manifest: Path, pin: str):  # noqa: D401  (deliberately skips the TRAIN seal)
            j = F.unique_json(F.pinned_bytes(manifest, pin, "role manifest"), "role manifest")
            F.require(j.get("schema") == F.ROLE_SCHEMA and j.get("status") == "complete", "role: schema/status")
            self.sha = pin
            self.dates, self.instruments = int(j["dates"]), int(j["instruments"])
            self.score_begin, self.score_end = int(j["score_begin"]), int(j["score_end"])
            self.source_sha256 = j.get("source_sha256")
            d, n = self.dates, self.instruments
            F.require(0 < d <= 4096 and 0 < n <= 20000, "role: dimensions")
            F.require(0 <= self.score_begin < self.score_end == d, "role: score window")
            F.require(self.score_end - 2 - self.score_begin >= 2, "role: fewer than two scored decisions")
            self._files, self._base = j["files"], Path(manifest).parent
            self.sessions = self._read("sessions.i64", "<i8", d)
            self.ids = self._read("ids.u64", "<u8", n)
            s = self.sessions
            F.require(s[0] > 0 and bool(np.all(np.diff(s) > 0)) and bool(np.all(s % F.DAY_NS == 0)),
                      "role: session axis")
            F.require(self.ids[0] != 0 and bool(np.all(self.ids[1:] > self.ids[:-1])), "role: instrument axis")
            self.begin, self.end = self.score_begin, self.score_end - 2

    return AnyRole


def load_combined(run_dir: Path, prefix: str, role) -> tuple[np.ndarray, dict, str]:
    """<prefix>_combined.*: receipt-verified, bound to the role axes; NaN outside member & finite."""
    meta_bytes = (Path(run_dir) / f"{prefix}_combined.json").read_bytes()
    meta = json.loads(meta_bytes)
    need(meta.get("schema") == COMBINED_SCHEMA and meta.get("status") == "complete", f"{prefix} combined: schema")
    need(int(meta["dates"]) == role.dates and int(meta["instruments"]) == role.instruments,
         f"{prefix} combined: axes differ from the role")
    need(meta.get("role_manifest_sha256") == role.sha, f"{prefix} combined: bound to another role manifest")
    need(int(meta["score_begin"]) == role.score_begin and int(meta["score_end"]) == role.score_end,
         f"{prefix} combined: score window differs from the role")
    d, n = role.dates, role.instruments

    def arr(suffix, dtype, shape):
        name = f"{prefix}_combined{suffix}"
        rec = meta["files"][name]
        data = (Path(run_dir) / name).read_bytes()
        need(len(data) == int(rec["bytes"]) == int(np.prod(shape)) * np.dtype(dtype).itemsize,
             f"{prefix} combined: extent {name}")
        need(sha256_bytes(data) == rec["sha256"], f"{prefix} combined: SHA-256 {name}")
        return np.frombuffer(data, dtype=dtype).reshape(shape)

    need(np.array_equal(arr("_sessions.i64", "<i8", (d,)), role.sessions) and
         np.array_equal(arr("_ids.u64", "<u8", (n,)), role.ids), f"{prefix} combined: session/id axes differ")
    member = arr("_member.u8", "u1", (d, n)).astype(bool)
    combined = arr(".f64", "<f8", (d, n))
    combined = np.where(member & np.isfinite(combined), combined, np.nan)
    return combined, meta, sha256_bytes(meta_bytes)


def plain_paper(NR, combined: np.ndarray, r0: np.ndarray, begin: int, end: int, lags=LAGS):
    """nav_recon.desired_target (centered tied rank over member&finite, demeaned, gross 1) per decision d,
    p_L(d) = w(d) . r0[d+L] (r0 = the fitter's valid guarded returns, invalid -> 0)."""
    d_count, t = combined.shape[0], end - begin
    out = {lag: np.full(t, np.nan) for lag in lags}
    turn, prev, flat = [], None, 0
    for j in range(t):
        d = begin + j
        row = combined[d]
        w = NR.desired_target(row, np.isfinite(row))
        if not w.any():
            flat += 1
        for lag in lags:
            if d + lag < d_count:
                out[lag][j] = float(w @ r0[d + lag])
        if prev is not None:
            turn.append(float(np.abs(w - prev).sum()))
        prev = w
    return out, (float(np.mean(turn)) if turn else None), flat


def neutral_paper(q: np.ndarray, r0: np.ndarray, columns: np.ndarray, begin: int, lags=LAGS):
    """p_L(d) = q(d) . r0[d+L] on the context columns (q = F.Context.book of the combined)."""
    t, d_count = q.shape[0], r0.shape[0]
    out = {}
    for lag in lags:
        s = np.full(t, np.nan)
        rows = max(0, min(t, d_count - begin - lag))
        if rows:
            s[:rows] = (q[:rows] * r0[begin + lag:begin + lag + rows][:, columns]).sum(axis=1)
        out[lag] = s
    return out


def read_nav(path: Path) -> dict[int, tuple[float, float]]:
    out = {}
    with open(path, newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            if row.get("return_observation") == "1":
                out[int(row["session_ns"])] = (float(row["gross_return"]), float(row["net_return"]))
    return out


def compare_candidates(records, signs: dict, weights: dict) -> dict:
    """Counts only (never ids): pinned sign / weight equality of one run's per-candidate metadata."""
    seen = {}
    for cid, sign, weight in records:
        if isinstance(cid, str):
            seen[cid] = (sign, weight)
    lib = set(weights)
    common = sorted(lib & set(seen))
    sign_bad = sum(1 for c in common if seen[c][0] != signs.get(c, 0))
    sign_bad_w = sum(1 for c in common if weights[c] > 0 and seen[c][0] != signs.get(c, 0))
    diffs = [abs(float(seen[c][1]) - float(weights[c])) for c in common
             if isinstance(seen[c][1], (int, float)) and not isinstance(seen[c][1], bool)]
    return {"records": len(seen), "compared": len(common), "missing": len(lib - set(seen)),
            "extra": len(set(seen) - lib), "sign_mismatches": sign_bad, "sign_mismatches_weighted": sign_bad_w,
            "weight_values": len(diffs), "weight_max_abs_diff": max(diffs) if diffs else None,
            "weights_exact": bool(diffs) and len(diffs) == len(common) and max(diffs) == 0.0}


def metadata_check(run_dir: Path, prefix: str, cw_bytes: bytes, role_sha: str, comb_meta: dict,
                   comb_meta_sha: str, nav_recipe: dict | None) -> dict:
    """Did this run use the pinned signs and weights? Equality/count results only, no candidate ids."""
    cw = json.loads(cw_bytes)
    cw_sha = sha256_bytes(cw_bytes)
    signs, weights = cw["signs"], cw["weights"]
    out = {"composition_weights_sha256": cw_sha, "composition_weights_is_frozen_pin": cw_sha == FROZEN_WEIGHTS_SHA,
           "combined_binds_weights": comb_meta.get("composition_weights_sha256") == cw_sha,
           "combined_sign_mode": comb_meta.get("composition_signs"),
           "combined_binds_library": comb_meta.get("library_sha256") == cw.get("library_sha256")}
    summary = Path(run_dir) / "summary.json"
    if summary.is_file():
        s = json.loads(summary.read_bytes())
        out["summary_binds_weights"] = s.get("composition_weights_sha256") == cw_sha
        roles = s.get("roles") if isinstance(s.get("roles"), list) else []
        for entry in roles:
            if not isinstance(entry, dict):
                continue
            recs = [(c.get("id"), c.get("composition_sign"), c.get("composition_weight"))
                    for c in entry.get("candidates", []) if isinstance(c, dict)]
            out[f"summary_role_{entry.get('role')}"] = compare_candidates(recs, signs, weights)
    else:
        out["summary"] = "absent"
    jsonl = Path(run_dir) / f"{prefix}_candidates.jsonl"
    if jsonl.is_file():
        recs = []
        for line in jsonl.read_text(encoding="utf-8").splitlines():
            if line.strip():
                j = json.loads(line)
                if "composition_weight" in j or "composition_sign" in j:
                    recs.append((j.get("id"), j.get("composition_sign"), j.get("composition_weight")))
        out[f"{prefix}_candidates_jsonl"] = compare_candidates(recs, signs, weights)
    else:
        out[f"{prefix}_candidates_jsonl"] = "absent"
    if nav_recipe is not None:
        out["nav_binds_combined"] = nav_recipe.get("combined_sha256") == comb_meta_sha
        out["nav_binds_role"] = nav_recipe.get("role_sha256") == role_sha
        out["nav_cadence"] = nav_recipe.get("cadence")
        out["nav_rule"] = nav_recipe.get("rule")
    return out


def aligned(series: np.ndarray, sessions: np.ndarray, begin: int, lag: int, nav: dict, which: int = 0):
    xs, ys = [], []
    for j, v in enumerate(series):
        k = begin + j + lag
        if math.isfinite(v) and k < sessions.size and int(sessions[k]) in nav:
            xs.append(v)
            ys.append(nav[int(sessions[k])][which])
    return np.array(xs), np.array(ys)


def e_role(F, NR, *, train: bool, manifest: Path, run_dir: Path, prefix: str, nav_dir: Path, scenario: str,
           cw_bytes: bytes, context_dir: Path | None = None, frozen: np.ndarray | None = None,
           frozen_context_sha: str | None = None, log=None) -> dict:
    """Book-level paper book of one role's saved combined vs its NAV. No per-candidate output."""
    msha = sha256_file(manifest)
    role = F.RoleManifest(manifest, msha) if train else make_any_role(F)(manifest, msha)
    combined, comb_meta, comb_meta_sha = load_combined(run_dir, prefix, role)
    t = role.end - role.begin
    # plain book: returns from the fitter's PricePanel (valid guarded, invalid -> 0)
    p = role.payload()
    panel = F.PricePanel(p["close"], p["raw_close"], p["volume"], p["present"])
    del p
    r0 = np.where(np.isnan(panel.returns), 0.0, panel.returns)
    del panel
    plain, tau_plain, flat_plain = plain_paper(NR, combined, r0, role.begin, role.end)
    # neutral book: the fitter's Context (work cache for TRAIN when it verifies, else rebuilt)
    ctx = F.Context.load(context_dir, role) if context_dir is not None else None
    source = "work-cache" if ctx is not None else "built"
    if ctx is None:
        ctx = F.Context.build(role, log)
    q, live = ctx.book(combined, 1)
    columns, digest, refused = ctx.columns, ctx.digest, len(ctx.refused)
    fwd_check = float(np.max(np.abs((q * ctx.forward).sum(axis=1) - neutral_paper(q, r0, columns, role.begin, (2,))[2])))
    del ctx, combined
    neutral = neutral_paper(q, r0, columns, role.begin)
    tau_neutral = F.standalone_turnover(q)
    del q, r0
    years = years_of(role.sessions[role.begin:role.end])
    books = {"plain": plain, "neutral": neutral}
    paper = {}
    for name, series in books.items():
        paper[name] = {f"lag{lag}": ann_stats(series[lag]) for lag in LAGS}
        paper[name]["by_year_lag2"] = {int(y): sharpe(series[2][years == y]) for y in sorted(set(years.tolist()))}
    paper["plain"]["tau_mean"], paper["plain"]["flat_decisions"] = tau_plain, flat_plain
    paper["neutral"]["tau_mean"], paper["neutral"]["flat_decisions"] = tau_neutral, int(t - live.sum())
    nav_path = Path(nav_dir) / f"daily_{scenario}.csv"
    recipe_path = Path(nav_dir) / "recipe.json"
    recipe = json.loads(recipe_path.read_bytes()) if recipe_path.is_file() else None
    nav_out = {"file": str(nav_path)}
    if nav_path.is_file():
        nav = read_nav(nav_path)
        g = np.array([v[0] for _, v in sorted(nav.items())])
        n_ = np.array([v[1] for _, v in sorted(nav.items())])
        nav_out.update(return_rows=len(nav), gross=ann_stats(g), net=ann_stats(n_), corr={})
        for name, series in books.items():
            cells = {}
            for lag in LAGS:
                xs, ys = aligned(series[lag], role.sessions, role.begin, lag, nav)
                cells[f"lag{lag}"] = {"n": int(xs.size), "corr_gross": corr(xs, ys),
                                      "paper_sharpe_aligned": sharpe(xs), "nav_gross_sharpe_aligned": sharpe(ys)}
            best = max((c for c in cells if cells[c]["corr_gross"] is not None),
                       key=lambda c: cells[c]["corr_gross"], default=None)
            cells["best_lag"] = best
            nav_out["corr"][name] = cells
    else:
        nav_out["status"] = "absent"
    out = {"role": "train" if train else "validation", "role_manifest_sha256": msha,
           "role_manifest_is_frozen_pin": msha == (FROZEN_TRAIN_ROLE_SHA if train else FROZEN_VAL_ROLE_SHA),
           "decisions": t,
           "first_decision": NR.date_of(role.sessions[role.begin]), "last_decision": NR.date_of(role.sessions[role.end - 1]),
           "context": {"source": source, "digest": digest, "refused_decisions": refused,
                       "matches_frozen_fit_context": (digest == frozen_context_sha) if frozen_context_sha else None,
                       "forward_lag2_max_abs_diff": fwd_check},
           "paper": paper, "nav": nav_out,
           "metadata": metadata_check(run_dir, prefix, cw_bytes, msha, comb_meta, comb_meta_sha, recipe),
           "semantics": ("plain = nav_recon.desired_target (tied rank over member&finite, demeaned, gross 1); "
                         "neutral = fit_composition_weights.Context.book (tied rank over used&finite, OLS "
                         "residual on [1,z_beta,z_vol,z_ladv], gross 1; the fitter's FACTOR_SEMANTICS); "
                         "p_L(d) = book(d) . r[d+L], r valid-guarded else 0; NAV row aligned at session d+L. "
                         "Not modelled: NAV band, partial fills, costs, neutralize-guard skips, stale carry.")}
    if frozen is not None:
        out["frozen_factor_blend"] = {"sharpe": sharpe(frozen), "corr_neutral_lag2": corr(frozen, np.nan_to_num(neutral[2])),
                                      "corr_plain_lag2": corr(frozen, np.nan_to_num(plain[2]))}
    return out


def section_e(F, NR, paths, roles, get_panel, log=None) -> dict:
    cw_bytes = (Path(paths.weights_dir) / "composition_weights.json").read_bytes()
    out = {}
    if "train" in roles:
        P = get_panel()
        ctx_dir = Path(paths.work_dir) / P.role_sha / P.prov["semantics_tag"] / "context"
        out["train"] = e_role(F, NR, train=True, manifest=paths.train_role, run_dir=paths.train_run, prefix="train",
                              nav_dir=paths.train_nav, scenario=paths.scenario, cw_bytes=cw_bytes,
                              context_dir=ctx_dir, frozen=frozen_series(P), frozen_context_sha=P.prov["context_sha256"],
                              log=log)
    if "val" in roles:
        out["val"] = e_role(F, NR, train=False, manifest=paths.val_role, run_dir=paths.val_run, prefix="validation",
                            nav_dir=paths.val_nav, scenario=paths.scenario, cw_bytes=cw_bytes, log=log)
    return out


# ------------------------------------------------------------------------------ section F
def section_f(P) -> dict:
    w = np.array([float(P.cw["weights"].get(i, 0.0)) for i in P.ids])
    s = np.array([float(P.cw["signs"].get(i, 0)) for i in P.ids])
    zf = np.where(np.isnan(P.factors), 0.0, P.factors)
    contrib = (w * s)[:, None] * zf
    per_k = contrib.sum(axis=1)
    total = float(per_k.sum())
    years = sorted(set(P.years.tolist()))
    yk = {y: contrib[:, P.years == y].sum(axis=1) for y in years}
    rows = []
    for k in np.flatnonzero(w > 0):
        rows.append({"id": P.ids[k], "family": P.families[k], "weight": w[k], "sign": int(s[k]), "pnl": per_k[k],
                     "share": per_k[k] / total if total else None,
                     "by_year": {int(y): float(yk[y][k]) for y in years},
                     "standalone_sharpe": sharpe(s[k] * zf[k]),
                     "fit_sharpe": _float(P.admission[P.ids[k]].get("fit_sharpe")),
                     "hold_sharpe": _float(P.admission[P.ids[k]].get("hold_sharpe"))})
    rows.sort(key=lambda r: -r["pnl"])
    fams = {}
    for r in rows:
        f = fams.setdefault(r["family"], {"family": r["family"], "candidates": 0, "weight": 0.0, "pnl": 0.0,
                                          "by_year": {int(y): 0.0 for y in years}})
        f["candidates"] += 1
        f["weight"] += r["weight"]
        f["pnl"] += r["pnl"]
        for y in years:
            f["by_year"][int(y)] += r["by_year"][int(y)]
    fam_rows = sorted(fams.values(), key=lambda f: -f["pnl"])
    for f in fam_rows:
        f["share"] = f["pnl"] / total if total else None
    pnls = np.array([r["pnl"] for r in rows])
    top5 = float(np.sort(pnls)[::-1][:5].sum()) if pnls.size else 0.0
    abs_sorted = np.sort(np.abs(pnls))[::-1]
    return {"total_pnl": total, "by_year_total": {int(y): float(yk[y].sum()) for y in years},
            "top5_share_of_total": top5 / total if total else None,
            "top5_abs_share_of_abs": float(abs_sorted[:5].sum() / abs_sorted.sum()) if abs_sorted.sum() > 0 else None,
            "negative_contributors": int((pnls < 0).sum()), "candidates": rows, "families": fam_rows,
            "note": "P&L = sum over TRAIN decisions of w_k s_k f_k,t (gross-1 factor units, flat days 0)."}


def _float(v):
    try:
        x = float(v)
        return x if math.isfinite(x) else None
    except (TypeError, ValueError):
        return None


# ------------------------------------------------------------------------------ markdown
def f3(v, fmt="{:+.3f}") -> str:
    return "n/a" if v is None else fmt.format(v)


def md_a(a: dict) -> list[str]:
    r, i, s = a["reproduction"], a["in_sample"], a["screen_diagnostics"]
    lines = ["## A. Reproduce the frozen TRAIN fit", "",
             f"- weights max |dw| = {r['weights_max_abs_diff']:.3e} (bitwise equal: {r['weights_bitwise_equal']}); "
             f"mv_solution max |d| = {f3(r['mv_solution_max_abs_diff'], '{:.3e}')}",
             f"- admitted set equal: {r['admitted_equal']} ({r['admitted_count']} vs frozen {r['frozen_admitted_count']}); "
             f"status mismatches {len(r['status_mismatch_ids'])}, screen sign mismatches {len(r['screen_sign_mismatch_ids'])}, "
             f"pinned sign mismatches {len(r['pinned_sign_mismatch_ids'])}; counts {r['counts']}",
             f"- fitter unchanged since the freeze: {r['fitter_unchanged']}; weights file is the frozen pin: "
             f"{r['composition_weights_is_frozen_pin']}",
             f"- screen: FIT-vs-HOLD Spearman {f3(s['spearman_fit_hold_sharpe'])} (p {f3(s['spearman_p'], '{:.2f}')}, "
             f"n {s['spearman_n']}); sign kept in HOLD {s['n_kept']}/{s['n_oriented']} = {f3(s['frac_sign_kept'], '{:.3f}')}",
             "", f"In-sample F_t ({i['first_decision']}..{i['last_decision']}, {i['days']} decisions): "
             f"SR {f3(i['sharpe'])}, mean {f3(i['mean_ann'], '{:+.4f}')}/yr, vol {f3(i['vol_ann'], '{:.4f}')}/yr", "",
             "| year | days | SR | mean/yr | P&L | share |", "|---|---|---|---|---|---|"]
    for y, v in i["by_year"].items():
        lines.append(f"| {y} | {v['days']} | {f3(v['sharpe'])} | {f3(v['mean_ann'], '{:+.4f}')} | "
                     f"{v['pnl']:+.4f} | {f3(v['pnl_share'], '{:.1%}')} |")
    return lines + [""]


def md_b(b: dict) -> list[str]:
    lines = ["## B. Walk-forward process estimate (TRAIN only)", "", *[f"- {n}" for n in b["notes"]], "",
             "| split | description | min days | admitted (overlap frozen) | weighted | IS SR | OOS SR | OOS mean/yr |",
             "|---|---|---|---|---|---|---|---|"]
    for s in b["splits"]:
        lines.append(f"| {s['id']} | {s['description']} | {s['min_fit_days']} | {s['admitted']} "
                     f"({s['overlap_with_frozen_admitted']}) | {s['weighted']} | {f3(s['is']['sharpe'])} | "
                     f"{f3(s['oos']['sharpe'])} | {f3(s['oos']['mean_ann'], '{:+.4f}')} |")
    return lines + [""]


def md_c(c: dict) -> list[str]:
    heads = [s["id"] for s in c["splits"]]
    lines = ["## C. Construction alternatives (IS SR / OOS SR)", "", *[f"- {n}" for n in c["notes"]], "",
             "| rule | " + " | ".join(f"{h} IS | {h} OOS" for h in heads) + " |",
             "|---|" + "---|---|" * len(heads)]
    for rid, desc in ((r["id"], r["description"]) for r in c["rules"]):
        cells = []
        for s in c["splits"]:
            cell = s["cells"][rid]
            if "skipped" in cell:
                cells += ["skip", "skip"]
            else:
                cells += [f3(cell["is_sharpe"]), f3(cell["oos_sharpe"])]
        lines.append(f"| {rid} {desc} | " + " | ".join(cells) + " |")
    return lines + [""]


def md_d(d: dict) -> list[str]:
    lines = ["## D. Selection-bias null (TRAIN)", "", f"- {d['method']}",
             f"- reps {d['reps_total']} (seeds {', '.join(d['seeds'])}); no-book reps {d['no_book_reps']}; "
             f"seeds discarded because the inputs changed: {d.get('discarded_seeds_inputs_changed') or 'none'}", "",
             "| statistic | observed | null mean | p05 | p50 | p95 | max | percentile of observed |",
             "|---|---|---|---|---|---|---|---|"]
    for key, v in d["null"].items():
        lines.append(f"| {key} | {f3(v['observed'])} | {f3(v.get('mean'))} | {f3(v.get('p05'))} | {f3(v.get('p50'))} | "
                     f"{f3(v.get('p95'))} | {f3(v.get('max'))} | {f3(v['percentile_of_observed'], '{:.1f}')} |")
    return lines + [""]


def md_e(e: dict) -> list[str]:
    lines = ["## E. Paper-book math check (book level)", ""]
    for key in ("train", "val"):
        if key not in e:
            continue
        r = e[key]
        lines += [f"### {r['role']} ({r['first_decision']}..{r['last_decision']}, {r['decisions']} decisions)", "",
                  f"- context {r['context']['source']} (refused {r['context']['refused_decisions']}, matches frozen fit: "
                  f"{r['context']['matches_frozen_fit_context']}); role manifest is the frozen pin: {r['role_manifest_is_frozen_pin']}",
                  "", "| book | lag1 SR | lag2 SR | lag3 SR | tau mean | flat | corr NAV gross lag1 / lag2 / lag3 | best lag |",
                  "|---|---|---|---|---|---|---|---|"]
        for name in ("plain", "neutral"):
            p = r["paper"][name]
            c = r["nav"].get("corr", {}).get(name, {})
            cs = " / ".join(f3((c.get(f"lag{lag}") or {}).get("corr_gross")) for lag in LAGS) if c else "n/a"
            lines.append(f"| {name} | {f3(p['lag1']['sharpe'])} | {f3(p['lag2']['sharpe'])} | {f3(p['lag3']['sharpe'])} | "
                         f"{f3(p['tau_mean'], '{:.3f}')} | {p['flat_decisions']} | {cs} | {c.get('best_lag', 'n/a')} |")
        if "gross" in r["nav"]:
            lines.append(f"\nNAV ({r['nav']['return_rows']} return rows): gross SR {f3(r['nav']['gross']['sharpe'])}, "
                         f"net SR {f3(r['nav']['net']['sharpe'])}")
        yl = r["paper"]["neutral"]["by_year_lag2"]
        lines.append("- neutral lag2 SR by year: " + ", ".join(f"{y} {f3(v)}" for y, v in yl.items()))
        if "frozen_factor_blend" in r:
            fb = r["frozen_factor_blend"]
            lines.append(f"- frozen factor blend F_t: SR {f3(fb['sharpe'])}; corr with neutral lag2 paper "
                         f"{f3(fb['corr_neutral_lag2'])}, plain {f3(fb['corr_plain_lag2'])}")
        m = r["metadata"]
        flat = {k: v for k, v in m.items() if not isinstance(v, dict)}
        lines.append("- metadata: " + ", ".join(f"{k}={v}" for k, v in flat.items() if k != "composition_weights_sha256"))
        for k, v in m.items():
            if isinstance(v, dict):
                lines.append(f"  - {k}: compared {v['compared']}, missing {v['missing']}, extra {v['extra']}, sign "
                             f"mismatches {v['sign_mismatches']} (weighted {v['sign_mismatches_weighted']}), weight max |d| "
                             f"{f3(v['weight_max_abs_diff'], '{:.2e}')}, exact {v['weights_exact']}")
        lines.append("")
    return lines


def md_f(f: dict) -> list[str]:
    years = list(f["by_year_total"])
    lines = ["## F. TRAIN attribution of in-sample F P&L", "", f"- {f['note']}",
             f"- total {f['total_pnl']:+.4f}; by year " + ", ".join(f"{y} {v:+.4f}" for y, v in f["by_year_total"].items()),
             f"- top-5 share of total {f3(f['top5_share_of_total'], '{:.1%}')}; top-5 |share| {f3(f['top5_abs_share_of_abs'], '{:.1%}')}; "
             f"negative contributors {f['negative_contributors']}", "",
             "| family | n | weight | P&L | share | " + " | ".join(str(y) for y in years) + " |",
             "|---|---|---|---|---|" + "---|" * len(years)]
    for r in f["families"]:
        lines.append(f"| {r['family']} | {r['candidates']} | {r['weight']:.3f} | {r['pnl']:+.4f} | {f3(r['share'], '{:.1%}')} | "
                     + " | ".join(f"{r['by_year'][y]:+.4f}" for y in years) + " |")
    lines += ["", "| candidate | family | w | s | P&L | share | " + " | ".join(str(y) for y in years) +
              " | SR full | SR FIT | SR HOLD |", "|---|---|---|---|---|---|" + "---|" * (len(years) + 3)]
    for r in f["candidates"]:
        lines.append(f"| {r['id']} | {r['family']} | {r['weight']:.3f} | {r['sign']:+d} | {r['pnl']:+.4f} | "
                     f"{f3(r['share'], '{:.1%}')} | " + " | ".join(f"{r['by_year'][y]:+.4f}" for y in years) +
                     f" | {f3(r['standalone_sharpe'])} | {f3(r['fit_sharpe'])} | {f3(r['hold_sharpe'])} |")
    return lines + [""]


RENDER = {"A": md_a, "B": md_b, "C": md_c, "D": md_d, "E": md_e, "F": md_f}


def render_md(doc: dict) -> str:
    lines = ["# v3 post-mortem (T16)", "", f"schema {doc['schema']}; updated {doc.get('updated')}; "
             f"sections present: {', '.join(s for s in SECTIONS if s in doc['sections'])}", ""]
    for s in SECTIONS:
        if s in doc["sections"]:
            try:
                lines += RENDER[s](json.loads(json.dumps(doc["sections"][s])))
            except (KeyError, TypeError, ValueError) as exc:  # a table bug never loses the JSON
                lines += [f"## {s}: markdown render failed ({exc}); see postmortem.json", ""]
    return "\n".join(lines) + "\n"


# ------------------------------------------------------------------------------ output
def write_atomic(path: Path, data: bytes) -> None:
    tmp = path.with_name(path.name + f".tmp-{os.getpid()}")
    with open(tmp, "wb") as fh:
        fh.write(data)
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, path)


def load_doc(out_dir: Path) -> dict:
    path = out_dir / "postmortem.json"
    if path.is_file():
        doc = json.loads(path.read_bytes())
        need(doc.get("schema") == SCHEMA, f"{path}: not a {SCHEMA} document")
        return doc
    return {"schema": SCHEMA, "sections": {}, "timings": {}}


def save_doc(out_dir: Path, doc: dict) -> None:
    doc["updated"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    doc = clean(doc)
    write_atomic(out_dir / "postmortem.json", (json.dumps(doc, indent=1, sort_keys=True, allow_nan=False) + "\n").encode())
    write_atomic(out_dir / "postmortem.md", render_md(doc).encode("utf-8"))


# ------------------------------------------------------------------------------ CLI
def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    p.add_argument("--output", type=Path, required=True, help="output directory (postmortem.json/.md merge here)")
    p.add_argument("--sections", default="A,B,C,D,E,F", help="comma list of A..F")
    p.add_argument("--reps", type=int, default=200, help="D: bootstrap reps for this seed")
    p.add_argument("--seed", type=int, default=1, help="D: seed; runs of different seeds pool in DIR")
    p.add_argument("--e-roles", default="train,val", help="E: comma list of train,val")
    p.add_argument("--budget-seconds", type=float, default=165.0, help="D stops between reps before this")
    p.add_argument("--root", type=Path, default=DEFAULT_ROOT, help="base for every default input path")
    p.add_argument("--fitter", type=Path, default=None)
    p.add_argument("--work-dir", type=Path, default=None, help="fitter --work-dir (ROOT/<train sha>/<tag>/...)")
    p.add_argument("--weights-dir", type=Path, default=None, help="admission.csv/json + composition_weights.json")
    p.add_argument("--library", type=Path, default=None)
    p.add_argument("--train-role", type=Path, default=None, help="TRAIN role manifest.json")
    p.add_argument("--train-run", type=Path, default=None, help="dir with train_combined.* (+ summary.json)")
    p.add_argument("--train-nav", type=Path, default=None)
    p.add_argument("--val-role", type=Path, default=None, help="VALIDATION role manifest.json (E only)")
    p.add_argument("--val-run", type=Path, default=None, help="dir with validation_combined.* (E only)")
    p.add_argument("--val-nav", type=Path, default=None)
    p.add_argument("--scenario", default=SCENARIO)
    a = p.parse_args(argv)
    r, be = a.root, a.root / "build-equity"
    defaults = {"fitter": r / "atx-impl/tools/fit_composition_weights.py", "work_dir": be / "mega-fit-work",
                "weights_dir": be / "mega-weights-v3-plain", "library": r / "atx-impl/strategies/pv_fields_ic121_v3.json",
                "train_role": be / "recent-fast-train-2020-2022-v2/manifest.json",
                "train_run": be / "mega-v3w-plain-train-1", "train_nav": be / "mega-nav-v3-plain-b1",
                "val_role": be / "recent-fast-validation-2023-2024-v1/manifest.json",
                "val_run": be / "mega-v3-VAL-1", "val_nav": be / "mega-nav-v3-VAL"}
    for k, v in defaults.items():
        if getattr(a, k) is None:
            setattr(a, k, v)
    return a


def main(argv=None) -> int:
    a = parse_args(argv)
    started = time.perf_counter()
    deadline = started + a.budget_seconds
    sections = [s.strip().upper() for s in a.sections.split(",") if s.strip()]
    roles = [s.strip().lower() for s in a.e_roles.split(",") if s.strip()]
    try:
        need(sections and all(s in SECTIONS for s in sections), f"--sections must be a subset of {SECTIONS}")
        need(all(r in ("train", "val") for r in roles), "--e-roles must be a subset of train,val")
        need(a.reps >= 0, "--reps must be >= 0")
        F = import_fitter(a.fitter)
        NR = import_nav_recon()
        out_dir = Path(a.output)
        out_dir.mkdir(parents=True, exist_ok=True)
        doc = load_doc(out_dir)
        panel = {}

        def get_panel():
            if "p" not in panel:
                tick = time.perf_counter()
                panel["p"] = load_train_panel(F, a)
                print(f"[postmortem] TRAIN panel: {len(panel['p'].ids)} candidates x {panel['p'].factors.shape[1]} "
                      f"decisions in {time.perf_counter() - tick:.1f} s", flush=True)
            return panel["p"]

        def log(line):
            print(f"[postmortem] {line}", flush=True)

        for s in SECTIONS:
            if s not in sections:
                continue
            tick = time.perf_counter()
            if s == "A":
                result = section_a(F, get_panel())
            elif s == "B":
                result = section_b(F, get_panel())
            elif s == "C":
                result = section_c(F, get_panel())
            elif s == "D":
                result = merge_d(doc["sections"].get("D"), section_d(F, get_panel(), a.reps, a.seed, deadline))
            elif s == "E":
                result = dict(doc["sections"].get("E", {}))
                result.update(section_e(F, NR, a, roles, get_panel, log))
            else:
                result = section_f(get_panel())
            doc["sections"][s] = result
            seconds = time.perf_counter() - tick
            doc["timings"][s if s != "E" else "E:" + "+".join(roles)] = round(seconds, 2)
            save_doc(out_dir, doc)
            print(f"[postmortem] section {s}: {seconds:.1f} s, peak RSS {NR.peak_rss_mb():.0f} MiB", flush=True)
    except (PostmortemError, OSError, KeyError, ValueError) as exc:
        print(f"postmortem_v3: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:  # the fitter's FitError and anything unexpected: loud, non-zero
        traceback.print_exc()
        print(f"postmortem_v3: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1
    print(f"[postmortem] total {time.perf_counter() - started:.1f} s, peak RSS {NR.peak_rss_mb():.0f} MiB; "
          f"wrote {out_dir / 'postmortem.json'} and postmortem.md", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
