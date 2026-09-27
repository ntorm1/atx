#!/usr/bin/env python3
"""t17_checks.py -- T17 PIT/look-ahead audit follow-up checks C1-C8 (read-only; the root runs it).

Each check settles one RISK/UNKNOWN of task-T17-report.md against the criterion written there. Results merge
into DIR/t17_checks.json (a rerun of a check replaces only that check); DIR/t17_checks.md is re-rendered from it
with one verdict paragraph per check (what was found + CONFIRMED / CLEARED / INCONCLUSIVE, or ERROR).

  C1  FINRA: are later revisions baked into the republished (settlement < 2021-06-01) values?   report #8
  C2  TRAIN only: SI-change edge on republished-vintage vs vintage-safe TRAIN decisions         report #8
  C3  FINRA as-of alignment: value at available_at D comes from the settlement disseminated on D report #9, #7
  C4  vendor dead-line coverage (survivorship): annual vanish rate of year-start members         report #27
  C5  book level: write-off (zero delisting return) materiality; guarded-interval asymmetry      report #26, A3
  C6  price-risk-v1 market proxy: ALL-union equal-weight market vs member-only market           report #6
  C7  FINRA dissemination offsets (business days) by schedule label era                         report #7, A2
  C8  TickerHistory3: IV crush day vs earnFlag 0, earnings-adjustment incidence by period        report #13-15

Hygiene: nothing selects anything. C2 reads TRAIN (2020-2022) fitter records only. C5 reads NAV outputs at BOOK
level only (daily series and event aggregates; no candidate, no per-candidate validation number). C3/C7 never read
a raw FINRA file for a settlement on/after 2025-01-01; C8 decodes TickerHistory3 batches and discards rows dated
on/after 2025-01-01 immediately (as the fields producer does). Every role/weights input is pinned by SHA-256.

Suggested grouping (each invocation fits 180 s / 1536 MiB; soft stop via --budget-seconds):
  1. --checks C1,C3,C7    FINRA raw files, as-of CSV, schedule            (~72 + <=120 raw file reads)
  2. --checks C2,C4,C5,C6 TRAIN fitter records, role payloads, NAV CSVs    (C6 streams rows; peak ~0.3 GiB)
  3. --checks C8          one TickerHistory3 5-column scan
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import os
import sys
import time
import traceback
from pathlib import Path

import numpy as np

SCHEMA = "atx.t17-checks/v1"
CHECKS = ("C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8")
TITLES = {
    "C1": "FINRA revisions baked into republished pre-2021-06 short interest (report #8)",
    "C2": "TRAIN-only SI-change edge, republished vs vintage-safe decisions (report #8)",
    "C3": "FINRA as-of alignment of available_at to the disseminated settlement (report #9, #7)",
    "C4": "Vendor dead-line coverage / survivorship (report #27)",
    "C5": "Book-level write-off materiality and guarded-interval asymmetry (report #26, A3)",
    "C6": "price-risk-v1 ALL-union market vs member-only market (report #6)",
    "C7": "FINRA dissemination offsets by schedule label era (report #7, A2)",
    "C8": "TickerHistory3 IV crush timing and earnings-adjustment incidence (report #13-15; #14 stays UNKNOWN)",
}
DEFAULT_ROOT = Path("C:/atx-wt/pool-2")
DEFAULT_FINRA = Path("C:/atx/data/finra_short_interest")
DEFAULT_TH = Path("C:/Users/natha/Downloads/TickerHistory3.parquet")
FROZEN_TRAIN_ROLE_SHA = "210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de"
FROZEN_VAL_ROLE_SHA = "0c757c41a363659664c96359a2d2288e10f792e2b91ab38ca8bf5e064dfbbda7"
FROZEN_WEIGHTS_SHA = "db0a8b9a1b70d6ba81f7af6d1e1a7f8774f3a98ac6e1ecf2566a1610ce764aac"
SCENARIO = "modeled-1bn-stale5-v1+swap-fin-v1"
SEAL = "2025-01-01"
TRAIN_END = "2023-01-01"
DAY_NS = 86_400_000_000_000
ANN = 252
EXCHANGE_CLASSES = ("AMEX", "ARCA", "BZX", "NNM", "NYSE", "SC")
RAW_COLUMNS = ["symbolCode", "marketClassCode", "currentShortPositionQuantity", "previousShortPositionQuantity",
               "revisionFlag", "stockSplitFlag"]
C1_ERAS = (("republished_TRAIN", "20191201", "20210601"), ("original_PIT", "20220101", "20230701"))
C1_MIN_FILES, C1_MIN_R = 10, 20
C2_SI = ("si_change_10_s21", "si_change_42_s21")
C2_CONTROLS = ("vol_of_vol_21_126_s63", "year_forward_week_s63", "iv_level_21_s21")
C2_MIN_DAYS = 60
C3_SPLIT = "2021-06-10"          # first available_at of original-vintage rows (fields manifests vintage_safe_from)
C3_DATES_PER_ERA, C3_ROWS_PER_DATE, C3_MIN_VALUE, C3_MIN_SAMPLES = 20, 5, 100_000, 100
C8_SAMPLE_IDS = 400


class CheckError(Exception):
    pass


def need(cond, msg: str):
    if not cond:
        raise CheckError(msg)


# ------------------------------------------------------------------------------ resources
def _pmc():
    import ctypes
    import ctypes.wintypes as wt

    class PMC(ctypes.Structure):
        _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                    ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                    ("PeakPagefileUsage", ctypes.c_size_t)]
    pmc = PMC()
    pmc.cb = ctypes.sizeof(PMC)
    k32, psapi = ctypes.windll.kernel32, ctypes.windll.psapi
    k32.GetCurrentProcess.restype = wt.HANDLE
    psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.POINTER(PMC), wt.DWORD]
    psapi.GetProcessMemoryInfo(k32.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb)
    return pmc


def rss_mib() -> tuple[float, float]:
    """(current, peak) resident MiB; NaN when unavailable (diagnostic only)."""
    try:
        if os.name == "nt":
            p = _pmc()
            return p.WorkingSetSize / 2**20, p.PeakWorkingSetSize / 2**20
        import resource
        peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024
        return peak, peak
    except Exception:
        return float("nan"), float("nan")


class Budget:
    """Soft wall-clock deadline for the invocation and a hard RSS guard, polled at loop boundaries."""

    def __init__(self, seconds: float, max_rss_mib: float):
        self.deadline = time.monotonic() + seconds
        self.max_rss = max_rss_mib

    def over(self) -> bool:
        return time.monotonic() > self.deadline

    def rss(self, stage: str):
        cur, _ = rss_mib()
        if cur == cur and cur > self.max_rss:
            raise CheckError(f"RSS {cur:.0f} MiB exceeds --max-rss-mib at {stage}")


# ------------------------------------------------------------------------------ helpers
def sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
    return h.hexdigest()


def clean(x):
    """JSON-safe copy: numpy scalars -> Python, non-finite floats -> None."""
    if isinstance(x, dict):
        return {str(k): clean(v) for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [clean(v) for v in x]
    if isinstance(x, np.generic):
        x = x.item()
    if isinstance(x, float) and not math.isfinite(x):
        return None
    return x


def f3(x, fmt="{:.3f}"):
    return "n/a" if x is None or (isinstance(x, float) and not math.isfinite(x)) else fmt.format(x)


def sharpe(x: np.ndarray, min_days: int = C2_MIN_DAYS):
    x = x[np.isfinite(x)]
    if len(x) < min_days:
        return None, int(len(x))
    sd = x.std(ddof=1)
    return (float(x.mean() / sd * math.sqrt(ANN)) if sd > 0 else None), int(len(x))


class Role:
    """A pinned atx.recent-research-role/v1 directory (manifest SHA must equal the pin unless pin is None)."""

    def __init__(self, directory: Path, pin: str | None):
        self.dir = Path(directory)
        raw = (self.dir / "manifest.json").read_bytes()
        self.sha = sha256_bytes(raw)
        need(pin is None or self.sha == pin, f"{self.dir}: manifest SHA {self.sha[:12]} != pin {str(pin)[:12]}")
        self.m = json.loads(raw)
        need(self.m.get("schema") == "atx.recent-research-role/v1" and self.m.get("status") == "complete",
             f"{self.dir}: not a complete recent-research role")
        self.D, self.N = int(self.m["dates"]), int(self.m["instruments"])
        self.begin, self.end = int(self.m["score_begin"]), int(self.m["score_end"])

    def array(self, name: str, dtype: str, verify: bool = True) -> np.ndarray:
        entry = self.m["files"][name]
        path = self.dir / name
        need(path.stat().st_size == entry["bytes"], f"{path}: size differs from the role manifest")
        if verify:
            need(sha256_file(path) == entry["sha256"], f"{path}: SHA-256 differs from the role manifest")
        return np.fromfile(path, dtype=dtype)

    def sessions(self) -> np.ndarray:
        s = self.array("sessions.i64", "<i8")
        need(len(s) == self.D and np.all(s % DAY_NS == 0), f"{self.dir}: sessions axis")
        return (s // DAY_NS).astype("datetime64[D]")

    def matrix(self, name: str, dtype: str) -> np.ndarray:
        return self.array(name, dtype).reshape(self.D, self.N)


def years_of(days: np.ndarray) -> np.ndarray:
    return days.astype("datetime64[Y]").astype(np.int64) + 1970


# ------------------------------------------------------------------------------ FINRA raw access (C1, C3)
_VALUE_SETS: dict[str, set] = {}


def raw_path(a, settlement: str) -> Path:
    """settlement as YYYYMMDD or YYYY-MM-DD; refuses anything on/after the seal."""
    s = settlement.replace("-", "")
    need(s < SEAL.replace("-", ""), f"refusing FINRA raw file for settlement {s} (sealed)")
    return a.finra / "raw" / f"si_{s}.csv"


def load_raw(a, path: Path):
    """Exchange-listed rows of one consolidated file, symbol-indexed (last duplicate wins, as the producer)."""
    import pandas as pd
    x = pd.read_csv(path, sep="|", quoting=3, dtype=str, keep_default_na=False, on_bad_lines="skip",
                    usecols=lambda c: c in RAW_COLUMNS)
    need(all(c in x.columns for c in RAW_COLUMNS[:3]), f"{path.name}: symbol/class/current columns missing")
    for c in RAW_COLUMNS[3:]:
        if c not in x.columns:  # an absent previous/flag column reads as empty (C1 then sees no revision trail)
            x[c] = ""
    x = x[x.marketClassCode.isin(EXCHANGE_CLASSES)].drop_duplicates("symbolCode", keep="last").set_index("symbolCode")
    for c in ("currentShortPositionQuantity", "previousShortPositionQuantity"):
        x[c] = pd.to_numeric(x[c], errors="coerce")
    x["revisionFlag"] = x.revisionFlag.str.strip()
    x["stockSplitFlag"] = x.stockSplitFlag.str.strip()
    _VALUE_SETS[path.name[3:11]] = set(x.currentShortPositionQuantity.dropna().tolist())
    return x


def value_set(a, settlement: str) -> set | None:
    s = settlement.replace("-", "")
    if s not in _VALUE_SETS:
        path = raw_path(a, s)
        if not path.is_file():
            return None
        load_raw(a, path)
    return _VALUE_SETS[s]


def read_schedule(a):
    import pandas as pd
    s = pd.read_csv(a.finra / "dissemination_schedule.csv", dtype=str, keep_default_na=False)
    for c in ("settlement_date", "dissemination_date"):
        need(c in s.columns, f"schedule lacks {c}")
    return s.sort_values("settlement_date").reset_index(drop=True)


# ------------------------------------------------------------------------------ C1
def check_c1(a, budget: Budget) -> dict:
    files = sorted((a.finra / "raw").glob("si_*.csv"))
    eras, partial = {}, False
    for name, lo, hi in C1_ERAS:
        fs = [f for f in files if lo <= f.name[3:11] < hi]
        need(len(fs) >= C1_MIN_FILES, f"C1 {name}: only {len(fs)} raw files in [{lo}, {hi})")
        s = dict(first=fs[0].name[3:11], last=fs[-1].name[3:11], files=len(fs), pairs=0, rows=0,
                 split_rows_excluded=0, R=0, R_differs=0, nonR=0, nonR_differs=0)
        prev = None
        for f in fs:
            if budget.over():
                partial = True
                break
            cur = load_raw(a, f)
            if prev is not None:
                j = cur.join(prev["currentShortPositionQuantity"].rename("prior_current"), how="inner")
                j = j.dropna(subset=["previousShortPositionQuantity", "prior_current"])
                split = j.stockSplitFlag.eq("S")
                s["split_rows_excluded"] += int(split.sum())
                j = j[~split]
                differs = j.previousShortPositionQuantity != j.prior_current
                r = j.revisionFlag.eq("R")
                s["pairs"] += 1
                s["rows"] += int(len(j))
                s["R"] += int(r.sum())
                s["R_differs"] += int((r & differs).sum())
                s["nonR"] += int((~r).sum())
                s["nonR_differs"] += int((~r & differs).sum())
            prev = cur
            budget.rss("C1")
        s["R_rate"] = s["R_differs"] / s["R"] if s["R"] else None
        s["nonR_rate"] = s["nonR_differs"] / s["nonR"] if s["nonR"] else None
        s["R_per_1000_rows"] = 1000 * s["R"] / s["rows"] if s["rows"] else None
        eras[name] = s
    pre, post = eras["republished_TRAIN"], eras["original_PIT"]
    criterion = ("CLEARED if R_differs/R is high (>=0.5) in both eras with nonR mismatch ~0 (revision only in the next "
                 "cycle's `previous`); CONFIRMED if the republished era has R_differs/R < 0.2, or R frequency < 0.2x "
                 "the original era, while the original era is >= 0.5 (TRAIN-only ~1-cycle look-ahead), or both eras "
                 "< 0.2 (symmetric revision look-ahead); INCONCLUSIVE otherwise, if nonR mismatch > 5% (pairing "
                 "semantics off) or the original era has < 20 R rows")
    status, qualifier = "INCONCLUSIVE", ""
    nonr_ok = all(e["nonR_rate"] is not None and e["nonR_rate"] <= 0.05 for e in (pre, post))
    if partial:
        qualifier = "stopped at the time budget"
    elif not nonr_ok:
        qualifier = "non-revised rows mismatch > 5%: pairing semantics off"
    elif post["R"] < C1_MIN_R:
        qualifier = "too few revision-flagged rows in the original era for a reference"
    elif post["R_rate"] >= 0.5:
        few_pre = pre["R"] < C1_MIN_R or (pre["R_per_1000_rows"] or 0) < 0.2 * post["R_per_1000_rows"]
        if few_pre or (pre["R_rate"] is not None and pre["R_rate"] < 0.2):
            status, qualifier = "CONFIRMED", "TRAIN-only: republished values carry revisions (or no revision trail)"
        elif pre["R_rate"] >= 0.5:
            status, qualifier = "CLEARED", "revision channel cleared; byte-level republication differences unobservable"
    elif post["R_rate"] < 0.2 and pre["R"] >= C1_MIN_R and pre["R_rate"] < 0.2:
        status, qualifier = "CONFIRMED", "symmetric: revisions overwritten in both eras (both roles)"
    finding = (f"republished era {pre['first']}..{pre['last']}: {pre['pairs']} cycle pairs, {pre['rows']} rows, "
               f"R {pre['R']} ({f3(pre['R_per_1000_rows'], '{:.2f}')}/1000), R_differs/R {f3(pre['R_rate'])}, nonR "
               f"mismatch {f3(pre['nonR_rate'], '{:.4f}')}; original era {post['first']}..{post['last']}: "
               f"{post['pairs']} pairs, {post['rows']} rows, R {post['R']} ({f3(post['R_per_1000_rows'], '{:.2f}')}/1000), "
               f"R_differs/R {f3(post['R_rate'])}, nonR mismatch {f3(post['nonR_rate'], '{:.4f}')}"
               + (f" ({qualifier})" if qualifier else ""))
    return {"status": status, "finding": finding, "criterion": criterion, "metrics": {"eras": eras, "partial": partial}}


# ------------------------------------------------------------------------------ C2 (TRAIN only)
def check_c2(a, budget: Budget) -> dict:
    role = Role(a.train_role, a.train_role_sha)
    need(int(role.m["score_end_ns"]) <= int(np.datetime64(TRAIN_END, "ns").astype(np.int64)),
         "C2: the TRAIN role extends past 2022 (TRAIN-only check refuses)")
    ses = role.sessions()
    wraw = (a.weights_dir / "composition_weights.json").read_bytes()
    need(a.weights_sha is None or sha256_bytes(wraw) == a.weights_sha, "C2: composition_weights.json SHA != pin")
    w = json.loads(wraw)
    need(w.get("train_manifest_sha256") == role.sha, "C2: weights are not bound to this TRAIN role")
    prov = {c["id"]: c for c in w["provenance"]["candidates"]}
    fm = json.loads(a.train_fields.read_bytes())
    need(fm.get("role", {}).get("manifest_sha256") == role.sha, "C2: TRAIN fields manifest binds another role")
    si_entry = next(f for f in fm["fields"] if f["name"] == "si_shares")
    safe = np.datetime64(si_entry["coverage"]["vintage_risk"]["first_session_vintage_safe"], "D")
    base = a.work_dir / role.sha
    need(base.is_dir(), f"C2: fitter work dir {base} missing")
    rows = {}
    for cid in C2_SI + C2_CONTROLS:
        need(cid in prov, f"C2: {cid} not in the weights provenance")
        payload = prov[cid]["cache_payload_sha256"]
        series = []
        for p in sorted(base.glob(f"*/factors/{payload}*.json")):
            j = json.loads(p.read_bytes())
            if j.get("candidate_id") != cid or j.get("role_manifest_sha256") != role.sha:
                continue
            ctx = json.loads((p.parent.parent / "context" / "context.json").read_bytes())
            b, e = int(ctx["decision_begin"]), int(ctx["decision_end_exclusive"])
            f = np.array([np.nan if v is None else float(v) for v in j["f_unsigned"]])
            need(len(f) == e - b, f"C2: {p.name} length {len(f)} != decisions {e - b}")
            series.append((b, e, f))
        need(series, f"C2: no fitter factor record for {cid} (payload {payload[:12]})")
        b, e, f = series[0]
        for b2, e2, f2 in series[1:]:
            need((b2, e2) == (b, e) and np.array_equal(f2, f, equal_nan=True), f"C2: conflicting records for {cid}")
        days = ses[b:e]
        need(days[-1] < np.datetime64(TRAIN_END, "D"), "C2: a decision on/after 2023-01-01 (refused)")
        need(e + 1 < role.D, "C2: forward return row outside the TRAIN role")
        f = f * int(prov[cid]["sign"])
        rep, n_rep = sharpe(f[days < safe])
        saf, n_saf = sharpe(f[days >= safe])
        full, n_full = sharpe(f)
        rows[cid] = {"sign": int(prov[cid]["sign"]), "weight": prov[cid].get("weight"), "sr_republished": rep,
                     "days_republished": n_rep, "sr_vintage_safe": saf, "days_vintage_safe": n_saf,
                     "sr_full_train": full, "drop": (rep - saf) if rep is not None and saf is not None else None}
        budget.rss("C2")
    drops = lambda ids: [rows[i]["drop"] for i in ids if rows[i]["drop"] is not None]
    si_d, ct_d = drops(C2_SI), drops(C2_CONTROLS)
    si_drop = float(np.mean(si_d)) if len(si_d) == len(C2_SI) else None
    ctrl_drop = float(np.mean(ct_d)) if ct_d else None
    criterion = ("CONFIRMED if the SI candidates' mean Sharpe drop (republished - vintage-safe) > 1.0, exceeds the "
                 "controls' mean drop by > 0.75, and each SI republished Sharpe > 2x max(vintage-safe, 0); CLEARED if "
                 "the SI mean drop <= 0.5 or is within 0.25 of the controls' (regime-wide, not SI-specific); "
                 "INCONCLUSIVE otherwise")
    status = "INCONCLUSIVE"
    if si_drop is not None and ctrl_drop is not None:
        if (si_drop > 1.0 and si_drop - ctrl_drop > 0.75 and
                all(rows[i]["sr_republished"] > 2 * max(rows[i]["sr_vintage_safe"], 0) for i in C2_SI)):
            status = "CONFIRMED"
        elif si_drop <= 0.5 or si_drop - ctrl_drop <= 0.25:
            status = "CLEARED"
    finding = (f"TRAIN decisions split at {safe} (first vintage-safe session); oriented Sharpe republished -> "
               f"vintage-safe: " + "; ".join(f"{i} {f3(r['sr_republished'], '{:.2f}')} -> {f3(r['sr_vintage_safe'], '{:.2f}')}"
                                            for i, r in rows.items())
               + f"; SI mean drop {f3(si_drop, '{:.2f}')} vs controls {f3(ctrl_drop, '{:.2f}')}")
    return {"status": status, "finding": finding, "criterion": criterion,
            "metrics": {"split_session": str(safe), "candidates": rows, "si_mean_drop": si_drop,
                        "control_mean_drop": ctrl_drop, "train_role_sha256": role.sha}}


# ------------------------------------------------------------------------------ C3
def check_c3(a, budget: Budget) -> dict:
    import pandas as pd
    sch = read_schedule(a)
    by_diss: dict[str, list[int]] = {}
    for i, d in enumerate(sch.dissemination_date):
        by_diss.setdefault(d, []).append(i)
    asof = a.finra / "asof" / "si_shares.csv"
    asof_sha = sha256_file(asof)
    pinned = None
    if a.train_fields.is_file():
        fm = json.loads(a.train_fields.read_bytes())
        for f in fm.get("fields", []):
            if f.get("name") == "si_shares":
                pinned = next((s["sha256"] for s in f.get("sources", []) if str(s.get("path", "")).endswith("si_shares.csv")), None)
    x = pd.read_csv(asof, dtype={"security_id": "int64", "available_at": str, "value": str})
    x = x[(x.available_at >= "2019-12-01") & (x.available_at < SEAL)]
    not_in_schedule = int((~x.available_at.isin(by_diss.keys())).sum())
    x = x[pd.to_numeric(x.value, errors="coerce") >= C3_MIN_VALUE]
    rng = np.random.default_rng(17)
    hits = {"own": 0, "own_unique": 0, "prev_only": 0, "next_only": 0, "none": 0}
    per_era = {}
    missing_files, sampled_dates, partial = set(), 0, False
    for era, mask in (("republished", x.available_at < C3_SPLIT), ("original", x.available_at >= C3_SPLIT)):
        dates = sorted(set(x.available_at[mask]) & set(by_diss))
        pick = rng.choice(dates, min(C3_DATES_PER_ERA, len(dates)), replace=False) if dates else []
        eh = {k: 0 for k in hits}
        for av in sorted(pick):
            if budget.over():
                partial = True
                break
            idx = by_diss[av]
            settle = [sch.settlement_date[i] for i in idx]
            prev_i, next_i = min(idx) - 1, max(idx) + 1
            own_sets = [value_set(a, s) for s in settle]
            for s, vs in zip(settle, own_sets):
                if vs is None:
                    missing_files.add(s)
            prev_s = sch.settlement_date[prev_i] if prev_i >= 0 else None
            next_s = sch.settlement_date[next_i] if next_i < len(sch) and sch.settlement_date[next_i] < SEAL else None
            prev_vs = value_set(a, prev_s) if prev_s else None
            next_vs = value_set(a, next_s) if next_s else None
            g = x[x.available_at == av]
            for v in g.value.sample(min(C3_ROWS_PER_DATE, len(g)), random_state=1).astype(float):
                own = any(vs is not None and v in vs for vs in own_sets)
                prv = prev_vs is not None and v in prev_vs
                nxt = next_vs is not None and v in next_vs
                key = "own" if own else "prev_only" if prv else "next_only" if nxt else "none"
                eh[key] += 1
                if own and not prv and not nxt:
                    eh["own_unique"] += 1
            sampled_dates += 1
            budget.rss("C3")
        per_era[era] = eh
        for k in hits:
            hits[k] += eh[k]
    total = hits["own"] + hits["prev_only"] + hits["next_only"] + hits["none"]
    criterion = ("CLEARED if >= 95% of sampled values are found in the settlement disseminated on their available_at, "
                 "no value is found only in the NEXT settlement, and every available_at is an official dissemination "
                 "date; CONFIRMED (one-cycle look-ahead, both roles) if next_only >= 2 and >= 2% of samples; "
                 f"INCONCLUSIVE otherwise or with < {C3_MIN_SAMPLES} samples")
    status = "INCONCLUSIVE"
    if total >= C3_MIN_SAMPLES and hits["next_only"] >= 2 and hits["next_only"] / total >= 0.02:
        status = "CONFIRMED"
    elif (total >= C3_MIN_SAMPLES and hits["own"] / total >= 0.95 and hits["next_only"] == 0
          and not_in_schedule == 0):
        status = "CLEARED"
    finding = (f"{total} sampled values (>= {C3_MIN_VALUE:,} shares) over {sampled_dates} dissemination dates: own "
               f"settlement {hits['own']} (unique to it {hits['own_unique']}), previous-only {hits['prev_only']}, "
               f"next-only {hits['next_only']}, none {hits['none']}; available_at not in the schedule: {not_in_schedule}; "
               f"as-of CSV SHA {'matches' if pinned == asof_sha else 'DIFFERS from' if pinned else 'not compared to'} "
               f"the frozen TRAIN fields-v4 pin" + ("; stopped at the time budget" if partial else "")
               + (f"; missing raw files for {sorted(missing_files)[:5]}" if missing_files else ""))
    return {"status": status, "finding": finding, "criterion": criterion,
            "metrics": {"hits": hits, "per_era": per_era, "not_in_schedule": not_in_schedule, "asof_sha256": asof_sha,
                        "asof_sha256_frozen_fields_v4": pinned, "partial": partial,
                        "missing_raw_settlements": sorted(missing_files)}}


# ------------------------------------------------------------------------------ C4
def check_c4(a, budget: Budget) -> dict:
    out = {}
    for tag, directory, pin in (("TRAIN", a.train_role, a.train_role_sha), ("VAL", a.val_role, a.val_role_sha)):
        role = Role(directory, pin)
        ses = role.sessions()
        pres = role.matrix("present.u8", "u1").astype(bool)
        mem = role.matrix("member.u8", "u1").astype(bool)
        D = role.D
        last = np.where(pres.any(0), D - 1 - np.argmax(pres[::-1], 0), -1)
        del pres
        yr = years_of(ses)
        years = {}
        for y in np.unique(yr[role.begin:]):
            rows = np.flatnonzero(yr == y)
            i0, i1 = int(rows[0]), int(min(rows[-1] + 1, D - 5))
            cohort = mem[i0]
            gone = cohort & (last >= i0) & (last < i1)
            n = int(cohort.sum())
            years[str(int(y))] = {"members_at_first_session": n, "vanished_in_year": int(gone.sum()),
                                  "rate": gone.sum() / n if n else None}
        out[tag] = {"role_sha256": role.sha, "years": years}
        budget.rss("C4")
    rates = {t: [v["rate"] for v in out[t]["years"].values() if v["rate"] is not None] for t in out}
    tm, vm = (float(np.mean(rates[t])) if rates[t] else None for t in ("TRAIN", "VAL"))
    all_rates = rates["TRAIN"] + rates["VAL"]
    ratio = tm / vm if tm is not None and vm else None
    criterion = ("CLEARED if every scored year's vanish rate is >= 2.5% and the TRAIN/VAL mean ratio is within "
                 "[0.67, 1.5] (vendor keeps dead lines); CONFIRMED (survivorship) if any year < 1.5% or TRAIN mean < "
                 "0.5x VAL mean; INCONCLUSIVE otherwise")
    status = "INCONCLUSIVE"
    if all_rates and (min(all_rates) < 0.015 or (ratio is not None and ratio < 0.5)):
        status = "CONFIRMED"
    elif all_rates and min(all_rates) >= 0.025 and ratio is not None and 0.67 <= ratio <= 1.5:
        status = "CLEARED"
    finding = ("year-start members whose vendor rows end within the year: "
               + "; ".join(f"{t} " + ", ".join(f"{y} {f3(v['rate'], '{:.2%}')} ({v['vanished_in_year']}/"
                                                f"{v['members_at_first_session']})" for y, v in out[t]["years"].items())
                           for t in out)
               + f"; mean TRAIN {f3(tm, '{:.2%}')} vs VAL {f3(vm, '{:.2%}')} (ratio {f3(ratio, '{:.2f}')})")
    return {"status": status, "finding": finding, "criterion": criterion,
            "metrics": {"roles": out, "train_mean": tm, "val_mean": vm, "ratio": ratio}}


# ------------------------------------------------------------------------------ C5 (book level)
def check_c5(a, budget: Budget) -> dict:
    import pandas as pd
    runs = {}
    for tag, run in (("TRAIN", a.train_nav), ("VAL", a.val_nav)):
        d = pd.read_csv(run / f"daily_{a.scenario}.csv",
                        usecols=["session_ns", "pretrade_gross_dollars", "held_names", "guarded_intervals",
                                 "writeoff_return"])
        e = pd.read_csv(run / f"events_{a.scenario}.csv", usecols=["kind", "session_ns", "exposure_dollars", "pnl_dollars"])
        d["y"] = pd.to_datetime(d.session_ns).dt.year
        e["y"] = pd.to_datetime(e.session_ns).dt.year
        e["absx"] = e.exposure_dollars.abs()
        gmv = d.groupby("y").pretrade_gross_dollars.mean()
        years = {}
        for y in sorted(d.y.unique()):
            ey = e[e.y == y]
            wo = ey[ey.kind == "write-off"]
            gu = ey[ey.kind == "guarded"]
            dy = d[d.y == y]
            g = float(gmv.get(y, float("nan")))
            years[str(int(y))] = {
                "mean_pretrade_gmv": g, "writeoff_events": int(len(wo)),
                "writeoff_exposure_pct_mean_gmv": 100 * float(wo.absx.sum()) / g if g > 0 else None,
                "writeoff_pnl": float(wo.pnl_dollars.sum()), "writeoff_return_sum": float(dy.writeoff_return.sum()),
                "guarded_intervals": int(dy.guarded_intervals.sum()),
                "guarded_per_1000_held_name_days": 1000 * float(dy.guarded_intervals.sum()) / max(float(dy.held_names.sum()), 1.0),
                "guard_sensitivity_pct_mean_gmv": 100 * float(gu.pnl_dollars.sum()) / g if g > 0 else None}
        runs[tag] = {"run": str(run), "years": years,
                     "guarded_per_1000_held_name_days": 1000 * float(d.guarded_intervals.sum()) / max(float(d.held_names.sum()), 1.0)}
        budget.rss("C5")
    wo = [v["writeoff_exposure_pct_mean_gmv"] for r in runs.values() for v in r["years"].values()
          if v["writeoff_exposure_pct_mean_gmv"] is not None]
    wo_max = max(wo) if wo else None
    tg, vg = runs["TRAIN"]["guarded_per_1000_held_name_days"], runs["VAL"]["guarded_per_1000_held_name_days"]
    wo_status = "INCONCLUSIVE" if wo_max is None else "CLEARED" if wo_max < 2 else "CONFIRMED" if wo_max >= 5 else "INCONCLUSIVE"
    g_status = ("CONFIRMED" if vg > 3 * tg and vg > 0 else "CLEARED" if vg <= 1.5 * tg or vg < 0.01 else "INCONCLUSIVE")
    status = ("CONFIRMED" if "CONFIRMED" in (wo_status, g_status) else
              "CLEARED" if wo_status == g_status == "CLEARED" else "INCONCLUSIVE")
    criterion = ("write-off: CLEARED if every run-year's write-off exposure < 2% of mean pre-trade GMV, CONFIRMED "
                 "(material) if any >= 5%; guard: CONFIRMED (VAL role artifacts) if VAL guarded intervals per held "
                 "name-day > 3x TRAIN, CLEARED if <= 1.5x; overall CONFIRMED if either is, CLEARED if both are")
    finding = ("write-off exposure % mean GMV by year: "
               + "; ".join(f"{t} " + ", ".join(f"{y} {f3(v['writeoff_exposure_pct_mean_gmv'], '{:.2f}')}%"
                                                f" ({v['writeoff_events']} ev)" for y, v in r["years"].items())
                           for t, r in runs.items())
               + f" [{wo_status}]; guarded intervals per 1000 held name-days TRAIN {tg:.3f} vs VAL {vg:.3f} [{g_status}]")
    return {"status": status, "finding": finding, "criterion": criterion,
            "metrics": {"runs": runs, "writeoff_max_pct": wo_max, "writeoff_status": wo_status, "guard_status": g_status,
                        "scenario": a.scenario}}


# ------------------------------------------------------------------------------ C6
def check_c6(a, budget: Budget) -> dict:
    out = {}
    for tag, directory, pin in (("TRAIN", a.train_role, a.train_role_sha), ("VAL", a.val_role, a.val_role_sha)):
        role = Role(directory, pin)
        c = role.matrix("close.f64", "<f8")
        r = role.matrix("raw_close.f64", "<f8")
        p = role.matrix("present.u8", "u1").astype(bool)
        mem = role.matrix("member.u8", "u1").astype(bool)
        allm = np.full(role.D, np.nan)
        memm = np.full(role.D, np.nan)
        with np.errstate(all="ignore"):
            for t in range(max(role.begin, 1), role.D):
                ret = c[t] / c[t - 1] - 1
                la = np.log(c[t]) - np.log(c[t - 1])
                lr = np.log(r[t]) - np.log(r[t - 1])
                ok = (p[t] & p[t - 1] & np.isfinite(ret) & (c[t - 1] > 0) & (r[t - 1] > 0)
                      & ~((np.abs(la) > 1.5) | (np.abs(la) > np.abs(lr) + .10)))
                if ok.any():
                    allm[t] = ret[ok].mean()
                okm = ok & mem[t - 1]
                if okm.any():
                    memm[t] = ret[okm].mean()
        k = np.isfinite(allm) & np.isfinite(memm)
        diff = allm[k] - memm[k]
        corr = float(np.corrcoef(allm[k], memm[k])[0, 1]) if k.sum() > 2 else None
        out[tag] = {"role_sha256": role.sha, "sessions": int(k.sum()), "corr": corr,
                    "mean_diff_bp_per_day": float(diff.mean() * 1e4) if len(diff) else None,
                    "sd_diff_bp": float(diff.std() * 1e4) if len(diff) else None,
                    "union_names": role.N}
        del c, r, p, mem
        budget.rss("C6")
    t, v = out["TRAIN"], out["VAL"]
    criterion = ("CLEARED if corr > 0.98 and |mean difference| < 2 bp/day in both roles; CONFIRMED (material, "
                 "TRAIN-specific) if TRAIN |mean difference| >= 5 bp/day and > 2x VAL's; INCONCLUSIVE otherwise")
    status = "INCONCLUSIVE"
    vals_ok = all(x["corr"] is not None and x["mean_diff_bp_per_day"] is not None for x in (t, v))
    if vals_ok:
        if abs(t["mean_diff_bp_per_day"]) >= 5 and abs(t["mean_diff_bp_per_day"]) > 2 * abs(v["mean_diff_bp_per_day"]):
            status = "CONFIRMED"
        elif all(x["corr"] > 0.98 and abs(x["mean_diff_bp_per_day"]) < 2 for x in (t, v)):
            status = "CLEARED"
    finding = ("ALL-union minus member-only equal-weight market over scored sessions: "
               + "; ".join(f"{k} corr {f3(x['corr'], '{:.4f}')}, mean {f3(x['mean_diff_bp_per_day'], '{:+.2f}')} bp/day "
                           f"(sd {f3(x['sd_diff_bp'], '{:.2f}')} bp, {x['sessions']} sessions, union {x['union_names']})"
                           for k, x in out.items()))
    return {"status": status, "finding": finding, "criterion": criterion, "metrics": out}


# ------------------------------------------------------------------------------ C7
def check_c7(a, budget: Budget) -> dict:
    from pandas.tseries.holiday import USFederalHolidayCalendar
    s = read_schedule(a)
    s = s[(s.settlement_date >= "2019-06-01") & (s.settlement_date < SEAL)].copy()
    need(len(s) > 0, "C7: no schedule rows in [2019-06-01, 2025-01-01)")
    label_col = "third_column_label" if "third_column_label" in s.columns else None
    days = lambda col: np.array([str(v) for v in col], dtype="datetime64[D]")  # pandas-version independent
    hol = days(d.date().isoformat() for d in USFederalHolidayCalendar().holidays("2019-01-01", "2025-12-31"))
    settle, diss = days(s.settlement_date), days(s.dissemination_date)
    s["bd"] = np.busday_count(settle, diss, holidays=hol)
    s["label"] = s[label_col].astype(str) if label_col else "(no label column)"
    s["source_kind"] = s["source"].astype(str) if "source" in s.columns else "(none)"
    s["era"] = np.where(s.settlement_date < "2021-01-01", "pre-2021", "2021+")
    groups = {}
    for (era, label, src), g in s.groupby(["era", "label", "source_kind"]):
        groups[f"{era} | {label} | {src}"] = {"rows": int(len(g)), "bd_min": int(g.bd.min()), "bd_median": float(g.bd.median()),
                                            "bd_max": int(g.bd.max())}
    due_after = None
    if "due_date" in s.columns:
        dd = s.due_date.replace("", np.nan).dropna()
        due_after = int((days(dd) >= days(s.loc[dd.index, "dissemination_date"])).sum())
    early = bool((s.bd <= 5).any()) or any("due" in str(l).lower() for l in s.label.unique())
    pre_labels = sorted(set(s.label[s.era == "pre-2021"]))
    pre_ok = bool(pre_labels) and all(("receipt" in l.lower() or "exchange" in l.lower()) for l in pre_labels)
    in_range = bool(((s.bd >= 6) & (s.bd <= 9)).all())
    criterion = ("CLEARED if every settlement 2019-06..2024-12 has 6-9 business days to dissemination and pre-2021 labels "
                 "are the exchange receipt date; CONFIRMED (TRAIN SI clock early) if any offset <= 5 or a 'due' label; "
                 "INCONCLUSIVE otherwise")
    status = "CONFIRMED" if early else "CLEARED" if (in_range and pre_ok and not due_after) else "INCONCLUSIVE"
    finding = ("business days settlement -> dissemination (US federal holidays): "
               + "; ".join(f"{k}: n {v['rows']}, {v['bd_min']}-{v['bd_max']} (median {v['bd_median']:.0f})" for k, v in groups.items())
               + (f"; due_date >= dissemination in {due_after} rows" if due_after else ""))
    return {"status": status, "finding": finding, "criterion": criterion,
            "metrics": {"groups": groups, "pre_2021_labels": pre_labels, "due_on_or_after_dissemination": due_after}}


# ------------------------------------------------------------------------------ C8
def check_c8(a, budget: Budget) -> dict:
    import pandas as pd
    import pyarrow as pa
    import pyarrow.compute as pc
    import pyarrow.parquet as pq
    ids = np.union1d(Role(a.train_role, a.train_role_sha).array("ids.u64", "<u8"),
                     Role(a.val_role, a.val_role_sha).array("ids.u64", "<u8")).astype(np.int64)
    rng = np.random.default_rng(3)
    smp = pa.array(np.sort(rng.choice(ids, min(C8_SAMPLE_IDS, len(ids)), replace=False)), type=pa.int64())
    cols = ["tradingDate", "securityID", "earnFlag", "atmCenI_21d", "atmCenH_21d"]
    pf = pq.ParquetFile(a.tickerhistory, memory_map=False)
    schema = pf.schema_arrow
    need(schema.field("tradingDate").type == pa.date32() and schema.field("securityID").type == pa.int64(),
         "C8: TickerHistory3 date/id column types")
    lo = pa.scalar(dt.date(2019, 12, 1), type=pa.date32())
    hi = pa.scalar(dt.date(2025, 1, 1), type=pa.date32())
    parts, scanned, partial = [], 0, False
    for batch in pf.iter_batches(batch_size=262144, columns=cols, use_threads=False):
        if budget.over():
            partial = True
            break
        scanned += batch.num_rows
        keep = pc.and_(pc.is_in(batch.column("securityID"), value_set=smp),
                       pc.and_(pc.greater_equal(batch.column("tradingDate"), lo), pc.less(batch.column("tradingDate"), hi)))
        fb = batch.filter(pc.fill_null(keep, False))
        if fb.num_rows:
            parts.append(fb)
        budget.rss("C8")
    need(parts, "C8: no sampled rows")
    t = pa.Table.from_batches(parts).to_pandas()
    del parts
    t["tradingDate"] = pd.to_datetime(t.tradingDate)
    t = t.sort_values(["securityID", "tradingDate"]).reset_index(drop=True)
    t["year"] = t.tradingDate.dt.year
    t["period"] = np.where(t.year >= 2023, "VAL-era 2023-24", np.where(t.year >= 2020, "TRAIN-era 2020-22", "pre"))
    for c in ("atmCenI_21d", "atmCenH_21d"):
        v = t[c].astype(np.float64).where(lambda s: s > 0)
        lv = np.log(v)
        t["d_" + c] = lv - lv.groupby(t.securityID).shift(1)
        t["n_" + c] = t["d_" + c].groupby(t.securityID).shift(-1)
    ev = t[(t.earnFlag.astype(str).str.strip() == "0") & (t.period != "pre")]
    t["adj"] = t.atmCenI_21d.notna() & t.atmCenH_21d.notna() & (t.atmCenI_21d != t.atmCenH_21d)
    periods = {}
    for per in ("TRAIN-era 2020-22", "VAL-era 2023-24"):
        e = ev[ev.period == per]
        r = {"events": int(len(e))}
        for c in ("atmCenI_21d", "atmCenH_21d"):
            r["day0_" + c] = float(e["d_" + c].median()) if len(e) else None
            r["next_" + c] = float(e["n_" + c].median()) if len(e) else None
        # the series with the deeper day-0 median carries the crush
        cands = [(r["day0_" + c], r["next_" + c]) for c in ("atmCenI_21d", "atmCenH_21d")
                 if r["day0_" + c] is not None and r["next_" + c] is not None and math.isfinite(r["day0_" + c])]
        if cands:
            d0, nx = min(cands, key=lambda z: z[0])
            r["crush_on_day0"] = bool(d0 < -0.03 and abs(nx) < abs(d0) / 2)
            r["crush_on_next_day"] = bool(nx < -0.03 and abs(nx) > abs(d0))
        else:
            r["crush_on_day0"] = r["crush_on_next_day"] = None
        r["adjustment_incidence"] = float(t.adj[t.period == per].mean()) if (t.period == per).any() else None
        periods[per] = r
    by_year = {str(int(y)): float(g.mean()) for y, g in t.adj.groupby(t.year) if y >= 2020}
    tr, va = periods["TRAIN-era 2020-22"], periods["VAL-era 2023-24"]
    inc_ratio = (va["adjustment_incidence"] / tr["adjustment_incidence"]
                 if tr["adjustment_incidence"] and va["adjustment_incidence"] is not None else None)
    criterion = ("CLEARED if in both periods the IV crush falls on the earnFlag-0 session (median d-log < -0.03, next "
                 "day < half of it) and the earnings-adjustment incidence (I != H) ratio VAL/TRAIN is within [0.67, 1.5]; "
                 "CONFIRMED (asymmetric) if the crush day differs between periods or the ratio is outside [0.33, 3]; "
                 "INCONCLUSIVE otherwise. Never settles the calendar vintage (#14 stays UNKNOWN)")
    status = "INCONCLUSIVE"
    if not partial and tr["events"] >= 30 and va["events"] >= 30:
        differ = (tr["crush_on_day0"] != va["crush_on_day0"]) or (tr["crush_on_next_day"] != va["crush_on_next_day"])
        if differ or (inc_ratio is not None and not 1 / 3 <= inc_ratio <= 3):
            status = "CONFIRMED"
        elif tr["crush_on_day0"] and va["crush_on_day0"] and inc_ratio is not None and 0.67 <= inc_ratio <= 1.5:
            status = "CLEARED"
    fmt = lambda r: (f"{r['events']} flag-0 events, median d-log I {f3(r['day0_atmCenI_21d'])} (next {f3(r['next_atmCenI_21d'])}), "
                     f"H {f3(r['day0_atmCenH_21d'])} (next {f3(r['next_atmCenH_21d'])}), I!=H incidence "
                     f"{f3(r['adjustment_incidence'], '{:.3f}')}")
    finding = (f"{len(smp)} sampled ids, {scanned:,} rows decoded: TRAIN-era {fmt(tr)}; VAL-era {fmt(va)}; incidence ratio "
               f"{f3(inc_ratio, '{:.2f}')}" + ("; stopped at the time budget" if partial else "")
               + ". Calendar vintage (#14) is not settled by this check")
    return {"status": status, "finding": finding, "criterion": criterion,
            "metrics": {"periods": periods, "incidence_by_year": by_year, "incidence_ratio": inc_ratio,
                        "sampled_ids": len(smp), "rows_decoded": scanned, "partial": partial}}


FUNCS = {"C1": check_c1, "C2": check_c2, "C3": check_c3, "C4": check_c4, "C5": check_c5, "C6": check_c6,
         "C7": check_c7, "C8": check_c8}


# ------------------------------------------------------------------------------ output
def write_atomic(path: Path, data: bytes) -> None:
    tmp = path.with_name(path.name + f".tmp-{os.getpid()}")
    with open(tmp, "wb") as fh:
        fh.write(data)
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, path)


def load_doc(out_dir: Path) -> dict:
    path = out_dir / "t17_checks.json"
    if path.is_file():
        doc = json.loads(path.read_bytes())
        need(doc.get("schema") == SCHEMA, f"{path}: not a {SCHEMA} document")
        return doc
    return {"schema": SCHEMA, "checks": {}}


def render_md(doc: dict) -> str:
    lines = ["# T17 root checks (PIT / look-ahead follow-ups)", "",
             f"schema {doc['schema']}; updated {doc.get('updated')}; checks present: "
             + ", ".join(c for c in CHECKS if c in doc["checks"]) + ". Criteria as written in task-T17-report.md.", ""]
    for c in CHECKS:
        r = doc["checks"].get(c)
        if r is None:
            continue
        lines.append(f"- **{c} — {TITLES[c]}.** Found: {r.get('finding')}. Criterion: {r.get('criterion')}. "
                     f"**{r.get('status')}** ({f3(r.get('elapsed_s'), '{:.1f}')} s, peak {f3(r.get('peak_rss_mib'), '{:.0f}')} MiB, "
                     f"run {r.get('run_at')}).")
        lines.append("")
    return "\n".join(lines)


def save_doc(out_dir: Path, doc: dict) -> None:
    doc["updated"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    doc = clean(doc)
    write_atomic(out_dir / "t17_checks.json", (json.dumps(doc, indent=1, sort_keys=True, allow_nan=False) + "\n").encode())
    write_atomic(out_dir / "t17_checks.md", render_md(doc).encode("utf-8"))


# ------------------------------------------------------------------------------ CLI
def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    p.add_argument("--root", type=Path, default=DEFAULT_ROOT, help="tree whose build-equity holds the frozen artifacts")
    p.add_argument("--output", type=Path, required=True, help="directory; t17_checks.json/.md merge here")
    p.add_argument("--checks", default=",".join(CHECKS), help="comma list of C1..C8")
    p.add_argument("--finra", type=Path, default=DEFAULT_FINRA, help="FINRA root (raw/, asof/, dissemination_schedule.csv)")
    p.add_argument("--tickerhistory", type=Path, default=DEFAULT_TH)
    p.add_argument("--budget-seconds", type=float, default=170.0, help="soft deadline for the whole invocation")
    p.add_argument("--max-rss-mib", type=float, default=1400.0, help="hard RSS guard (the cap is 1536 MiB)")
    p.add_argument("--scenario", default=SCENARIO)
    p.add_argument("--train-role", type=Path, default=None)
    p.add_argument("--val-role", type=Path, default=None)
    p.add_argument("--train-fields", type=Path, default=None, help="TRAIN fields-v4 manifest.json")
    p.add_argument("--weights-dir", type=Path, default=None)
    p.add_argument("--work-dir", type=Path, default=None, help="fitter --work-dir (ROOT/<train sha>/<tag>/...)")
    p.add_argument("--train-nav", type=Path, default=None)
    p.add_argument("--val-nav", type=Path, default=None)
    p.add_argument("--train-role-sha", default=FROZEN_TRAIN_ROLE_SHA)
    p.add_argument("--val-role-sha", default=FROZEN_VAL_ROLE_SHA)
    p.add_argument("--weights-sha", default=FROZEN_WEIGHTS_SHA)
    a = p.parse_args(argv)
    be = a.root / "build-equity"
    defaults = {"train_role": be / "recent-fast-train-2020-2022-v2", "val_role": be / "recent-fast-validation-2023-2024-v1",
                "train_fields": be / "recent-fast-train-2020-2022-v2-fields-v4/manifest.json",
                "weights_dir": be / "mega-weights-v3-plain", "work_dir": be / "mega-fit-work",
                "train_nav": be / "mega-nav-v3-plain-b1", "val_nav": be / "mega-nav-v3-VAL"}
    for k, v in defaults.items():
        if getattr(a, k) is None:
            setattr(a, k, v)
    return a


def main(argv=None) -> int:
    a = parse_args(argv)
    checks = [c.strip().upper() for c in a.checks.split(",") if c.strip()]
    if not checks or any(c not in CHECKS for c in checks):
        print(f"t17_checks: --checks must be a subset of {','.join(CHECKS)}", file=sys.stderr)
        return 2
    out_dir = Path(a.output)
    out_dir.mkdir(parents=True, exist_ok=True)
    try:
        doc = load_doc(out_dir)
    except (CheckError, ValueError) as exc:
        print(f"t17_checks: {exc}", file=sys.stderr)
        return 2
    budget = Budget(a.budget_seconds, a.max_rss_mib)
    started, rc = time.perf_counter(), 0
    for c in CHECKS:
        if c not in checks:
            continue
        tick = time.perf_counter()
        try:
            result = FUNCS[c](a, budget)
        except (CheckError, OSError, KeyError, ValueError, StopIteration) as exc:
            result = {"status": "ERROR", "finding": f"{type(exc).__name__}: {exc}", "criterion": "n/a", "metrics": {}}
            rc = 1
        except Exception as exc:  # unexpected: loud, recorded, non-zero
            traceback.print_exc()
            result = {"status": "ERROR", "finding": f"{type(exc).__name__}: {exc}", "criterion": "n/a", "metrics": {}}
            rc = 1
        result["elapsed_s"] = round(time.perf_counter() - tick, 2)
        result["peak_rss_mib"] = round(rss_mib()[1], 1)
        result["run_at"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        result["inputs"] = {"root": str(a.root), "finra": str(a.finra), "tickerhistory": str(a.tickerhistory)}
        doc["checks"][c] = result
        save_doc(out_dir, doc)
        print(f"[t17] {c} {result['status']} in {result['elapsed_s']:.1f} s, peak RSS {result['peak_rss_mib']:.0f} MiB: "
              f"{result['finding'][:240]}", flush=True)
    print(f"[t17] total {time.perf_counter() - started:.1f} s; wrote {out_dir / 't17_checks.json'} and t17_checks.md",
          flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
