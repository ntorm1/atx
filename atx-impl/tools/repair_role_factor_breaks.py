#!/usr/bin/env python3
"""Detect and repair vendor factor re-anchoring breaks in a recent-research role (rule factor-break-v1).

``--scan-only`` is the QA scan (per-session counts, mass sessions, what the repair would do); it
writes nothing unless ``--csv`` names a new file. Without it the tool writes a NEW role directory
(exclusive) whose close.f64 has the break removed; every other payload is byte-identical and the
manifest keeps its schema plus a ``repair`` block.

Defect. A role's close.f64 is f64(raw f32 close) x f64 vendor cumulReturnFactor of the same row
(atx-engine/tools/prepare_recent_research.py). The TickerHistory3 2026-09-20 snapshot's factor is
not chained across 2021-01-04: bars before it omit corporate actions dated on or after it (atx-db
VA1 / ruling C-35: ~3,430 vendor lines step down, median 1.8%, exact ratios equal to later splits
applied from 2021-01-04 instead of from the line's start). Every adjusted return spanning the
session carries a factor step that the raw tape does not show.

Rule factor-break-v1 (declared before any use; parameters are the constants below and are
recorded in the output manifest). Logs throughout.

* Step: consecutive present observations p < t of one name at most MAX_GAP_DAYS calendar days
  apart. f = close/raw. s = ln(f_t/f_p) (factor step), r = ln(raw_t/raw_p) (raw move),
  a = ln(close_t/close_p) = r + s (adjusted move).
* Detector (which sessions): a jump cell is a step ending at t with |s| > CELL_STEP and
  |a| > |r| + CELL_EXCESS (the factor moved and the adjusted close moved more than the raw close).
  Session t is a MASS session when it has >= MASS_MIN_CELLS jump cells.
* Classification (which steps): on a mass session b, every step with p < b <= t and |s| > NOISE is
    kept_gap           a step across more than MAX_GAP_DAYS calendar days (never repaired, as VA1);
    kept_split_follow  split-like and the raw close followed on the same step: s < 0 and
                       r >= max(|s|/2, ln SPLIT_RATIO) (consolidation), or s > 0 and
                       -r >= max(s/2, ln SPLIT_RATIO) (forward split);
    kept_distribution  0 < s < ln SPLIT_RATIO: a same-day cash or stock distribution raises the
                       factor, while the re-anchoring lowers it for every later distribution or
                       forward split and raises it only for a later consolidation (>= SPLIT_RATIO);
    repaired           every other step: all factor decreases and every split-like increase whose
                       raw close did not follow.
* Repair: for a repaired step (p, t) of name j with k = f_t/f_p, close[0:t, j] *= k. The adjusted
  return across the step then equals the raw return (to rounding). Sessions >= t keep the vendor's
  current anchoring, so roles cut from the same source stay level-consistent after the break.

Thresholds (first principles, not outcomes): CELL_STEP 1% is above any ordinary quarterly cash
dividend (a 1% single payment is a >= 4%/yr yield) and far below any split (>= ln 1.2); a genuine
action moves the raw close against the factor, so it is a jump cell only when the name's own move
exceeds about half the step plus 0.5%. Legitimate jump cells are therefore bounded by the high-
yield ex-dates of one session (tens at a quarter-end peak), while a re-anchoring hits every name
with a later corporate action (hundreds to thousands): MASS_MIN_CELLS 50. NOISE 1e-9 is atx-db's
FACTOR_NOISE (a constant factor gives |s| ~ 1e-16 here). SPLIT_RATIO 1.25 (the smallest common
split, 5:4; VA1's ARTIFACT_SESSION_MIN_RISE) and the |s|/2 raw-follow rule are VA1 v2's artifact-
session veto, mirrored for increases. MAX_GAP_DAYS 10 is VA1's COVERAGE_MAX_GAP_DAYS. BIG_STEP 0.10
only reproduces nav_recon section E's big/small columns.

Python 3.12 + numpy. Reads only the role directory. Peak memory ~ all payloads + one close copy.
Exit codes: 0 done; 2 refused (nothing published); 3 no mass session and --allow-noop not given.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import time

import numpy as np

RULE = "factor-break-v1"
ROLE_SCHEMA = "atx.recent-research-role/v1"
DAY_NS = 86_400_000_000_000
CELL_STEP = 0.01
CELL_EXCESS = 0.01
MASS_MIN_CELLS = 50
BIG_STEP = 0.10
NOISE = 1e-9
SPLIT_RATIO = 1.25
MAX_GAP_DAYS = 10
MAX_RETURN_ERROR = 1e-12
MANIFEST_LIMIT = 1 << 20  # strategy_data.cpp kManifestLimit
CELLS_FILE = "repair_cells.csv"
TOOL_PATH = "atx-impl/tools/repair_role_factor_breaks.py"
DTYPES = {"sessions.i64": "<i8", "ids.u64": "<u8", "close.f64": "<f8", "raw_close.f64": "<f8",
          "volume.f64": "<f8", "present.u8": "u1", "member.u8": "u1"}
SCAN_FILES = ("sessions.i64", "ids.u64", "close.f64", "raw_close.f64", "present.u8")
ACTIONS = ("noise", "repaired", "kept_gap", "kept_split_follow", "kept_distribution")
REPAIRED, KEPT_GAP, KEPT_FOLLOW, KEPT_DIST = 1, 2, 3, 4
EXIT_REFUSED, EXIT_CLEAN = 2, 3
PARAMETERS = {"cell_step_ln": CELL_STEP, "cell_excess_ln": CELL_EXCESS, "mass_min_cells": MASS_MIN_CELLS,
              "noise_ln": NOISE, "split_ratio": SPLIT_RATIO, "max_gap_calendar_days": MAX_GAP_DAYS,
              "big_step_ln_scan_columns_only": BIG_STEP, "max_repaired_return_error_ln": MAX_RETURN_ERROR}
RULE_STATEMENT = (
    "step = consecutive present observations p<t of a name <= max_gap_calendar_days apart; s=ln(f_t/f_p), f=close/raw; "
    "r=ln(raw_t/raw_p); a=r+s. jump cell: step ending at t with |s|>cell_step_ln and |a|>|r|+cell_excess_ln. "
    "mass session: >= mass_min_cells jump cells. On a mass session b every step with p<b<=t and |s|>noise_ln is "
    "kept_gap (gap > max_gap_calendar_days), kept_split_follow (s<0 and r>=max(|s|/2, ln split_ratio), or s>0 and "
    "-r>=max(s/2, ln split_ratio)), kept_distribution (0<s<ln split_ratio), else repaired: close[0:t, j] *= f_t/f_p.")


class Refusal(Exception):
    pass


def sha_bytes(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def sha_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(1 << 22):
            h.update(chunk)
    return h.hexdigest()


def code_identity() -> dict:
    raw = Path(__file__).read_bytes()
    lf = raw.replace(b"\r\n", b"\n")
    return {"path": TOOL_PATH, "code_sha256": sha_bytes(raw), "code_sha256_lf": sha_bytes(lf),
            "code_git_blob_sha1": hashlib.sha1(b"blob %d\0" % len(lf) + lf).hexdigest()}


def iso(day: int) -> str:
    return (dt.date(1970, 1, 1) + dt.timedelta(days=int(day))).isoformat()


def peak_rss_mib() -> float:
    try:
        if os.name == "nt":
            import ctypes
            import ctypes.wintypes as wt

            class PMC(ctypes.Structure):
                _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD),
                            ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                            ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]
            pmc = PMC()
            pmc.cb = ctypes.sizeof(PMC)
            k32, psapi = ctypes.windll.kernel32, ctypes.windll.psapi
            k32.GetCurrentProcess.restype = wt.HANDLE
            psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.POINTER(PMC), wt.DWORD]
            psapi.GetProcessMemoryInfo(k32.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb)
            return pmc.PeakWorkingSetSize / 2**20
        import resource
        return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024
    except Exception:  # diagnostic only
        return float("nan")


# ----------------------------------------------------------------------------- input
def load_role(role: Path, expected_sha256: str, names) -> dict:
    """Manifest bound by SHA-256, then every named payload bound by the manifest's bytes + SHA-256."""
    path = role / "manifest.json"
    if path.stat().st_size > MANIFEST_LIMIT:
        raise Refusal("role manifest is oversized")
    manifest_bytes = path.read_bytes()
    manifest_sha = sha_bytes(manifest_bytes)
    if manifest_sha != expected_sha256.strip().lower():
        raise Refusal(f"role manifest SHA-256 {manifest_sha} does not match --role-sha256")
    m = json.loads(manifest_bytes)
    if m.get("schema") != ROLE_SCHEMA or m.get("status") != "complete":
        raise Refusal("role is not a complete atx.recent-research-role/v1 payload")
    if set(m.get("files", {})) != set(DTYPES):
        raise Refusal("role manifest files differ from the recent-research payload set")
    D, N = int(m["dates"]), int(m["instruments"])
    if not (0 < D <= 4096 and 0 < N <= 20000):
        raise Refusal("role shape outside the recent-research bounds")
    blobs, arrays = {}, {}
    for name in names:
        entry = m["files"][name]
        blob = (role / name).read_bytes()
        if len(blob) != entry["bytes"] or sha_bytes(blob) != entry["sha256"]:
            raise Refusal(f"role {name} bytes do not match the role manifest")
        shape = (D,) if name == "sessions.i64" else (N,) if name == "ids.u64" else (D, N)
        dtype = np.dtype(DTYPES[name])
        if len(blob) != math.prod(shape) * dtype.itemsize:
            raise Refusal(f"role {name} size disagrees with the manifest shape")
        blobs[name] = blob
        arrays[name] = np.frombuffer(blob, dtype=dtype).reshape(shape)
    sessions = arrays["sessions.i64"]
    if np.any(sessions % DAY_NS != 0) or np.any(np.diff(sessions) <= 0):
        raise Refusal("role sessions are not strictly increasing midnight labels")
    present = arrays["present.u8"]
    if np.any(present > 1):
        raise Refusal("role present mask is not binary")
    on = present == 1
    for name in ("close.f64", "raw_close.f64"):
        x = arrays[name]
        if not np.all(np.isfinite(x[on]) & (x[on] > 0)) or not np.all(np.isnan(x[~on])):
            raise Refusal(f"role {name} breaks the present => finite positive / absent => NaN contract")
    return {"dir": role, "manifest": m, "manifest_bytes": manifest_bytes, "manifest_sha256": manifest_sha,
            "D": D, "N": N, "days": sessions // DAY_NS, "ids": arrays["ids.u64"], "close": arrays["close.f64"],
            "raw": arrays["raw_close.f64"], "present": on, "blobs": blobs}


