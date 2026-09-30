#!/usr/bin/env python3
"""Daily book monitor M1-M4 (platform v7 W3; literature-v7 S7): flags only, never an action.

  book_monitor.py --baseline --output DIR [inputs]              TRAIN reference distributions (write once)
  book_monitor.py --reference DIR/monitor.json --output DIR2 [inputs]   live: compare to the reference

Inputs (each optional; a check without its input is ``n/a`` with the reason):
  --decide DIR          a decide output (decision.json: health, transfer coefficient); its holdings_days.csv is
                        used when --holdings-days is not given
  --holdings-days CSV   the NAV replay's --emit-holdings per-session summary (M1 realized NAV vol, M3)
  --bias CSV            the risk verb's bias.csv; rows with family=book give M1 (risk ... --book-weights)
  --daily-ic CSV        the u pass train_daily_ic.csv (M2 IC; h=5 rank IC times the admission sign s_k)
  --admission JSON      admission.json: the admitted members (M2, M4) and their signs
  --sleeve-daily CSV    the report card's daily_sleeve.csv (M2 turnover per member)
  --fit-work DIR        the fitter WorkStore: price-risk-v1 neutralized member returns f_k (M4 crowding)

Checks and declared thresholds (literature-v7 S7; [est] values are declared here, not fitted):
  M1 risk       b63 = sample SD of z = r / sigma_hat(t-1) of the book over the last 63 sessions; ok inside
                1 +- sqrt(2/63), warn inside the kurtosis-widened band 1 +- max(sqrt(2/63), 1.96 sqrt((k-1)/(4*63)))
                (k = the reference's pooled raw kurtosis of z), alarm outside it. Info: realized / ex-ante vol (Qian-Hua multiplier) and the book's
                63-session realized vol from the NAV (pretrade_nav(t) / posttrade_nav(t-1) - 1).
  M2 alpha      per admitted member: IC63 and IC252 = mean of the last 63 / 252 daily h=5 rank ICs (times s_k), warn
                outside the reference's p5-p95 of the rolling statistic; IC CUSUM (lower side) on weekly observations
                (every 5th scored decision: non-overlapping 5-day labels) standardized by the reference weekly mean
                and SD, S = max(0, S + (mu0 - x) / sd0 - k), k = .5, alarm at S >= h = 5, warn at S >= h / 2;
                turnover: T63 = mean of the last 63 daily turnovers vs the reference p5-p95 (warn), and a two-sided
                CUSUM on 5-decision block means (same k, h). The CUSUM starts at the first observation after the
                reference's last session (baseline: over the whole reference, in sample).
                In-control ARL (Siegmund, N(0,1) increments): (exp(-2 D b) + 2 D b - 1) / (2 D^2), D = -k,
                b = h + 1.166 -> 938 weekly observations (18.6 y); the ARL to a full decay (mean -> 0) is reported.
  M3 execution  cost/$ = sum(pretrade_nav - posttrade_nav) / sum(traded_dollars) over executed sessions (the replay's
                modelled cost, or live fills); ratio = last 21 executed sessions / the reference cost/$ (the model):
                alarm outside [.7, 1.3], warn outside the reference's p5-p95 of the rolling ratio. Fill rate
                sum(traded) / sum(traded + unfilled) over 21: warn below the reference p5, alarm below p1. Only the
                execution-cost part of implementation shortfall is observable here (no decision prices per name):
                the opportunity cost of unfilled orders is n/a.
  M4 crowding   stock-level comomentum (Lou-Polk) needs per-name residual returns of the book's names: n/a (no
                holdings.csv / specific returns in the inputs). Sleeve-level proxy: mean pairwise correlation of the
                admitted members' neutralized daily returns f_k over the last 63 decisions; warn above the
                reference p95, alarm above p99 of the rolling statistic.
Output DIR/monitor.json (``atx.book-monitor/v1``: sorted keys, floats to 10 significant digits), published
atomically, never overwritten. Exit 0 whatever the flags (they are data); 1 on a refused input.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fit_composition_weights as fcw  # noqa: E402

SCHEMA = "atx.book-monitor/v1"
CUSUM_H, CUSUM_K = 5.0, 0.5
CUSUM_WARN = CUSUM_H / 2.0
CUSUM_STRIDE = 5
IC_HORIZON = 5
BIAS_WINDOW = 63
IC_WINDOWS = (63, 252)
TURN_WINDOW = 63
COST_WINDOW = 21
COST_BAND = (0.7, 1.3)
CROWD_WINDOW = 63
SIG_DIGITS = 10
RANK = {"ok": 0, "warn": 1, "alarm": 2}
EXIT_OK, EXIT_REFUSED = 0, 1


class MonitorError(Exception):
    pass


def require(cond, msg):
    if not cond:
        raise MonitorError(msg)


def q(x):
    if isinstance(x, dict):
        return {str(k): q(v) for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [q(v) for v in x]
    if isinstance(x, np.integer):
        return int(x)
    if isinstance(x, (bool, np.bool_)):
        return bool(x)
    if isinstance(x, (float, np.floating)):
        x = float(x)
        return float(f"{x:.{SIG_DIGITS}g}") if math.isfinite(x) else None
    return x


def canonical(doc) -> bytes:
    return (json.dumps(q(doc), sort_keys=True, indent=1, allow_nan=False) + "\n").encode("utf-8")


def na(reason: str, **extra) -> dict:
    return dict(extra, status="n/a", reason=reason)


def worst(statuses) -> str:
    s = [x for x in statuses if x in RANK]
    return max(s, key=RANK.get) if s else "n/a"


def pct(x: np.ndarray, p) -> float | None:
    x = np.asarray(x, dtype=float)
    x = x[np.isfinite(x)]
    return float(np.percentile(x, p)) if x.size else None


def rolling_mean(x: np.ndarray, w: int) -> np.ndarray:
    """Mean of the finite values among the last ``w`` observations (NaN until ``w`` observations exist or when
    fewer than half are finite)."""
    x = np.asarray(x, dtype=float)
    f = np.isfinite(x)
    cs = np.concatenate([[0.0], np.cumsum(np.where(f, x, 0.0))])
    cn = np.concatenate([[0], np.cumsum(f)])
    out = np.full(x.size, np.nan)
    if x.size >= w:
        s, n = cs[w:] - cs[:-w], cn[w:] - cn[:-w]
        out[w - 1:] = np.where(n >= (w + 1) // 2, s / np.maximum(n, 1), np.nan)
    return out


def last_window_mean(x: np.ndarray, w: int) -> tuple[float | None, int]:
    x = np.asarray(x, dtype=float)[-w:]
    f = x[np.isfinite(x)]
    if len(x) < w or f.size < (w + 1) // 2:
        return None, int(f.size)
    return float(f.mean()), int(f.size)


def siegmund_arl(mu: float, k: float = CUSUM_K, h: float = CUSUM_H) -> float:
    """Average run length of a one-sided CUSUM max(0, S + x - k) with x ~ N(mu, 1), alarm at h (Siegmund 1985)."""
    d, b = mu - k, h + 1.166
    if abs(d) < 1e-12:
        return b * b
    return (math.exp(-2.0 * d * b) + 2.0 * d * b - 1.0) / (2.0 * d * d)


def cusum(z: np.ndarray, side: str) -> np.ndarray:
    """Path of the one-sided CUSUM on standardized observations z: ``low`` detects a fall (S += -z - k),
    ``high`` a rise (S += z - k)."""
    s, out = 0.0, np.zeros(len(z))
    for j, v in enumerate(z):
        s = max(0.0, s + (-v if side == "low" else v) - CUSUM_K)
        out[j] = s
    return out


def cusum_status(value: float) -> str:
    return "alarm" if value >= CUSUM_H else ("warn" if value >= CUSUM_WARN else "ok")


def band_status(value, lo, hi) -> str:
    if value is None or lo is None or hi is None:
        return "n/a"
    return "ok" if lo <= value <= hi else "warn"


# ------------------------------------------------------------------------------------------------ inputs
def read_bytes(path, what) -> tuple[bytes, str]:
    p = Path(path)
    require(p.is_file(), f"{what}: missing {p}")
    b = p.read_bytes()
    return b, hashlib.sha256(b).hexdigest()


def read_csv(path, what) -> tuple[list[dict], str]:
    b, s = read_bytes(path, what)
    return list(csv.DictReader(io.StringIO(b.decode("utf-8")))), s


def fnum(v) -> float:
    try:
        return float(v)
    except (TypeError, ValueError):
        return math.nan


class Inputs:
    def __init__(self, args):
        self.files: dict[str, str] = {}
        self.decision = None
        hd = args.holdings_days
        if args.decide:
            b, s = read_bytes(Path(args.decide) / "decision.json", "decide")
            self.files[str(Path(args.decide) / "decision.json")] = s
            self.decision = json.loads(b)
            require(self.decision.get("schema") == "atx.book-decision/v1", "decide: decision.json schema")
            if hd is None and (Path(args.decide) / "holdings_days.csv").is_file():
                hd = Path(args.decide) / "holdings_days.csv"
        self.days = None
        if hd:
            rows, s = read_csv(hd, "holdings days")
            self.files[str(hd)] = s
            self.days = rows
        self.bias = None
        if args.bias:
            rows, s = read_csv(args.bias, "bias")
            self.files[str(args.bias)] = s
            self.bias = [r for r in rows if r.get("family") == "book"]
        self.admission = None
        if args.admission:
            b, s = read_bytes(args.admission, "admission")
            self.files[str(args.admission)] = s
            self.admission = json.loads(b)
            require(self.admission.get("schema") == fcw.ADMISSION_SCHEMA, "admission: schema")
        self.members = list(self.admission.get("admitted", [])) if self.admission else []
        self.sign = {r["id"]: (r.get("s_k") or 1) for r in (self.admission or {}).get("candidates", [])}
        self.ic = None
        if args.daily_ic:
            b, s = read_bytes(args.daily_ic, "daily IC")
            self.files[str(args.daily_ic)] = s
            self.ic = {}
            for r in csv.DictReader(io.StringIO(b.decode("utf-8"))):
                if int(r["horizon"]) == IC_HORIZON and r["id"] in self.sign:
                    self.ic.setdefault(r["id"], []).append((int(r["session_ns"]), fnum(r["rank_ic"])))
            for k in self.ic:
                self.ic[k].sort()
        self.turn = None
        if args.sleeve_daily:
            rows, s = read_csv(args.sleeve_daily, "sleeve daily")
            self.files[str(args.sleeve_daily)] = s
            self.turn = {}
            for r in rows:
                self.turn.setdefault(r["id"], []).append((int(r["session_ns"]), fnum(r["turnover"])))
            for k in self.turn:
                self.turn[k].sort()
        self.resid, self.resid_note = None, None
        if args.fit_work:
            self.resid, self.resid_note = fit_records(Path(args.fit_work), self.admission, self.members)


def fit_records(work: Path, admission: dict | None, members: list[str]) -> tuple[dict | None, str | None]:
    """{member: f_unsigned array} from the fitter WorkStore records (any key layout), matched by candidate id,
    cache payload SHA and TRAIN role; content SHA verified. The v8 store (fcw.stored_factor_series: records keyed by
    payload SHA, matched to the admission's context digest) is read first."""
    if admission is None:
        return None, "--fit-work needs --admission"
    role = admission.get("inputs", {}).get("train_manifest_sha256")
    want = {r["id"]: r.get("cache_payload_sha256") for r in admission.get("candidates", []) if r["id"] in members}
    found: dict[str, np.ndarray] = fcw.stored_factor_series(work, str(role), want,
                                                            admission.get("inputs", {}).get("context_sha256"))
    base = work / str(role)
    if not found and not base.is_dir():
        return None, f"no WorkStore for role {role} under {work}"
    for path in sorted(base.glob("*/factors/*.json")):
        try:
            j = json.loads(path.read_bytes())
        except (OSError, ValueError):
            continue
        body = {k: v for k, v in j.items() if k != "content_sha256"}
        if (j.get("schema") != fcw.FACTOR_SCHEMA or j.get("role_manifest_sha256") != role or
                j.get("content_sha256") != hashlib.sha256(fcw.canonical_compact(body)).hexdigest()):
            continue
        for cid, payload in want.items():
            if cid not in found and j.get("candidate_id") == cid and j.get("cache_payload_sha256") == payload:
                found[cid] = np.array([math.nan if v is None else v for v in j["f_unsigned"]], dtype=float)
    if len(found) < 2:
        return None, f"fewer than two admitted members' records in {base}"
    lengths = {len(v) for v in found.values()}
    if len(lengths) != 1:
        return None, "member records cover different decision windows"
    return found, (None if len(found) == len(want) else f"records for {len(found)} of {len(want)} members")


# ------------------------------------------------------------------------------------------------ checks
def m1(inp: Inputs, ref: dict | None) -> dict:
    out: dict = {"window": BIAS_WINDOW, "band": [1 - math.sqrt(2 / BIAS_WINDOW), 1 + math.sqrt(2 / BIAS_WINDOW)]}
    if inp.days:
        nav_r = []
        prev = None
        for r in inp.days:
            pre, post = fnum(r["pretrade_nav"]), fnum(r["posttrade_nav"])
            if prev is not None and prev > 0:
                nav_r.append(pre / prev - 1.0)
            prev = post
        x = np.array(nav_r[-BIAS_WINDOW:])
        out["realized_nav_vol_annual"] = float(x.std(ddof=1) * math.sqrt(252)) if x.size >= 2 else None
    if not inp.bias:
        out.update(na("bias.csv has no family=book rows: rerun the risk verb with --book-weights <holdings.csv> "
                      "--book-weights-sha256 <sha> (holdings.csv from nav --emit-holdings)" if inp.bias is not None
                      else "no --bias input"))
        return out
    rows = sorted((int(r["session"]), fnum(r["z"]), fnum(r["forecast_vol"]), fnum(r["realized_return"]))
                  for r in inp.bias)
    sessions = np.array([r[0] for r in rows], dtype=np.int64)
    z = np.array([r[1] for r in rows])
    fz = z[np.isfinite(z)]
    m2 = float(((fz - fz.mean()) ** 2).mean()) if fz.size else math.nan
    kurt = float(((fz - fz.mean()) ** 4).mean() / (m2 * m2)) if fz.size and m2 > 0 else None
    b63 = np.array([np.nanstd(z[max(0, j - BIAS_WINDOW + 1):j + 1], ddof=1) if j >= BIAS_WINDOW - 1 else np.nan
                    for j in range(len(z))])
    k = (ref or {}).get("reference", {}).get("M1", {}).get("pooled_kurtosis", kurt) if ref else kurt
    half = 1.96 * math.sqrt(max(k - 1.0, 0.0) / (4 * BIAS_WINDOW)) if k is not None else None
    half = max(half, math.sqrt(2 / BIAS_WINDOW)) if half is not None else None  # widening never narrows the band
    kband = [1 - half, 1 + half] if half is not None else None
    last = z[-BIAS_WINDOW:]
    b = float(np.nanstd(last, ddof=1)) if np.isfinite(last).sum() >= 2 and len(last) == BIAS_WINDOW else None
    fv = np.array([r[2] for r in rows])[-BIAS_WINDOW:]
    rr = np.array([r[3] for r in rows])[-BIAS_WINDOW:]
    ratio = (float(np.nanstd(rr, ddof=1) / np.nanmean(fv)) if len(rr) >= 2 and np.nanmean(fv) > 0 else None)
    if b is None:
        status = "n/a"
    elif out["band"][0] <= b <= out["band"][1]:
        status = "ok"
    elif kband and kband[0] <= b <= kband[1]:
        status = "warn"
    else:
        status = "alarm"
    out.update(status=status, b63=b, kurtosis_band=kband, pooled_kurtosis=k, realized_over_forecast=ratio,
               last_session_ns=int(sessions[-1]),
               reference={"pooled_kurtosis": kurt, "b63_p5": pct(b63, 5), "b63_p50": pct(b63, 50),
                          "b63_p95": pct(b63, 95), "sessions": int(len(z))})
    return out


def weekly(series: list[tuple[int, float]]) -> tuple[np.ndarray, np.ndarray]:
    """Every CUSUM_STRIDE-th observation of an IC series (non-overlapping 5-day labels), finite only."""
    s = np.array([x[0] for x in series], dtype=np.int64)[::CUSUM_STRIDE]
    v = np.array([x[1] for x in series], dtype=float)[::CUSUM_STRIDE]
    ok = np.isfinite(v)
    return s[ok], v[ok]


def blocks(series: list[tuple[int, float]]) -> tuple[np.ndarray, np.ndarray]:
    """Means of consecutive CUSUM_STRIDE-observation blocks (a block's session = its last)."""
    s = np.array([x[0] for x in series], dtype=np.int64)
    v = np.array([x[1] for x in series], dtype=float)
    n = len(v) // CUSUM_STRIDE * CUSUM_STRIDE
    s, v = s[:n].reshape(-1, CUSUM_STRIDE), v[:n].reshape(-1, CUSUM_STRIDE)
    f = np.isfinite(v)
    with np.errstate(invalid="ignore"):
        m = np.where(f.sum(axis=1) > 0, np.where(f, v, 0.0).sum(axis=1) / np.maximum(f.sum(axis=1), 1), np.nan)
    ok = np.isfinite(m)
    return s[:, -1][ok], m[ok]


def m2(inp: Inputs, ref: dict | None) -> dict:
    out: dict = {"cusum": {"h": CUSUM_H, "k": CUSUM_K, "warn": CUSUM_WARN, "units": "sigma", "stride": CUSUM_STRIDE,
                           "arl0_observations": siegmund_arl(0.0), "arl0_years": siegmund_arl(0.0) * CUSUM_STRIDE / 252},
                 "ic_horizon": IC_HORIZON, "members": {}}
    if not inp.members:
        out.update(na("no --admission (admitted members)"))
        return out
    if inp.ic is None and inp.turn is None:
        out.update(na("no --daily-ic and no --sleeve-daily"))
        return out
    rref = (ref or {}).get("reference", {}).get("M2", {}).get("members", {})
    last_ref = (ref or {}).get("reference", {}).get("last_session_ns")
    for cid in inp.members:
        row: dict = {"sign": inp.sign.get(cid, 1)}
        mref = rref.get(cid) if ref else None
        if ref and not mref:
            out["members"][cid] = na("member not in the reference (admitted after the baseline)", sign=row["sign"])
            continue
        mine: dict = {}
        if inp.ic is not None and cid in inp.ic:
            ser = [(t, inp.sign.get(cid, 1) * v) for t, v in inp.ic[cid]]
            vals = np.array([v for _, v in ser])
            ws, wv = weekly(ser)
            mu0 = mref.get("ic_weekly_mean") if mref else (float(wv.mean()) if wv.size else None)
            sd0 = mref.get("ic_weekly_sd") if mref else (float(wv.std(ddof=1)) if wv.size >= 2 else None)
            mine.update(ic_weekly_mean=float(wv.mean()) if wv.size else None,
                        ic_weekly_sd=float(wv.std(ddof=1)) if wv.size >= 2 else None, ic_weekly_n=int(wv.size))
            for w in IC_WINDOWS:
                roll = rolling_mean(vals, w)
                mine[f"ic{w}_p5"], mine[f"ic{w}_p95"] = pct(roll, 5), pct(roll, 95)
                cur, n = last_window_mean(vals, w)
                lo = (mref or mine).get(f"ic{w}_p5")
                hi = (mref or mine).get(f"ic{w}_p95")
                row[f"ic{w}"] = {"value": cur, "n": n, "p5": lo, "p95": hi, "status": band_status(cur, lo, hi)}
            live = wv if not ref else wv[ws > (last_ref or 0)]
            if mu0 is not None and sd0 and live.size:
                path = cusum((live - mu0) / sd0, "low")
                row["ic_cusum"] = {"value": float(path[-1]), "max": float(path.max()), "observations": int(live.size),
                                   "mu0": mu0, "sd0": sd0, "status": cusum_status(float(path[-1])),
                                   "arl_to_zero_ic_years": siegmund_arl(mu0 / sd0) * CUSUM_STRIDE / 252}
            else:
                row["ic_cusum"] = na("no live weekly IC observations" if ref else "no weekly IC observations")
        else:
            row["ic63"] = row["ic252"] = row["ic_cusum"] = na("no daily IC series for this member")
        if inp.turn is not None and cid in inp.turn:
            ser = inp.turn[cid]
            vals = np.array([v for _, v in ser])
            bs, bv = blocks(ser)
            mine.update(turnover_block_mean=float(bv.mean()) if bv.size else None,
                        turnover_block_sd=float(bv.std(ddof=1)) if bv.size >= 2 else None)
            roll = rolling_mean(vals, TURN_WINDOW)
            mine["t63_p5"], mine["t63_p95"] = pct(roll, 5), pct(roll, 95)
            cur, n = last_window_mean(vals, TURN_WINDOW)
            lo, hi = (mref or mine).get("t63_p5"), (mref or mine).get("t63_p95")
            row["turnover63"] = {"value": cur, "n": n, "p5": lo, "p95": hi, "status": band_status(cur, lo, hi)}
            mu, sd = (mref or mine).get("turnover_block_mean"), (mref or mine).get("turnover_block_sd")
            live = bv if not ref else bv[bs > (last_ref or 0)]
            if mu is not None and sd and live.size:
                z = (live - mu) / sd
                hi_p, lo_p = cusum(z, "high"), cusum(z, "low")
                v = max(float(hi_p[-1]), float(lo_p[-1]))
                row["turnover_cusum"] = {"value": v, "high": float(hi_p[-1]), "low": float(lo_p[-1]),
                                         "observations": int(live.size), "mu0": mu, "sd0": sd,
                                         "status": cusum_status(v)}
            else:
                row["turnover_cusum"] = na("no live turnover blocks" if ref else "no turnover blocks")
        else:
            row["turnover63"] = row["turnover_cusum"] = na("no --sleeve-daily turnover for this member")
        row["status"] = worst(v.get("status") for v in row.values() if isinstance(v, dict))
        if not ref:
            row["reference"] = mine
        out["members"][cid] = row
    statuses = [r["status"] for r in out["members"].values()]
    out["status"] = worst(statuses)
    out["counts"] = {s: statuses.count(s) for s in ("ok", "warn", "alarm", "n/a")}
    return out


def m3(inp: Inputs, ref: dict | None) -> dict:
    out: dict = {"window": COST_WINDOW, "cost_ratio_band": list(COST_BAND),
                 "opportunity_cost": "n/a: unfilled orders' foregone return needs per-name decision prices"}
    if not inp.days:
        out.update(na("no --holdings-days (or decide dir with holdings_days.csv)"))
        return out
    ex = [r for r in inp.days if r.get("executed") == "1" and fnum(r["traded_dollars"]) > 0]
    if len(ex) < COST_WINDOW:
        out.update(na(f"fewer than {COST_WINDOW} executed sessions"))
        return out
    sess = np.array([int(r["session_ns"]) for r in ex], dtype=np.int64)
    cost = np.array([fnum(r["pretrade_nav"]) - fnum(r["posttrade_nav"]) for r in ex])
    traded = np.array([fnum(r["traded_dollars"]) for r in ex])
    unfilled = np.array([fnum(r["unfilled_dollars"]) for r in ex])
    nav = np.array([fnum(r["pretrade_nav"]) for r in ex])
    fills = np.array([fnum(r["fills"]) for r in ex])
    capped = np.array([fnum(r["capped_fills"]) for r in ex])
    model = (ref or {}).get("reference", {}).get("M3", {}).get("model_cost_per_dollar") if ref else None
    own = float(cost.sum() / traded.sum())
    model = own if model is None else model
    w = COST_WINDOW
    cs_c, cs_t, cs_u = (np.concatenate([[0.0], np.cumsum(a)]) for a in (cost, traded, unfilled))
    roll_ratio = ((cs_c[w:] - cs_c[:-w]) / (cs_t[w:] - cs_t[:-w])) / model
    roll_fill = (cs_t[w:] - cs_t[:-w]) / ((cs_t[w:] - cs_t[:-w]) + (cs_u[w:] - cs_u[:-w]))
    rref = (ref or {}).get("reference", {}).get("M3", {}) if ref else {}
    lo, hi = rref.get("ratio_p5", pct(roll_ratio, 5)), rref.get("ratio_p95", pct(roll_ratio, 95))
    ratio = float(roll_ratio[-1])
    s_ratio = "alarm" if not COST_BAND[0] <= ratio <= COST_BAND[1] else ("warn" if band_status(ratio, lo, hi) ==
                                                                       "warn" else "ok")
    p1, p5 = rref.get("fill_p1", pct(roll_fill, 1)), rref.get("fill_p5", pct(roll_fill, 5))
    fill = float(roll_fill[-1])
    s_fill = "alarm" if p1 is not None and fill < p1 else ("warn" if p5 is not None and fill < p5 else "ok")
    out.update(status=worst([s_ratio, s_fill]), last_session_ns=int(sess[-1]),
               cost_ratio={"value": ratio, "p5": lo, "p95": hi, "status": s_ratio,
                           "cost_per_dollar_bps": 1e4 * float(cost[-w:].sum() / traded[-w:].sum()),
                           "model_cost_per_dollar_bps": 1e4 * model},
               fill_rate={"value": fill, "p1": p1, "p5": p5, "status": s_fill},
               info={"capped_fill_share": float(capped[-w:].sum() / max(fills[-w:].sum(), 1.0)),
                     "execution_cost_bps_of_nav_per_day": 1e4 * float((cost[-w:] / nav[-w:]).mean())},
               reference={"model_cost_per_dollar": own, "ratio_p5": pct(roll_ratio, 5),
                          "ratio_p95": pct(roll_ratio, 95), "fill_p1": pct(roll_fill, 1),
                          "fill_p5": pct(roll_fill, 5), "executed_sessions": int(len(ex))})
    return out


def m4(inp: Inputs, ref: dict | None) -> dict:
    out: dict = {"window": CROWD_WINDOW,
                 "comomentum_stock_level": "n/a: needs per-name residual returns of the book's names (no holdings.csv "
                                           "or specific returns among the inputs)"}
    if inp.resid is None:
        out.update(na(inp.resid_note or "no --fit-work (neutralized member returns)"))
        return out
    ids = sorted(inp.resid)
    x = np.column_stack([inp.resid[i] for i in ids])
    t = x.shape[0]
    roll = np.full(t, np.nan)
    for e in range(CROWD_WINDOW, t + 1):
        roll[e - 1] = mean_pairwise_corr(x[e - CROWD_WINDOW:e])
    cur = float(roll[-1]) if math.isfinite(roll[-1]) else None
    rref = (ref or {}).get("reference", {}).get("M4", {}) if ref else {}
    p95, p99 = rref.get("p95", pct(roll, 95)), rref.get("p99", pct(roll, 99))
    status = ("n/a" if cur is None else "alarm" if p99 is not None and cur > p99 else
              "warn" if p95 is not None and cur > p95 else "ok")
    out.update(status=status, mean_pairwise_corr=cur, p95=p95, p99=p99, members=ids, decisions=t,
               note=inp.resid_note, reference={"p50": pct(roll, 50), "p95": pct(roll, 95), "p99": pct(roll, 99)})
    return out


def mean_pairwise_corr(x: np.ndarray) -> float:
    ok = np.all(np.isfinite(x), axis=1)
    y = x[ok]
    if y.shape[0] < 10:
        return math.nan
    sd = y.std(axis=0)
    y = y[:, sd > 0]
    if y.shape[1] < 2:
        return math.nan
    c = np.corrcoef(y, rowvar=False)
    k = c.shape[0]
    return float((c.sum() - k) / (k * (k - 1)))


def decision_block(inp: Inputs) -> dict:
    if inp.decision is None:
        return na("no --decide")
    h = inp.decision.get("health", {})
    status = {"ok": "ok", "warn": "warn", "error": "alarm"}.get(h.get("status"), "n/a")
    return {"status": status, "asof": inp.decision.get("asof"), "health_status": h.get("status"),
            "checks": {c.get("check"): c.get("status") for c in h.get("checks", [])},
            "transfer_coefficient": inp.decision.get("transfer_coefficient", {}).get("target")}


# ------------------------------------------------------------------------------------------------ run
def run(args) -> tuple[dict, bytes]:
    ref = None
    if args.reference:
        b, s = read_bytes(args.reference, "reference")
        ref = json.loads(b)
        require(ref.get("schema") == SCHEMA and ref.get("mode") == "baseline", "reference: not a baseline monitor.json")
        ref["_sha256"] = s
    inp = Inputs(args)
    checks = {"M1": m1(inp, ref), "M2": m2(inp, ref), "M3": m3(inp, ref), "M4": m4(inp, ref),
              "decision": decision_block(inp)}
    last = [c.get("last_session_ns") for c in (checks["M1"], checks["M3"]) if c.get("last_session_ns")]
    for series in ((inp.ic or {}).values(), (inp.turn or {}).values()):
        for ser in series:
            if ser:
                last.append(ser[-1][0])
    doc = {"schema": SCHEMA, "mode": "baseline" if args.baseline else "live",
           "action": "none: flags only (retire / reweight are owner decisions, literature-v7 S7)",
           "status": worst(c.get("status") for c in checks.values()), "checks": checks,
           "inputs": {"files": dict(sorted(inp.files.items())),
                      "reference_sha256": ref["_sha256"] if ref else None}}
    if args.baseline:
        doc["reference"] = {"last_session_ns": max(last) if last else None,
                            "M1": checks["M1"].get("reference", {}),
                            "M2": {"members": {k: v["reference"] for k, v in checks["M2"].get("members", {}).items()}},
                            "M3": checks["M3"].get("reference", {}), "M4": checks["M4"].get("reference", {})}
        doc["in_sample"] = True
    else:
        doc["reference_last_session_ns"] = ref.get("reference", {}).get("last_session_ns")
        for key in ("M1", "M3", "M4"):  # live: the reference distributions come from --reference only
            checks[key].pop("reference", None)
    return doc, canonical(doc)


def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    mode = p.add_mutually_exclusive_group(required=True)
    mode.add_argument("--baseline", action="store_true", help="write the TRAIN reference distributions")
    mode.add_argument("--reference", type=Path, help="a baseline monitor.json to compare against (live mode)")
    p.add_argument("--decide", type=Path)
    p.add_argument("--holdings-days", type=Path)
    p.add_argument("--bias", type=Path)
    p.add_argument("--daily-ic", type=Path)
    p.add_argument("--admission", type=Path)
    p.add_argument("--sleeve-daily", type=Path)
    p.add_argument("--fit-work", type=Path)
    p.add_argument("--output", type=Path, required=True, help="new output directory (never overwritten)")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    try:
        require(not Path(args.output).exists(), f"output exists; refusing overwrite: {args.output}")
        doc, data = run(args)
        fcw.publish_directory(args.output, {"monitor.json": data})
    except (MonitorError, fcw.FitError, ValueError, KeyError) as exc:
        print(f"book_monitor: {exc}", file=sys.stderr)
        return EXIT_REFUSED
    summary = {"output": str(args.output), "mode": doc["mode"], "status": doc["status"],
               "checks": {k: v.get("status") for k, v in doc["checks"].items()}}
    print(json.dumps(summary, sort_keys=True))
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