# ----------------------------------------------------------------------------- rule
def session_stats(close, raw, present, days) -> dict:
    """Per-session counts over steps ending at t (see module docstring). big/small are nav_recon
    section E's columns (adjacent sessions only); f1_prev/f1 count factors exactly 1.0."""
    D, N = close.shape
    keys = ("present", "steps", "jump", "big", "small", "dec", "inc", "long_gap", "f1_prev", "f1")
    out = {k: np.zeros(D, np.int64) for k in keys}
    last = np.full(N, -1, np.int64)
    lc, lr = np.full(N, np.nan), np.full(N, np.nan)
    with np.errstate(divide="ignore", invalid="ignore"):
        for t in range(D):
            pt = present[t]
            ct, rt = close[t], raw[t]
            hp = pt & (last >= 0)
            gap = days[t] - days[np.maximum(last, 0)]
            step = hp & (gap <= MAX_GAP_DAYS)
            adjacent = hp & (last == t - 1)
            a = np.log(ct) - np.log(lc)
            r = np.log(rt) - np.log(lr)
            s = a - r
            abs_s, abs_a, abs_r = np.abs(s), np.abs(a), np.abs(r)
            out["present"][t] = np.count_nonzero(pt)
            out["steps"][t] = np.count_nonzero(step)
            out["jump"][t] = np.count_nonzero(step & (abs_s > CELL_STEP) & (abs_a > abs_r + CELL_EXCESS))
            out["big"][t] = np.count_nonzero(adjacent & (abs_s > BIG_STEP) & (abs_a > abs_r + BIG_STEP))
            out["small"][t] = np.count_nonzero(adjacent & (abs_s > CELL_STEP) & (abs_s <= BIG_STEP)
                                               & (abs_a > abs_r + CELL_EXCESS))
            out["dec"][t] = np.count_nonzero(step & (s < -NOISE))
            out["inc"][t] = np.count_nonzero(step & (s > NOISE))
            out["long_gap"][t] = np.count_nonzero(hp & ~step & (abs_s > NOISE))
            out["f1_prev"][t] = np.count_nonzero(step & (lc == lr))
            out["f1"][t] = np.count_nonzero(pt & (ct == rt))
            last[pt] = t
            lc[pt], lr[pt] = ct[pt], rt[pt]
    return out


def crossing_steps(close, raw, present, days, b: int) -> dict:
    """Every name's step (p, t) with p < b <= t (last observation before b, first at or after it)."""
    before, after = present[:b], present[b:]
    j = np.flatnonzero(before.any(axis=0) & after.any(axis=0))
    p = (b - 1) - np.argmax(before[::-1], axis=0)[j]
    t = b + np.argmax(after, axis=0)[j]
    cp, rp, ct, rt = close[p, j], raw[p, j], close[t, j], raw[t, j]
    a = np.log(ct) - np.log(cp)
    r = np.log(rt) - np.log(rp)
    s = a - r
    k = (ct / rt) / (cp / rp)
    gap = days[t] - days[p]
    split = math.log(SPLIT_RATIO)
    half = np.abs(s) / 2
    follow = ((s < 0) & (r >= np.maximum(half, split))) | ((s > 0) & (-r >= np.maximum(half, split)))
    action = np.where(np.abs(s) <= NOISE, 0,
             np.where(gap > MAX_GAP_DAYS, KEPT_GAP,
             np.where(follow, KEPT_FOLLOW,
             np.where((s > 0) & (s < split), KEPT_DIST, REPAIRED))))
    keep = action != 0
    return {"session": b, "j": j[keep], "p": p[keep], "t": t[keep], "s": s[keep], "r": r[keep], "a": a[keep],
            "k": k[keep], "gap": gap[keep], "action": action[keep]}


def analyse(role: dict) -> dict:
    close, raw, present, days = role["close"], role["raw"], role["present"], role["days"]
    stats = session_stats(close, raw, present, days)
    mass = [int(b) for b in np.flatnonzero(stats["jump"] >= MASS_MIN_CELLS)]
    steps, seen = [], np.empty(0, np.int64)
    for b in mass:
        # A step that crosses several mass sessions is one step: listed (and repaired) once, under the
        # first. Its classification depends only on (p, t), so the choice of session cannot change it.
        x = crossing_steps(close, raw, present, days, b)
        key = x["j"].astype(np.int64) * role["D"] + x["t"]
        fresh = ~np.isin(key, seen)
        steps.append({k: (v[fresh] if isinstance(v, np.ndarray) else v) for k, v in x.items()})
        seen = np.concatenate((seen, key[fresh]))
    return {"stats": stats, "mass": mass, "steps": steps}


def apply_repair(close, steps) -> np.ndarray:
    """close[0:t, j] *= k for every repaired step, sessions ascending, then t and columns ascending."""
    new = np.array(close, dtype="<f8", copy=True)
    for x in steps:
        rep = x["action"] == REPAIRED
        j, t, k = x["j"][rep], x["t"][rep], x["k"][rep]
        for tv in np.unique(t):
            sel = t == tv
            new[:tv, j[sel]] = new[:tv, j[sel]] * k[sel]
    return new


def verify_repair(role: dict, new: np.ndarray, steps) -> dict:
    """Refuses unless the repair is exact, local, contract-preserving and leaves no mass session."""
    close, raw, present = role["close"], role["raw"], role["present"]
    if not np.all(np.isfinite(new[present]) & (new[present] > 0)) or not np.all(np.isnan(new[~present])):
        raise Refusal("repaired close breaks the present => finite positive / absent => NaN contract")
    allowed = np.zeros(new.shape, dtype=bool)
    worst = 0.0
    for x in steps:
        rep = x["action"] == REPAIRED
        j, p, t = x["j"][rep], x["p"][rep], x["t"][rep]
        for jj, tt in zip(j.tolist(), t.tolist()):
            allowed[:tt, jj] = True
        if len(j):
            err = np.abs((np.log(new[t, j]) - np.log(new[p, j])) - (np.log(raw[t, j]) - np.log(raw[p, j])))
            worst = max(worst, float(err.max()))
    if worst > MAX_RETURN_ERROR:
        raise Refusal(f"repaired step return differs from the raw return by {worst:.3g} (ln)")
    same = (new == close) | (np.isnan(new) & np.isnan(close))
    if np.any(~same & ~allowed):
        raise Refusal("repair changed a cell outside the repaired names' pre-break history")
    post = session_stats(new, raw, present, role["days"])
    if np.any(post["jump"] >= MASS_MIN_CELLS):
        raise Refusal("a mass session remains after repair")
    return {"post": post, "worst": worst, "changed_cells": int(np.count_nonzero(~same))}


# ----------------------------------------------------------------------------- report
def print_scan(role: dict, res: dict, top: int) -> None:
    st, days, D = res["stats"], role["days"], role["D"]
    mass = set(res["mass"])
    print(f"{RULE} scan  role {role['dir'].resolve()}")
    print(f"manifest_sha256 {role['manifest_sha256']}  dates {D}  instruments {role['N']}  "
          f"sessions {iso(days[0])}..{iso(days[-1])}")
    print(f"detector: jump = step with |dlog f|>{CELL_STEP} & |dlog adj|>|dlog raw|+{CELL_EXCESS} "
          f"(consecutive present obs <= {MAX_GAP_DAYS} days apart); MASS = jump >= {MASS_MIN_CELLS}")
    print("columns: big/small = nav_recon section E (adjacent sessions); dec/inc = factor steps beyond "
          f"{NOISE:g}; longgap = factor steps across > {MAX_GAP_DAYS} days; f1prev/f1 = share of steps whose "
          "prior-obs factor is exactly 1 / share of present names with factor exactly 1")
    years = (days.astype("datetime64[D]").astype("datetime64[Y]").astype(np.int64))
    year_start = {0} | {int(t) for t in np.flatnonzero(np.diff(years) != 0) + 1}
    top_rows = {int(t) for t in np.argsort(-st["jump"], kind="stable")[:top] if st["jump"][t] > 0}
    rows = sorted(mass | top_rows | year_start | {int(t) for t in np.flatnonzero(st["big"] > 0)})
    print(f"{'session':10} {'present':>7} {'steps':>6} {'jump':>5} {'big':>4} {'small':>5} {'dec':>5} "
          f"{'inc':>5} {'longgap':>7} {'f1prev':>6} {'f1':>5}  flag")
    for t in rows:
        f1p = st["f1_prev"][t] / st["steps"][t] if st["steps"][t] else float("nan")
        f1 = st["f1"][t] / st["present"][t] if st["present"][t] else float("nan")
        flag = "MASS" if t in mass else ("year-start" if t in year_start else "")
        print(f"{iso(days[t]):10} {st['present'][t]:7d} {st['steps'][t]:6d} {st['jump'][t]:5d} {st['big'][t]:4d} "
              f"{st['small'][t]:5d} {st['dec'][t]:5d} {st['inc'][t]:5d} {st['long_gap'][t]:7d} {f1p:6.3f} "
              f"{f1:5.3f}  {flag}")
    other = np.array([st["jump"][t] if t not in mass else 0 for t in range(D)])
    worst = int(np.argmax(other))
    active = st["steps"] > 0
    print(f"summary: sessions with >=1 jump cell {int(np.count_nonzero(st['jump']))} of {D}; median jump/session "
          f"{np.median(st['jump'][active]) if active.any() else 0:.0f}; max outside mass sessions {int(other[worst])} "
          f"on {iso(days[worst])} (threshold {MASS_MIN_CELLS})")
    big = st["big"]
    print(f"summary: sessions with >=1 big {int(np.count_nonzero(big))}; big cells total {int(big.sum())}, "
          f"excluding the top session {int(big.sum() - big.max())}; longgap steps total {int(st['long_gap'].sum())}")
    for x in res["steps"]:
        print_classification(role, x, top)
    if res["mass"]:
        print(f"verdict: MASS {len(res['mass'])} session(s): {', '.join(iso(days[b]) for b in res['mass'])}")
    else:
        print("verdict: CLEAN (no mass session)")


def print_classification(role: dict, x: dict, top: int) -> None:
    days, ids, a = role["days"], role["ids"], x["action"]
    rep = a == REPAIRED
    counts = {ACTIONS[c]: int(np.count_nonzero(a == c)) for c in (REPAIRED, KEPT_GAP, KEPT_FOLLOW, KEPT_DIST)}
    spanning = int(np.count_nonzero(x["t"] != x["session"]))
    print(f"mass {iso(days[x['session']])}: crossing steps with |s|>{NOISE:g}: {len(a)} "
          f"(gap-spanning {spanning}); " + ", ".join(f"{k} {v}" for k, v in counts.items()))
    if rep.any():
        s = np.abs(x["s"][rep])
        q = np.quantile(s, [0.1, 0.5, 0.9])
        print(f"  repaired: decrease {int(np.count_nonzero(x['s'][rep] < 0))}, increase "
              f"{int(np.count_nonzero(x['s'][rep] > 0))}; |ln k| p10 {q[0]:.4f} p50 {q[1]:.4f} p90 {q[2]:.4f} "
              f"max {s.max():.4f}; |raw move| p50 {np.median(np.abs(x['r'][rep])):.4f}; "
              f"k>=1.25 or k<=0.8: {int(np.count_nonzero(s >= math.log(SPLIT_RATIO)))}")
        order = np.argsort(-s, kind="stable")[:top]
        idx = np.flatnonzero(rep)[order]
        print("  largest repaired (id k raw_ratio): " + "; ".join(
            f"{int(ids[x['j'][i]])} {x['k'][i]:.5g} {math.exp(x['r'][i]):.4f}" for i in idx))
    kept = np.flatnonzero(~rep)
    if len(kept):
        print("  kept (id action k raw_ratio gap_days): " + "; ".join(
            f"{int(ids[x['j'][i]])} {ACTIONS[a[i]]} {x['k'][i]:.5g} {math.exp(x['r'][i]):.4f} {int(x['gap'][i])}"
            for i in kept[:20]) + (f"; ... {len(kept) - 20} more" if len(kept) > 20 else ""))


def write_exclusive(path: Path, data: bytes) -> None:
    with path.open("xb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


def scan_csv(path: Path, role: dict, res: dict) -> None:
    st, mass = res["stats"], set(res["mass"])
    keys = ("present", "steps", "jump", "big", "small", "dec", "inc", "long_gap", "f1_prev", "f1")
    lines = ["session," + ",".join(keys) + ",mass"]
    for t in range(role["D"]):
        lines.append(iso(role["days"][t]) + "," + ",".join(str(int(st[k][t])) for k in keys)
                     + ("," + ("1" if t in mass else "0")))
    write_exclusive(path, ("\n".join(lines) + "\n").encode("ascii"))


def cells_csv(role: dict, steps) -> bytes:
    days, ids, sessions = role["days"], role["ids"], role["days"] * DAY_NS
    lines = ["mass_session,session,session_ns,prev_session,gap_days,column,security_id,"
             "ln_factor_step,factor_step,ln_raw_move,ln_adj_move,action"]
    for x in steps:
        order = np.lexsort((x["j"], x["t"]))
        for i in order:
            lines.append(",".join((
                iso(days[x["session"]]), iso(days[x["t"][i]]), str(int(sessions[x["t"][i]])), iso(days[x["p"][i]]),
                str(int(x["gap"][i])), str(int(x["j"][i])), str(int(ids[x["j"][i]])),
                format(float(x["s"][i]), ".17g"), format(float(x["k"][i]), ".17g"),
                format(float(x["r"][i]), ".17g"), format(float(x["a"][i]), ".17g"), ACTIONS[x["action"][i]])))
    return ("\n".join(lines) + "\n").encode("ascii")


# ----------------------------------------------------------------------------- repair
def publish_manifest(path: Path, value: dict) -> bytes:
    content = (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")
    if len(content) > MANIFEST_LIMIT:
        raise Refusal("repaired manifest would exceed the loader's 1 MiB manifest limit")
    pending = path.with_name("." + path.name + ".pending")
    write_exclusive(pending, content)
    os.link(pending, path)  # atomic exclusive publication: no manifest or its complete bytes
    pending.unlink()
    return content


def repair(role: dict, res: dict, out: Path) -> tuple[str, dict]:
    steps, days = res["steps"], role["days"]
    new = apply_repair(role["close"], steps)
    check = verify_repair(role, new, steps)
    new_close = new.astype("<f8", copy=False).tobytes()
    if not res["mass"] and new_close != role["blobs"]["close.f64"]:
        raise Refusal("no-op repair changed close bytes")
    cells = cells_csv(role, steps)
    out.mkdir(parents=False, exist_ok=False)
    files = {}
    for name in sorted(DTYPES):
        data = new_close if name == "close.f64" else role["blobs"][name]
        write_exclusive(out / name, data)
    write_exclusive(out / CELLS_FILE, cells)
    source = role["manifest"]["files"]
    for name in sorted(DTYPES):  # re-read from disk: what was published, not what was intended
        digest = sha_file(out / name)
        files[name] = {"bytes": (out / name).stat().st_size, "sha256": digest}
        if name != "close.f64" and files[name] != source[name]:
            raise Refusal(f"published {name} is not byte-identical to the source role")
    if sha_file(out / CELLS_FILE) != sha_bytes(cells):
        raise Refusal("published repair cell list changed on disk")
    sessions = []
    for x in steps:
        b, a = x["session"], x["action"]
        rep = a == REPAIRED
        sessions.append({
            "session": iso(days[b]), "session_ns": int(days[b]) * DAY_NS,
            "jump_cells": int(res["stats"]["jump"][b]), "big_adjacent": int(res["stats"]["big"][b]),
            "small_adjacent": int(res["stats"]["small"][b]), "factor_decreases": int(res["stats"]["dec"][b]),
            "factor_increases": int(res["stats"]["inc"][b]), "crossing_steps": int(len(a)),
            "gap_spanning_steps": int(np.count_nonzero(x["t"] != b)),
            "repaired": int(np.count_nonzero(rep)),
            "repaired_decrease": int(np.count_nonzero(rep & (x["s"] < 0))),
            "repaired_increase": int(np.count_nonzero(rep & (x["s"] > 0))),
            "kept_gap": int(np.count_nonzero(a == KEPT_GAP)),
            "kept_split_follow": int(np.count_nonzero(a == KEPT_FOLLOW)),
            "kept_distribution": int(np.count_nonzero(a == KEPT_DIST)),
            "post_repair_jump_cells": int(check["post"]["jump"][b])})
    repaired_names = np.unique(np.concatenate([x["j"][x["action"] == REPAIRED] for x in steps])) if steps else []
    manifest = json.loads(role["manifest_bytes"])
    manifest["files"] = files
    manifest["repair"] = {
        "rule": RULE, "rule_statement": RULE_STATEMENT, "parameters": PARAMETERS, "noop": not res["mass"],
        "source_role": {"path": str(role["dir"].resolve()), "manifest_sha256": role["manifest_sha256"],
                        "close_sha256": source["close.f64"]["sha256"]},
        "tool": code_identity(),
        "defect": ("vendor cumulReturnFactor re-anchoring: bars before the mass session omit corporate actions dated "
                   "on or after it (TickerHistory3 2026-09-20 snapshot, 2021-01-04; atx-db VA1 / C-35)"),
        "close_basis_note": ("close_basis is kept verbatim for loader compatibility (strategy_data.cpp pins it); "
                             "close.f64 is that vendor basis with each repaired step neutralized: close[0:t, j] *= "
                             "f_t/f_p (sessions >= t unchanged)"),
        "mass_sessions": sessions,
        "detector_jump_cells_by_session": [int(v) for v in res["stats"]["jump"]],
        "repaired_names": int(len(repaired_names)), "changed_close_cells": check["changed_cells"],
        "max_repaired_return_error_ln": check["worst"],
        "cells": {"file": CELLS_FILE, "bytes": len(cells), "sha256": sha_bytes(cells),
                  "rows": cells.count(b"\n") - 1, "listed": "every crossing step with |s| > noise_ln (repaired and kept)"},
        "unchanged_files": [n for n in sorted(DTYPES) if n != "close.f64"]}
    content = publish_manifest(out / "manifest.json", manifest)
    return sha_bytes(content), check


# ----------------------------------------------------------------------------- main
def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--role", required=True, type=Path, help="source role directory")
    p.add_argument("--role-sha256", required=True, help="SHA-256 of the source role manifest.json")
    p.add_argument("--scan-only", action="store_true", help="QA scan only; writes nothing (except --csv)")
    p.add_argument("--csv", type=Path, help="scan-only: write per-session counts to this NEW file")
    p.add_argument("--out", type=Path, help="repair: NEW role directory (must not exist; parent must)")
    p.add_argument("--expect-sessions", help="repair: comma-separated ISO mass sessions, or 'none'; refuse if "
                   "the detected set differs")
    p.add_argument("--allow-noop", action="store_true", help="repair: publish an unchanged copy when no mass session")
    p.add_argument("--top", type=int, default=10)
    a = p.parse_args(argv)
    if a.scan_only == (a.out is not None):
        p.error("give exactly one of --scan-only or --out")
    if a.csv is not None and not a.scan_only:
        p.error("--csv is a scan-only option")
    started = time.perf_counter()
    try:
        if a.out is not None and (a.out.exists() or not a.out.resolve().parent.is_dir()):
            raise Refusal(f"--out {a.out} must not exist and its parent must be a directory (exclusive output)")
        if a.csv is not None and a.csv.exists():
            raise Refusal(f"--csv {a.csv} already exists (exclusive output)")
        role = load_role(a.role, a.role_sha256, SCAN_FILES if a.scan_only else tuple(DTYPES))
        res = analyse(role)
        print_scan(role, res, a.top)
        if a.scan_only:
            if a.csv is not None:
                scan_csv(a.csv, role, res)
                print(f"per-session counts: {a.csv}")
        else:
            found = [iso(role["days"][b]) for b in res["mass"]]
            if a.expect_sessions is not None:
                want = [] if a.expect_sessions.strip().lower() == "none" else sorted(
                    dt.date.fromisoformat(x.strip()).isoformat() for x in a.expect_sessions.split(","))
                if want != found:
                    raise Refusal(f"detected mass sessions {found or 'none'} differ from --expect-sessions {want or 'none'}")
            if not res["mass"] and not a.allow_noop:
                print("no mass session: nothing to repair; no output written (--allow-noop publishes a copy)")
                return EXIT_CLEAN
            sha, check = repair(role, res, a.out)
            print(f"repaired: changed close cells {check['changed_cells']}; max |ln return error| "
                  f"{check['worst']:.3g}; post-repair max jump/session {int(check['post']['jump'].max())}")
            print(f"output {a.out.resolve()}")
            print(f"output manifest sha256 {sha}")
    except (Refusal, OSError, KeyError, ValueError) as e:
        # Nothing is published without its manifest (publish-last); a partial --out is never a role.
        print(f"refused: {type(e).__name__}: {e}", file=sys.stderr)
        return EXIT_REFUSED
    print(f"elapsed {time.perf_counter() - started:.2f}s  peak rss {peak_rss_mib():.0f} MiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
