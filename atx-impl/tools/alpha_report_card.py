#!/usr/bin/env python3
"""Per-alpha report card over a TRAIN u pass (platform v7 W3; literature-v7 R5.6, Assaying-Anomalies style).

  alpha_report_card.py --u-pass U --train ROLE/manifest.json --train-sha256 SHA --admission ADM.json
                       [--admission-sha256 SHA] [--ids a,b] --output OUT

Run from the root the u pass ran in (the runner summary's cache and fields paths are relative to it). Reads the
u pass (summary.json, orientations.json, recipe.json, train_daily_ic.csv, train_candidates.jsonl), every
candidate's cached raw signal (the summary's candidate_cache entries, validated and SHA-verified by
fit_composition_weights.CacheLayout), the pinned TRAIN role (fit_composition_weights.RoleManifest: TRAIN-only, nothing
on or after the TRAIN end of research_window.py; a role reaching it raises TrainWindowError, a ValueError naming the
window id and the seal), the size and FF12 fields of the pinned research fields manifest, and the
admission table. Writes a new directory (published atomically, never overwritten): card-<id>.json and
card-<id>.html per candidate, index.json / index.html (candidates ranked by WorldQuant fitness), daily_sleeve.csv
(per-candidate daily turnover / PnL / coverage, the monitor's TRAIN reference) and manifest.json (inputs and file
SHAs). Output is deterministic: sorted keys, floats rounded to 10 significant digits, no timestamps.

Definitions (the runner's are those of strategy_ic_runner.cpp + atx-engine factory/ic_screen.cpp):
  rows          decision d in [score_begin, score_end); eligible names = present & member at d (the runner's
                decision membership); a row's width is its eligible count.
  label(h)      cumulative: close[d+1+h]/close[d+1]-1 (the runner's labels, execution delay 1); lagged one-day:
                close[d+h+1]/close[d+h]-1 (entry shifted by h-1, h = 1 equals cumulative h = 1). Either needs both
                endpoints present with finite positive close, a finite positive decision close and no guarded
                return between the endpoints (guard_for: |log close step| > 1.5 or > |log raw step| + .10).
  rank IC       per row, Pearson over the paired names (eligible, finite signal and label) of the signal's
                tie-averaged ranks over its finite eligible names and the label's over its finite eligible names
                (marginal ranks). The runner re-ranks both on the paired subset; the two agree when the paired set
                equals both marginal sets and otherwise differ by O(1e-4) per day. A row is kept under the runner's
                rules: paired >= min_names, paired >= .8 labels, paired >= .8 eligible, labels >= min_names and
                labels >= .8 eligible. runner_check compares the cumulative h = 5/21/63 series to the runner's own.
  IC / IR by year  the runner's own daily rank IC (train_daily_ic.csv, as written = DSL orientation) per calendar
                year of the decision session: n, mean, sd (ddof 1), IR = mean / sd (daily, not annualized).
  decay         mean lagged one-day rank IC for h = 1..63; half-life = ln 2 / -ln rho of the least-squares fit
                m(h) = a rho^(h-1) (rho on a log-spaced half-life grid 0.25..1000 days, 2001 points; a closed form).
  size / FF12   rank IC at the runner's orientation horizon (cumulative h = 21) within each group of the row:
                size terciles of me_company (issuer market equity; tie-averaged ranks, tercile floor(3 r / n)),
                or ADV63 dollar volume when the fields lack it; FF12 = grp_ff12. Within-group marginal ranks, the
                runner's coverage rules with min_names -> 30.
  book          q(d) = centered signal ranks over the row, scaled to sum |q| = 1 (dollar neutral, gross 1, not
                neutralized; the fitter's tau is price-risk-v1 neutralized); pnl(d) = sum q r1 with r1 = the
                cumulative h = 1 label (0 where invalid); a flat row is NaN.
  turnover      mean over consecutive rows of sum_i |q(d)_i - q(d-1)_i| (instruments aligned by id).
  WorldQuant    Sharpe = sqrt(252) mean(pnl) / sd(pnl); returns = 252 mean(pnl) / (booksize / 2) with booksize 1;
                fitness = Sharpe sqrt(|returns| / max(turnover, .125)); margin = sum pnl / sum traded, in bps.
                Gross of costs.
  coverage      finite-signal eligible names / eligible names per row, averaged per year.
  correlation   signal: mean over rows of the cross-sectional Spearman (marginal ranks, >= 50 common names) with
                every admitted member and each theme composite (mean of the members' rank / n, missing = 0);
                pnl: Pearson of the daily book PnL over rows both finite (>= 250) with every admitted member and
                each theme composite (mean of the members' PnL). max |rho| over the admitted members but itself.
Numpy only (mega_report components for the pages); BLAS pinned to one thread.
"""
from __future__ import annotations

import os

for _var in ("OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS", "OMP_NUM_THREADS"):
    os.environ.setdefault(_var, "1")

import argparse  # noqa: E402
import concurrent.futures  # noqa: E402
import csv  # noqa: E402
import hashlib  # noqa: E402
import io  # noqa: E402
import json  # noqa: E402
import math  # noqa: E402
from pathlib import Path  # noqa: E402
import re  # noqa: E402
import sys  # noqa: E402
import time  # noqa: E402

import numpy as np  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fit_composition_weights as fcw  # noqa: E402
from mega_report import analysis as A  # noqa: E402
from mega_report import components as C  # noqa: E402
from mega_report import theme as TH  # noqa: E402

SCHEMA = "atx.alpha-report-card/v1"
INDEX_SCHEMA = "atx.alpha-report-card-index/v1"
RUNNER_HORIZONS = (5, 21, 63)
ORIENTATION_H = 21
DECAY_H = tuple(range(1, 64))
BLOCK = 1 + max(DECAY_H)          # sessions d+1 .. d+64 cover every label endpoint
MIN_COVERAGE = 0.8                # ic_screen.cpp kMinCoverage
GROUP_MIN_NAMES = 30
SIGNAL_CORR_MIN_NAMES = 50
PNL_CORR_MIN_DAYS = fcw.MIN_COMMON_DAYS
ANNUAL = 252
WQ_TURNOVER_FLOOR = 0.125
GUARD_ABS_LOG, GUARD_EXCESS_LOG = 1.5, 0.10
HALF_LIFE_GRID = np.logspace(math.log10(0.25), math.log10(1000.0), 2001)
SIG_DIGITS = 10
ID_RE = re.compile(r"[A-Za-z0-9_.-]{1,96}")
EXIT_OK, EXIT_REFUSED = 0, 1
LEGACY_THEME_KEYS = ("theme", "family")


class CardError(Exception):
    """A loud refusal: bad pin, missing input or contract violation."""


def require(cond, msg: str) -> None:
    if not cond:
        raise CardError(msg)


def sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


# ------------------------------------------------------------------------------------------------ determinism
def q(x):
    """A JSON-ready value: floats rounded to SIG_DIGITS significant digits, NaN/inf -> None, numpy -> python."""
    if isinstance(x, dict):
        return {str(k): q(v) for k, v in x.items()}
    if isinstance(x, (list, tuple)):
        return [q(v) for v in x]
    if isinstance(x, (np.integer,)):
        return int(x)
    if isinstance(x, (bool, np.bool_)):
        return bool(x)
    if isinstance(x, (float, np.floating)):
        x = float(x)
        return float(f"{x:.{SIG_DIGITS}g}") if math.isfinite(x) else None
    return x


def canonical(doc) -> bytes:
    return (json.dumps(q(doc), sort_keys=True, indent=1, allow_nan=False) + "\n").encode("utf-8")


def ffmt(x) -> str:
    return "" if x is None or not math.isfinite(x) else f"{x:.{SIG_DIGITS}g}"


# ------------------------------------------------------------------------------------------------ ranks
class Ranked:
    """One ``rank_core`` pass: the (row, group, value) sort of a row-major panel, flattened.

    ``full`` = per-row sorted column order; ``groups`` = the sorted group id of every flat position (``n_groups`` =
    left out); ``g_starts`` = flat start of every (row, group) segment; ``centered`` = the tie-averaged rank within
    the segment minus the segment centre (rank - (n - 1) / 2) at every flat sorted position."""

    __slots__ = ("full", "groups", "g_starts", "centered", "kept", "n_groups", "shape")


def rank_core(values: np.ndarray, valid: np.ndarray, groups: np.ndarray | None = None, n_groups: int = 1,
              order: np.ndarray | None = None) -> tuple[Ranked, np.ndarray]:
    t, w = values.shape
    key = np.where(valid, values, np.inf)
    if order is None:
        order = np.argsort(key, axis=1)  # tie order is irrelevant: tie runs are averaged
    if groups is None:  # invalid keys are +inf: they already sort last, one group of the valid prefix
        full = order
        gs = (np.arange(w)[None, :] >= valid.sum(axis=1, keepdims=True)).astype(np.int16)
        n_groups = 1
    else:
        g_sorted = np.take_along_axis(np.where(valid & (groups >= 0), groups, n_groups), order, axis=1).astype(np.int16)
        o2 = np.argsort(g_sorted, axis=1, kind="stable")  # radix: keeps the value order inside a group
        full = np.take_along_axis(order, o2, axis=1)
        gs = np.take_along_axis(g_sorted, o2, axis=1)
    vs = np.take_along_axis(key, full, axis=1).ravel()
    gf = gs.ravel()
    n = t * w
    new_g = np.zeros(n, dtype=bool)
    new_g[::w] = True
    new_g[1:] |= gf[1:] != gf[:-1]
    new_t = new_g.copy()
    new_t[1:] |= vs[1:] != vs[:-1]
    t_starts, g_starts = np.flatnonzero(new_t), np.flatnonzero(new_g)
    t_mid = (t_starts + np.append(t_starts[1:], n) - 1) / 2.0
    g_mid = (g_starts + np.append(g_starts[1:], n) - 1) / 2.0
    res = Ranked()
    res.full, res.groups, res.g_starts, res.n_groups, res.shape = full, gf, g_starts, n_groups, (t, w)
    res.centered = t_mid[np.cumsum(new_t) - 1] - g_mid[np.cumsum(new_g) - 1]
    res.kept = gf < n_groups
    return res, order


def scatter_ranks(res: Ranked) -> np.ndarray:
    t, w = res.shape
    out = np.full(t * w, np.nan)
    out[(res.full + (np.arange(t) * w)[:, None]).ravel()] = np.where(res.kept, res.centered, np.nan)
    return out.reshape(t, w)


def centered_ranks(values: np.ndarray, valid: np.ndarray, groups: np.ndarray | None = None, n_groups: int = 1,
                   order: np.ndarray | None = None) -> tuple[np.ndarray, np.ndarray]:
    """Row-wise tie-averaged ranks within (row, group), centered: rank - (n_group - 1) / 2; NaN elsewhere.

    ``groups`` (int, -1 = none) splits each row; ``order`` reuses a previous call's value order of the same
    ``values``/``valid`` (the group split is a stable radix pass over it). Returns (ranks, value order)."""
    res, order = rank_core(values, valid, groups, n_groups, order)
    return scatter_ranks(res), order


def pearson_from_sums(n, sx, sy, sxx, syy, sxy):
    with np.errstate(invalid="ignore", divide="ignore"):
        nn = np.maximum(n, 1)
        cov = sxy - sx * sy / nn
        vx = sxx - sx * sx / nn
        vy = syy - sy * sy / nn
        flat = n * 64.0 * np.finfo(float).eps
        r = cov / np.sqrt(vx * vy)
        bad = (n < 3) | ~(vx > flat * np.maximum(sxx / nn, 1e-300)) | ~(vy > flat * np.maximum(syy / nn, 1e-300))
    return np.where(bad | ~np.isfinite(r), np.nan, np.clip(r, -1.0, 1.0))


def moments(x: np.ndarray) -> dict:
    x = np.asarray(x, dtype=float)
    x = x[np.isfinite(x)]
    n = int(x.size)
    mean = float(x.mean()) if n else None
    sd = float(x.std(ddof=1)) if n >= 2 else None
    return {"n": n, "mean": mean, "sd": sd, "ir": (mean / sd if sd else None)}


def years_of(sessions_ns: np.ndarray) -> np.ndarray:
    return np.asarray(sessions_ns, dtype="int64").astype("datetime64[ns]").astype("datetime64[Y]").astype(int) + 1970


def by_year(values: np.ndarray, years: np.ndarray, reducer=moments) -> dict:
    out = {str(y): reducer(values[years == y]) for y in sorted(set(int(v) for v in years))}
    out["all"] = reducer(values)
    return out


def half_life_fit(m: np.ndarray, hs: np.ndarray) -> dict | None:
    ok = np.isfinite(m)
    if int(ok.sum()) < 5:
        return None
    y, h = m[ok], hs[ok].astype(float)
    rhos = np.exp(-math.log(2.0) / HALF_LIFE_GRID)
    w = rhos[:, None] ** (h[None, :] - 1.0)
    a = (w @ y) / np.einsum("gh,gh->g", w, w)
    sse = ((y[None, :] - a[:, None] * w) ** 2).sum(axis=1)
    j = int(np.argmin(sse))
    tss = float(((y - y.mean()) ** 2).sum())
    hl = float(HALF_LIFE_GRID[j])
    return {"a": float(a[j]), "rho": float(rhos[j]), "half_life_days": hl, "sse": float(sse[j]),
            "r2": (1.0 - float(sse[j]) / tss) if tss > 0 else None, "beyond_window": hl > max(DECAY_H),
            "horizons_used": int(ok.sum())}


# ------------------------------------------------------------------------------------------------ inputs
def read_json_file(path: Path, what: str):
    require(Path(path).is_file(), f"{what}: missing {path}")
    try:
        return json.loads(Path(path).read_bytes().decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise CardError(f"{what}: JSON parse: {exc}") from exc


class Inputs:
    def __init__(self, args):
        u = Path(args.u_pass)
        self.u_dir = u
        self.files: dict[str, str] = {}
        summary_bytes = (u / "summary.json").read_bytes() if (u / "summary.json").is_file() else b""
        require(summary_bytes, f"u pass: no summary.json in {u}")
        summary_sha = sha256_bytes(summary_bytes)
        self.files[str(u / "summary.json")] = summary_sha
        summary = json.loads(summary_bytes)
        ob = (u / "orientations.json").read_bytes() if (u / "orientations.json").is_file() else b""
        require(ob, f"u pass: no orientations.json in {u}")
        o_sha = sha256_bytes(ob)
        self.files[str(u / "orientations.json")] = o_sha
        orient = json.loads(ob)
        self.role = fcw.RoleManifest(args.train, args.train_sha256)
        require(orient.get("train_manifest_sha256") in (None, self.role.sha),
                "orientations: train_manifest_sha256 differs from the pinned role")
        try:
            self.layout = fcw.CacheLayout(u / "summary.json", summary_sha, self.role, o_sha, orient.get("recipe_sha256"))
        except fcw.FitError as exc:
            raise CardError(f"u pass: {exc}") from exc
        self.library_sha = orient.get("library_sha256")
        rows = orient.get("candidates")
        require(isinstance(rows, list) and rows, "orientations: candidates")
        self.cands = [{"id": r["id"], "family": r.get("family"), "dsl_sha256": r["dsl_sha256"],
                       "runner_sign": r.get("sign")} for r in rows]
        for c in self.cands:
            require(isinstance(c["id"], str) and ID_RE.fullmatch(c["id"]), f"candidate id not file-safe: {c['id']!r}")
        if args.ids:
            want = [x for x in args.ids.split(",") if x]
            known = {c["id"] for c in self.cands}
            require(all(w in known for w in want), f"--ids: unknown {sorted(set(want) - known)}")
            self.cands = [c for c in self.cands if c["id"] in set(want)]
        recipe = read_json_file(u / "recipe.json", "u pass recipe") if (u / "recipe.json").is_file() else {}
        self.min_names = int(recipe.get("min_names", 1000))
        self.min_dates = int(recipe.get("min_dates", 128))
        text = (u / "train_daily_ic.csv").read_bytes()
        self.files[str(u / "train_daily_ic.csv")] = sha256_bytes(text)
        self.daily_ic = A.load_daily_ic(text.decode("utf-8"), "rank_ic")
        cj = (u / "train_candidates.jsonl").read_bytes() if (u / "train_candidates.jsonl").is_file() else b""
        self.runner = A.load_candidates(cj.decode("utf-8")) if cj else {}
        self.frozen_sign = {}
        for line in cj.decode("utf-8").splitlines():
            if line.strip():
                o = json.loads(line)
                if "frozen_train_sign" in o:
                    self.frozen_sign[o["id"]] = o["frozen_train_sign"]
        # research fields: size (me_company) and FF12 (grp_ff12), pinned by the runner summary
        self.size, self.ff12, self.fields_note = None, None, {}
        rf = summary["roles"][0].get("research_fields") or {}
        fsha = rf.get("manifest_sha256")
        if isinstance(fsha, dict):
            fsha = fsha.get("train")
        if rf.get("directory") and fcw.is_hash(fsha):
            man_path = Path(rf["directory"]) / "manifest.json"
            mb = man_path.read_bytes() if man_path.is_file() else b""
            require(mb and sha256_bytes(mb) == fsha, f"research fields: manifest {man_path} missing or its SHA differs")
            self.files[str(man_path)] = fsha
            man = json.loads(mb)
            require(man.get("role", {}).get("manifest_sha256") == self.role.sha, "research fields: another role")
            for name in ("me_company", "grp_ff12"):
                rec = man.get("files", {}).get(f"{name}.f64")
                if rec is None:
                    self.fields_note[name] = "absent from the fields manifest"
                    continue
                data = (man_path.parent / f"{name}.f64").read_bytes()
                size = self.role.dates * self.role.instruments * 8
                require(len(data) == size and int(rec["bytes"]) == size and sha256_bytes(data) == rec["sha256"],
                        f"research fields: {name}.f64 extent or SHA differs")
                self.files[str(man_path.parent / f"{name}.f64")] = rec["sha256"]
                arr = np.frombuffer(data, dtype="<f8").reshape(self.role.dates, self.role.instruments)
                setattr(self, "size" if name == "me_company" else "ff12", arr)
        else:
            self.fields_note["fields"] = "the u pass pinned no research fields"
        # admission table
        ab = Path(args.admission).read_bytes() if Path(args.admission).is_file() else b""
        require(ab, f"admission: missing {args.admission}")
        a_sha = sha256_bytes(ab)
        require(args.admission_sha256 is None or args.admission_sha256 == a_sha, "admission: SHA-256 pin differs")
        self.files[str(args.admission)] = a_sha
        self.admission = json.loads(ab)
        require(self.admission.get("schema") == fcw.ADMISSION_SCHEMA, "admission: schema")
        require(self.admission.get("inputs", {}).get("train_manifest_sha256") == self.role.sha,
                "admission: TRAIN manifest differs from the pinned role")
        self.adm_rows = {r["id"]: r for r in self.admission.get("candidates", [])}
        self.admitted = [i for i in self.admission.get("admitted", []) if i in {c["id"] for c in self.cands}]
        self.panel = self.role.payload()


# ------------------------------------------------------------------------------------------------ geometry
class Geometry:
    """Decision rows, compressed eligible columns, the runner's return guard and the h = 21 / h = 1 labels."""

    def __init__(self, role: fcw.RoleManifest, panel: dict):
        self.d_all, self.n_all = role.dates, role.instruments
        self.sb, self.se = role.score_begin, role.score_end
        self.t = self.se - self.sb
        self.sessions = role.sessions[self.sb:self.se].astype(np.int64)
        self.years = years_of(self.sessions)
        present = panel["present"].astype(bool)
        member = panel["member"].astype(bool)
        self.present = present
        self.close = panel["close"]
        elig = present[self.sb:self.se] & member[self.sb:self.se]
        self.width = elig.sum(axis=1).astype(np.int64)
        self.w = int(self.width.max()) if self.t else 0
        self.cols = np.full((self.t, self.w), -1, dtype=np.int64)
        for r in range(self.t):
            idx = np.flatnonzero(elig[r])
            self.cols[r, :idx.size] = idx
        self.colmask = self.cols >= 0
        self.safe_cols = np.where(self.colmask, self.cols, 0)
        self.cell_rows = np.nonzero(self.colmask)[0]
        self.cell_cols = self.cols[self.colmask]
        self.ever = np.unique(self.cell_cols)                      # instruments eligible on some row
        self.cell_pos = np.searchsorted(self.ever, self.cell_cols)
        self.rows = (self.sb + np.arange(self.t))[:, None]
        self.bad = guard_prefix(panel["close"], panel["raw_close"], present)
        dec = self.gather(self.close, 0)
        self.dec_ok = np.isfinite(dec) & (dec > 0) & self.colmask
        self.label21, self.label21_ok = self.label(ORIENTATION_H)
        self.r1, r1_ok = self.label(1)
        self.r1 = np.where(r1_ok, self.r1, 0.0)

    def gather(self, full: np.ndarray, offset: int, fill=np.nan) -> np.ndarray:
        rows = self.rows + offset
        inb = rows < self.d_all
        out = full[np.minimum(rows, self.d_all - 1), self.safe_cols]
        if out.dtype.kind == "f":
            return np.where(self.colmask & inb, out, fill)
        return np.where(self.colmask & inb, out, 0)

    def label(self, h: int) -> tuple[np.ndarray, np.ndarray]:
        """Cumulative runner label close[d+1+h]/close[d+1]-1 on the compressed grid, and its validity."""
        e_c, x_c = self.gather(self.close, 1), self.gather(self.close, 1 + h)
        e_p, x_p = (self.gather(self.present.view(np.uint8), 1).astype(bool),
                    self.gather(self.present.view(np.uint8), 1 + h).astype(bool))
        e_b, x_b = self.gather(self.bad, 1), self.gather(self.bad, 1 + h)
        inb = (self.rows + 1 + h) < self.d_all
        with np.errstate(invalid="ignore", divide="ignore"):
            ret = x_c / e_c - 1.0
            ok = (self.dec_ok & inb & e_p & x_p & (e_b == x_b) & np.isfinite(e_c) & np.isfinite(x_c) & (e_c > 0) &
                  (x_c > 0) & np.isfinite(ret))
        return np.where(ok, ret, np.nan), ok


def guard_prefix(close: np.ndarray, raw: np.ndarray, present: np.ndarray) -> np.ndarray:
    """strategy_ic_runner.cpp guard_for(): prefix counts of guarded interval returns per instrument."""
    d, n = close.shape
    bad = np.zeros((d, n), dtype=np.int32)
    with np.errstate(invalid="ignore", divide="ignore"):
        for a in range(1, d, 128):
            b = min(d, a + 128)
            c0, c1, r0, r1 = close[a - 1:b - 1], close[a:b], raw[a - 1:b - 1], raw[a:b]
            both = (present[a - 1:b - 1] & present[a:b] & np.isfinite(c0) & np.isfinite(c1) & (c0 > 0) & (c1 > 0))
            r = np.log(c1) - np.log(c0)
            flag = np.abs(r) > GUARD_ABS_LOG
            raw_ok = np.isfinite(r0) & np.isfinite(r1) & (r0 > 0) & (r1 > 0)
            flag |= raw_ok & (np.abs(r) > np.abs(np.log(r1) - np.log(r0)) + GUARD_EXCESS_LOG)
            bad[a:b] = both & flag
    return np.cumsum(bad, axis=0, dtype=np.int32)


# ------------------------------------------------------------------------------------------------ groups
def size_groups(geo: Geometry, inputs: Inputs, panel: dict) -> tuple[np.ndarray, str]:
    if inputs.size is not None:
        size, name = geo.gather(inputs.size, 0), "me_company"
    else:  # ADV63 dollar volume over (d-62 .. d], unusable days add 0
        dollars = np.where(geo.present & np.isfinite(panel["raw_close"]) & (panel["raw_close"] > 0) &
                           np.isfinite(panel["volume"]) & (panel["volume"] >= 0),
                           panel["raw_close"] * panel["volume"], 0.0)
        cs = np.vstack([np.zeros((1, geo.n_all)), np.cumsum(dollars, axis=0)])
        adv = np.full((geo.d_all, geo.n_all), np.nan)
        adv[62:] = (cs[63:] - cs[:-63]) / 63.0
        size, name = geo.gather(adv, 0), "adv63_dollar_volume (me_company unavailable)"
    ok = np.isfinite(size) & (size > 0) & geo.colmask
    ranks, _ = centered_ranks(size, ok)
    n = ok.sum(axis=1, keepdims=True)
    r0 = ranks + (n - 1) / 2.0
    with np.errstate(invalid="ignore"):
        terc = np.clip(np.floor(3.0 * r0 / np.maximum(n, 1)), 0, 2)
    return np.where(ok, terc, -1).astype(np.int16), name


def ff12_groups(geo: Geometry, inputs: Inputs) -> np.ndarray | None:
    if inputs.ff12 is None:
        return None
    v = geo.gather(inputs.ff12, 0)
    ok = np.isfinite(v) & (v >= 1) & (v <= 12) & (v == np.floor(v)) & geo.colmask
    return np.where(ok, v - 1, -1).astype(np.int16)


class GroupLabels:
    """Within-group label ranks at the orientation horizon for one grouping, and per (row, group) counts."""

    def __init__(self, geo: Geometry, groups: np.ndarray, n_groups: int, names: list[str]):
        self.groups, self.g, self.names = groups, n_groups, names
        self.ranks, _ = centered_ranks(geo.label21, geo.label21_ok, groups, n_groups)
        flat = np.arange(geo.t)[:, None] * n_groups + groups
        size = geo.t * n_groups
        member = groups >= 0
        self.eligible = np.bincount(flat[member], minlength=size).reshape(geo.t, n_groups)
        lab = member & np.isfinite(self.ranks)
        self.labels = np.bincount(flat[lab], minlength=size).reshape(geo.t, n_groups)
        self.flat = flat

    def ic(self, signal: np.ndarray, valid: np.ndarray, order: np.ndarray) -> dict:
        res, _ = rank_core(signal, valid, self.groups, self.g, order)
        t, w = res.shape
        y = self.ranks.ravel()[(res.full + (np.arange(t) * w)[:, None]).ravel()]  # label ranks, sorted order
        both = res.kept & np.isfinite(y)
        x0, y0 = np.where(both, res.centered, 0.0), np.where(both, y, 0.0)
        seg_row, seg_grp = res.g_starts // w, res.groups[res.g_starts]
        use = seg_grp < self.g
        sums = []
        for arr in (both.astype(float), x0, y0, x0 * x0, y0 * y0, x0 * y0):  # per (row, group) segment
            table = np.zeros(self.labels.shape)
            table[seg_row[use], seg_grp[use]] = np.add.reduceat(arr, res.g_starts)[use]
            sums.append(table)
        n = sums[0]
        r = pearson_from_sums(*sums)
        keep = ((n >= GROUP_MIN_NAMES) & (self.labels >= GROUP_MIN_NAMES) & (n >= MIN_COVERAGE * self.labels) &
                (n >= MIN_COVERAGE * self.eligible) & (self.labels >= MIN_COVERAGE * self.eligible))
        r = np.where(keep, r, np.nan)
        out = {}
        for g, name in enumerate(self.names):
            m = moments(r[:, g])
            used = np.isfinite(r[:, g])
            m["mean_names"] = float(n[used, g].mean()) if used.any() else None
            out[name] = m
        return out


# ------------------------------------------------------------------------------------------------ the cards
def row_decay(geo: Geometry, R: np.ndarray, min_names: int, log=None, workers: int = 1) -> tuple[np.ndarray, list]:
    """(candidates x rows x specs) daily rank IC for the lagged one-day h = 1..63 specs, then cumulative 5/21/63.

    Rows are independent: ``workers`` threads take disjoint row blocks (numpy sorts, gathers and the one-thread
    BLAS products release the GIL), so the result does not depend on the worker count."""
    specs = [("lag", h) for h in DECAY_H] + [("cum", h) for h in RUNNER_HORIZONS]
    entry = np.array([h - 1 if kind == "lag" else 0 for kind, h in specs])
    exit_ = np.array([h for kind, h in specs])
    k_n = R.shape[1]
    out = np.full((k_n, geo.t, len(specs)), np.nan)
    pres_u8 = geo.present.view(np.uint8)
    started = time.perf_counter()
    blocks = [range(a, min(geo.t, a + 16)) for a in range(0, geo.t, 16)]
    run_parallel(lambda rows: _decay_rows(geo, R, rows, specs, entry, exit_, pres_u8, min_names, out), blocks,
                 workers)
    if log:
        log(f"card: decay rows={geo.t} specs={len(specs)} seconds={time.perf_counter() - started:.1f}")
    return out, specs


def run_parallel(fn, items, workers: int) -> list:
    """``fn`` over ``items`` in order; results in item order whatever the worker count."""
    if workers <= 1 or len(items) <= 1:
        return [fn(x) for x in items]
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        return list(pool.map(fn, items))


def _decay_rows(geo, R, rows_range, specs, entry, exit_, pres_u8, min_names, out) -> None:
    k_n = R.shape[1]
    for r in rows_range:
        w = int(geo.width[r])
        cols = geo.cols[r, :w]
        d = geo.sb + r
        blk = d + 1 + np.arange(BLOCK)
        inb = blk < geo.d_all
        rows = np.minimum(blk, geo.d_all - 1)[:, None]
        c = geo.close[rows, cols[None, :]]
        p = (pres_u8[rows, cols[None, :]] == 1) & inb[:, None]
        b = geo.bad[rows, cols[None, :]]
        with np.errstate(invalid="ignore", divide="ignore"):
            end_ok = p & np.isfinite(c) & (c > 0)
            ret = c[exit_] / c[entry] - 1.0
            ok = (end_ok[entry] & end_ok[exit_] & (b[exit_] == b[entry]) & geo.dec_ok[r, :w][None, :] &
                  np.isfinite(ret))
        yr, _ = centered_ranks(ret, ok)                               # specs x names
        my = np.isfinite(yr)
        y0 = np.where(my, yr, 0.0).T                                  # names x specs
        myf = my.T.astype(float)
        x = R[r, :, :w].astype(np.float64)                            # candidates x names
        mx = np.isfinite(x)
        x0 = np.where(mx, x, 0.0)
        mxf = mx.astype(float)
        top = np.vstack([x0, mxf]) @ np.hstack([y0, myf])
        s = len(specs)
        sxy, sx, sy, n = top[:k_n, :s], top[:k_n, s:], top[k_n:, :s], top[k_n:, s:]
        sxx = (x0 * x0) @ myf
        syy = mxf @ (y0 * y0)
        rho = pearson_from_sums(n, sx, sy, sxx, syy, sxy)
        labels = my.sum(axis=1)[None, :]
        keep = ((n >= min_names) & (n >= MIN_COVERAGE * labels) & (n >= MIN_COVERAGE * w) & (labels >= min_names) &
                (labels >= MIN_COVERAGE * w))
        out[:, r, :] = np.where(keep, rho, np.nan)


def theme_of(row: dict | None, cand: dict) -> str:
    for key in LEGACY_THEME_KEYS:
        if row and isinstance(row.get(key), str):
            return row[key]
    return cand.get("family") or "unknown"


def build(args, log=None) -> tuple[dict[str, bytes], dict]:
    started = time.perf_counter()
    inputs = Inputs(args)
    geo = Geometry(inputs.role, inputs.panel)
    cands, k_n = inputs.cands, len(inputs.cands)
    ids = [c["id"] for c in cands]
    if log:
        log(f"card: inputs loaded candidates={k_n} rows={geo.t} width={geo.w} "
            f"seconds={time.perf_counter() - started:.1f}")
    terc, size_name = size_groups(geo, inputs, inputs.panel)
    size_labels = GroupLabels(geo, terc, 3, ["small", "mid", "large"])
    ff = ff12_groups(geo, inputs)
    ff_labels = GroupLabels(geo, ff, 12, [str(i) for i in range(1, 13)]) if ff is not None else None
    inputs.size = inputs.ff12 = inputs.panel = None  # the geometry keeps close, presence and the guard only
    R = np.full((geo.t, k_n, geo.w), np.nan, dtype=np.float32)
    per: dict[str, dict] = {}
    pnl = np.full((geo.t, k_n), np.nan)
    turnover = np.full((geo.t, k_n), np.nan)
    coverage = np.full((geo.t, k_n), np.nan)
    elig = geo.colmask.sum(axis=1)
    entries = []
    for cand in cands:  # every entry is validated before any payload is read
        try:
            entries.append(inputs.layout.resolve(cand, inputs.role))
        except fcw.FitError as exc:
            raise CardError(str(exc)) from exc

    def one(k: int) -> dict:
        tick = time.perf_counter()
        try:
            signal = fcw.load_candidate_signal(entries[k], inputs.role)
        except fcw.FitError as exc:
            raise CardError(str(exc)) from exc
        s = geo.gather(signal, 0)
        del signal
        valid = np.isfinite(s) & geo.colmask
        xr, order = centered_ranks(s, valid)
        R[:, k, :] = xr
        info = {"payload_sha256": entries[k]["payload_sha256"],
                "cache_entry": "base" if entries[k]["fields_manifest_sha256"] is None else "fields"}
        info["size"] = size_labels.ic(s, valid, order)
        info["ff12"] = ff_labels.ic(s, valid, order) if ff_labels is not None else None
        # book, pnl, turnover, coverage
        gross = np.abs(np.where(valid, xr, 0.0)).sum(axis=1)
        qk = np.where(valid, xr, 0.0) / np.where(gross > 0, gross, 1.0)[:, None]
        full = np.zeros((geo.t, geo.ever.size))
        full[geo.cell_rows, geo.cell_pos] = qk[geo.colmask]  # instruments aligned by id across rows
        live = gross > 0
        pk = (qk * geo.r1).sum(axis=1)
        mature = np.arange(geo.t) < geo.t - 2
        pnl[:, k] = np.where(live & mature, pk, np.nan)
        turnover[1:, k] = np.abs(np.diff(full, axis=0)).sum(axis=1)
        coverage[:, k] = valid.sum(axis=1) / np.maximum(elig, 1)
        if log:
            log(f"card: {k + 1}/{k_n} {cands[k]['id']} seconds={time.perf_counter() - tick:.2f}")
        return info

    for cand, info in zip(cands, run_parallel(one, list(range(k_n)), args.workers)):
        per[cand["id"]] = info
    daily, specs = row_decay(geo, R, inputs.min_names, log, args.workers)
    # theme composites (admitted members only) for the correlations
    themes = {}
    for i in inputs.admitted:
        themes.setdefault(theme_of(inputs.adm_rows.get(i), cands[ids.index(i)]), []).append(ids.index(i))
    theme_names = sorted(themes)
    comp_r = np.full((geo.t, len(theme_names), geo.w), np.nan)
    comp_pnl = np.full((geo.t, len(theme_names)), np.nan)
    for j, th in enumerate(theme_names):
        members = themes[th]
        acc = np.zeros((geo.t, geo.w))
        anyv = np.zeros((geo.t, geo.w), dtype=bool)
        for k in members:
            xk = R[:, k, :].astype(np.float64)
            vk = np.isfinite(xk)
            nk = vk.sum(axis=1, keepdims=True)
            acc += np.where(vk, xk / np.maximum(nk, 1), 0.0)
            anyv |= vk
        comp_r[:, j, :], _ = centered_ranks(acc / len(members), anyv)
        pm = pnl[:, members]
        cnt = np.isfinite(pm).sum(axis=1)
        comp_pnl[:, j] = np.where(cnt > 0, np.nansum(pm, axis=1) / np.maximum(cnt, 1), np.nan)
    # signal correlation: mean daily Spearman (marginal ranks) over rows
    k_all = k_n + len(theme_names)
    s_sum, s_cnt = np.zeros((k_all, k_all)), np.zeros((k_all, k_all))
    tick = time.perf_counter()
    for r in range(geo.t):
        w = int(geo.width[r])
        x = np.concatenate([R[r, :, :w], comp_r[r, :, :w]], axis=0).T.astype(np.float64)
        rho, _ = A.pairwise_corr(x, SIGNAL_CORR_MIN_NAMES)
        ok = np.isfinite(rho)
        s_sum[ok] += rho[ok]
        s_cnt[ok] += 1
    with np.errstate(invalid="ignore", divide="ignore"):
        sig_corr = np.where(s_cnt > 0, s_sum / np.maximum(s_cnt, 1), np.nan)
    pnl_corr, _ = A.pairwise_corr(np.hstack([pnl, comp_pnl]), PNL_CORR_MIN_DAYS)
    if log:
        log(f"card: correlations seconds={time.perf_counter() - tick:.1f}")
    # assemble
    files: dict[str, bytes] = {}
    index_rows = []
    lag_idx = [j for j, (kind, _) in enumerate(specs) if kind == "lag"]
    hs = np.array([specs[j][1] for j in lag_idx])
    daily_lines = ["id,decision_index,session_ns,turnover,pnl,coverage"]
    for k, cand in enumerate(cands):
        cid = cand["id"]
        adm = inputs.adm_rows.get(cid)
        card = {"schema": SCHEMA, "id": cid, "family": cand.get("family"), "theme": theme_of(adm, cand),
                "tier": (adm or {}).get("tier"), "dsl_sha256": cand["dsl_sha256"],
                "payload_sha256": per[cid]["payload_sha256"], "cache_entry": per[cid]["cache_entry"],
                "runner_sign": cand.get("runner_sign"), "frozen_train_sign": inputs.frozen_sign.get(cid)}
        # runner's own series
        runner_h, ic_year = {}, {}
        for h in RUNNER_HORIZONS:
            ses, vals = inputs.daily_ic.get((cid, h), (np.zeros(0, np.int64), np.zeros(0)))
            ic_year[str(h)] = by_year(vals, years_of(ses))
            rs = inputs.runner.get(cid, {}).get(h, {})
            runner_h[str(h)] = {"mean": rs.get("rank_mean"), "se": rs.get("rank_se"), "valid_dates": rs.get("dates"),
                                "hac_lag": rs.get("hac_lag"), "coverage": rs.get("coverage")}
        card["runner"] = {"min_names": inputs.min_names, "horizons": runner_h}
        card["ic_by_year"] = ic_year
        # decay
        m = np.array([np.nanmean(daily[k, :, j]) if np.isfinite(daily[k, :, j]).any() else np.nan for j in lag_idx])
        nd = [int(np.isfinite(daily[k, :, j]).sum()) for j in lag_idx]
        fit = half_life_fit(m, hs)
        card["decay"] = {"h": list(hs), "ic": list(m), "n_dates": nd, "fit": fit}
        check = {}
        for h in RUNNER_HORIZONS:
            j = specs.index(("cum", h))
            mine = daily[k, :, j]
            ses, vals = inputs.daily_ic.get((cid, h), (np.zeros(0, np.int64), np.zeros(0)))
            pos = np.searchsorted(geo.sessions, ses)
            ok_pos = (pos < geo.t) & (geo.sessions[np.minimum(pos, geo.t - 1)] == ses)
            theirs = np.full(geo.t, np.nan)
            theirs[pos[ok_pos]] = vals[ok_pos]
            both = np.isfinite(mine) & np.isfinite(theirs)
            check[str(h)] = {"card_mean": float(np.nanmean(mine)) if np.isfinite(mine).any() else None,
                             "runner_mean": float(np.nanmean(theirs)) if np.isfinite(theirs).any() else None,
                             "card_valid_dates": int(np.isfinite(mine).sum()),
                             "runner_valid_dates": int(np.isfinite(theirs).sum()),
                             "valid_date_mismatches": int((np.isfinite(mine) != np.isfinite(theirs)).sum()),
                             "max_abs_daily_diff": float(np.abs(mine[both] - theirs[both]).max()) if both.any() else None}
            cm, rm = check[str(h)]["card_mean"], check[str(h)]["runner_mean"]
            check[str(h)]["abs_mean_diff"] = abs(cm - rm) if cm is not None and rm is not None else None
        card["runner_check"] = check
        card["ic_by_size_tercile"] = {"horizon": ORIENTATION_H, "size_field": size_name, "min_names": GROUP_MIN_NAMES,
                                      "groups": per[cid]["size"]}
        card["ic_by_ff12"] = ({"horizon": ORIENTATION_H, "field": "grp_ff12", "min_names": GROUP_MIN_NAMES,
                               "groups": per[cid]["ff12"]} if per[cid]["ff12"] is not None else
                              {"groups": None, "reason": inputs.fields_note.get("grp_ff12") or
                               inputs.fields_note.get("fields", "grp_ff12 unavailable")})
        # book
        pk, tk, ck = pnl[:, k], turnover[:, k], coverage[:, k]
        both = np.isfinite(pk) & np.isfinite(tk)
        mean_p = float(np.nanmean(pk)) if np.isfinite(pk).any() else None
        sd_p = float(np.nanstd(pk, ddof=1)) if np.isfinite(pk).sum() >= 2 else None
        tau = float(np.nanmean(tk)) if np.isfinite(tk).any() else None
        sharpe = mean_p / sd_p * math.sqrt(ANNUAL) if mean_p is not None and sd_p else None
        rets = 2.0 * ANNUAL * mean_p if mean_p is not None else None
        fitness = (sharpe * math.sqrt(abs(rets) / max(tau, WQ_TURNOVER_FLOOR))
                   if sharpe is not None and rets is not None and tau is not None else None)
        margin = (1e4 * float(pk[both].sum()) / float(tk[both].sum())
                  if both.any() and float(tk[both].sum()) > 0 else None)
        card["turnover"] = {"mean": tau, "by_year": by_year(tk, geo.years)}
        card["worldquant"] = {"sharpe": sharpe, "returns": rets, "turnover": tau, "fitness": fitness,
                              "margin_bps": margin, "pnl_days": int(np.isfinite(pk).sum()),
                              "sharpe_by_year": {y: (v["mean"] / v["sd"] * math.sqrt(ANNUAL)
                                                     if v["sd"] and v["mean"] is not None else None)
                                                 for y, v in by_year(pk, geo.years).items()}}
        card["coverage"] = {y: v["mean"] for y, v in by_year(ck, geo.years).items()}
        # correlations
        corr = {}
        for kind, mat in (("signal", sig_corr), ("pnl", pnl_corr)):
            members = {i: mat[k, ids.index(i)] for i in inputs.admitted if i != cid}
            th = {t: mat[k, k_n + j] for j, t in enumerate(theme_names)}
            fin = [(abs(v), i) for i, v in members.items() if v is not None and math.isfinite(v)]
            best = sorted(fin, key=lambda z: (-z[0], z[1]))[0] if fin else None
            corr[kind] = {"members": members, "themes": th,
                          "max_abs": {"rho": members[best[1]] if best else None, "with": best[1] if best else None}}
        card["correlation"] = corr
        if adm is not None:
            rules = inputs.admission.get("rules", {})
            card["admission"] = {
                "screen": inputs.admission.get("screen"), "status": adm.get("status"),
                "failed_checks": adm.get("failed_checks"), "redundant_with": adm.get("redundant_with"),
                "redundant_rho": adm.get("redundant_rho"), "max_abs_rho": adm.get("max_abs_rho"),
                "max_abs_rho_with": adm.get("max_abs_rho_with"), "admission_rank": adm.get("admission_rank"),
                "tau": adm.get("tau"), "hac_t": adm.get("hac_t"), "train_sharpe": adm.get("train_sharpe",
                                                                                          adm.get("fit_sharpe")),
                "s_k": adm.get("s_k"), "payload_matches_admission": adm.get("cache_payload_sha256") ==
                per[cid]["payload_sha256"],
                "thresholds": {x: rules.get(x) for x in ("tau_limit", "cost_tau_limit", "veto_t", "rho_limit",
                                                         "min_train_days", "min_fit_days") if x in rules}}
        else:
            card["admission"] = {"status": "not-in-admission-table"}
        files[f"card-{cid}.json"] = canonical(card)
        files[f"card-{cid}.html"] = card_html(card).encode("utf-8")
        index_rows.append(index_row(card))
        for r in range(geo.t):
            daily_lines.append(f"{cid},{geo.sb + r},{int(geo.sessions[r])},{ffmt(tk[r])},{ffmt(pk[r])},{ffmt(ck[r])}")
    files["daily_sleeve.csv"] = ("\n".join(daily_lines) + "\n").encode("utf-8")
    index_rows.sort(key=lambda row: (row["fitness"] is None, -(row["fitness"] or 0.0), row["id"]))
    for n, row in enumerate(index_rows, 1):
        row["rank"] = n
    index = {"schema": INDEX_SCHEMA, "ranking": "WorldQuant fitness, descending (n/a last), ties by id",
             "candidates": index_rows, "admitted": inputs.admitted, "themes": theme_names,
             "definitions": definitions(size_name, inputs), "window": {
                 "decision_rows": geo.t, "first_session_ns": int(geo.sessions[0]),
                 "last_session_ns": int(geo.sessions[-1])},
             "inputs": {"u_pass": str(inputs.u_dir), "role_manifest_sha256": inputs.role.sha,
                        "library_sha256": inputs.library_sha, "vm_identity": inputs.layout.vm_identity,
                        "min_names": inputs.min_names, "files": dict(sorted(inputs.files.items())),
                        "admission_screen": inputs.admission.get("screen"),
                        "admission_runner_summary_sha256": inputs.admission.get("inputs", {}).get(
                            "runner_summary_sha256")}}
    files["index.json"] = canonical(index)
    files["index.html"] = index_html(index).encode("utf-8")
    files["manifest.json"] = canonical({"schema": "atx.alpha-report-card-manifest/v1",
                                        "files": {n: sha256_bytes(b) for n, b in sorted(files.items())}})
    summary = {"status": "complete", "output": str(args.output), "candidates": k_n,
               "seconds": round(time.perf_counter() - started, 2)}
    return files, summary


def definitions(size_name: str, inputs: Inputs) -> dict:
    return {
        "rows": "decisions [score_begin, score_end); eligible = present & member at d",
        "label_cumulative": "close[d+1+h]/close[d+1]-1 (runner)", "label_lagged": "close[d+h+1]/close[d+h]-1",
        "endpoint_rules": "both endpoints present, finite positive close; decision close finite positive; no "
                          "guarded return between (|log step| > 1.5 or > |log raw step| + .10)",
        "rank_ic": "Pearson over paired names of marginal tie-averaged ranks (signal over finite eligible, label "
                   "over finite eligible); runner rules min_names, 80% of labels, 80% of eligible",
        "rank_ic_difference": "runner re-ranks on the paired subset; runner_check states the gap at h=5/21/63",
        "ic_by_year": "runner train_daily_ic.csv rank_ic (DSL orientation); IR = mean/sd daily",
        "decay": "lagged one-day rank IC h=1..63; half-life ln2/-ln(rho) of least squares a*rho^(h-1), grid "
                 "0.25..1000 days log-spaced 2001",
        "groups": f"cumulative h={ORIENTATION_H}; within-group marginal ranks; size terciles of {size_name}; FF12 "
                  f"grp_ff12; min names {GROUP_MIN_NAMES}",
        "book": "q = centered ranks / sum|ranks| (dollar neutral, gross 1, not neutralized); pnl = sum q*r1, "
                "r1 = cumulative h=1 label else 0",
        "turnover": "mean sum_i |q(d)_i - q(d-1)_i| over consecutive rows",
        "worldquant": "sharpe=sqrt(252)*mean/sd; returns=252*mean/(booksize/2); fitness=sharpe*sqrt(|returns|/"
                      "max(turnover,.125)); margin_bps=1e4*sum pnl/sum traded; gross of costs",
        "coverage": "finite-signal eligible / eligible, mean per year",
        "correlation_signal": f"mean daily Spearman (marginal ranks, >= {SIGNAL_CORR_MIN_NAMES} names) vs admitted "
                              "members and theme composites (mean rank/n of members, missing 0)",
        "correlation_pnl": f"Pearson of daily book pnl (>= {PNL_CORR_MIN_DAYS} common days) vs admitted members "
                           "and theme composites (mean member pnl)",
        "fields": inputs.fields_note or "me_company and grp_ff12 from the pinned research fields",
    }


def index_row(card: dict) -> dict:
    wq, rh = card["worldquant"], card["runner"]["horizons"]
    fitd = card["decay"]["fit"] or {}
    ic21 = card["ic_by_year"][str(ORIENTATION_H)]["all"]
    return {"id": card["id"], "theme": card["theme"], "status": card["admission"].get("status"),
            "ic21": rh[str(ORIENTATION_H)]["mean"], "ir21": ic21["ir"], "half_life": fitd.get("half_life_days"),
            "turnover": wq["turnover"], "sharpe": wq["sharpe"], "fitness": wq["fitness"],
            "margin_bps": wq["margin_bps"], "coverage": card["coverage"].get("all"),
            "max_abs_signal_rho": card["correlation"]["signal"]["max_abs"]["rho"],
            "max_abs_signal_rho_with": card["correlation"]["signal"]["max_abs"]["with"],
            "max_abs_pnl_rho": card["correlation"]["pnl"]["max_abs"]["rho"]}


# ------------------------------------------------------------------------------------------------ pages
def _page(title: str, head_meta: list[tuple[str, str]], body: str) -> str:
    dl = "".join(f"<div><dt>{C.esc(k)}</dt><dd>{C.esc(v)}</dd></div>" for k, v in head_meta)
    return ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">'
            f'<title>{C.esc(title)}</title><style>{TH.css(False)}</style></head>'
            f'<body><div class="page"><header class="doc-head"><div class="row"><p class="eyebrow">alpha report card'
            f'</p><button type="button" id="theme-btn" class="theme-btn" hidden>Theme</button></div>'
            f'<h1>{C.esc(title)}</h1><dl class="meta">{dl}</dl></header><main>{body}</main></div>'
            f'<script>{TH.SCRIPT}</script></body></html>\n')


def _r(v):
    return q(v) if isinstance(v, float) else v


def card_html(card: dict) -> str:
    wq, fitd = card["worldquant"], card["decay"]["fit"] or {}
    adm = card["admission"]
    kpis = C.kpi_strip([
        {"label": "rank IC 21d", "value": C.fmt(card["runner"]["horizons"]["21"]["mean"], "+.4f")},
        {"label": "IR 21d", "value": C.fmt(card["ic_by_year"]["21"]["all"]["ir"], "+.3f")},
        {"label": "half-life", "value": C.fmt(fitd.get("half_life_days"), ".1f"), "sub": "days"},
        {"label": "turnover", "value": C.fmt(wq["turnover"], ".3f")},
        {"label": "fitness", "value": C.fmt(wq["fitness"], "+.2f")},
        {"label": "margin", "value": C.fmt(wq["margin_bps"], "+.2f"), "sub": "bps"},
        {"label": "coverage", "value": C.fmt(card["coverage"].get("all"), "pct1")},
        {"label": "admission", "value": str(adm.get("status"))}])
    years = [y for y in card["ic_by_year"]["21"] if y != "all"]
    fig1 = C.bar_chart(years, [{"name": f"h={h}", "color": col, "label": False,
                                "values": [card["ic_by_year"][str(h)].get(y, {}).get("mean") for y in years]}
                               for h, col in zip(RUNNER_HORIZONS, ("s1", "s2", "flat"))],
                       value_fmt="+.3f", aria="rank IC by year")
    hs, ic = card["decay"]["h"], card["decay"]["ic"]
    series = [{"name": "lagged one-day rank IC", "color": "s2", "markers": False,
               "points": [(h, v) for h, v in zip(hs, ic) if v is not None and math.isfinite(v)]}]
    if fitd:
        series.append({"name": "fit a*rho^(h-1)", "color": "flat", "dash": "4 3", "markers": False,
                       "points": [(h, fitd["a"] * fitd["rho"] ** (h - 1)) for h in hs]})
    fig2 = C.xy_chart(series, x_label="h (sessions after entry)", y_label="rank IC", x_fmt=".0f", y_fmt="+.4f",
                      aria="IC decay")
    groups = []
    for key, label in (("ic_by_size_tercile", "size"), ("ic_by_ff12", "FF12")):
        g = card[key].get("groups")
        if g:
            groups += [(f"{label} {name}", v.get("mean")) for name, v in g.items()]
    fig3 = C.bar_chart([g[0] for g in groups], [{"name": "rank IC 21d", "color": "s2", "label": True,
                                                 "values": [g[1] for g in groups]}],
                       horizontal=True, value_fmt="+.3f", label_w=120, aria="IC by group")
    corr = card["correlation"]
    names = sorted(corr["signal"]["members"]) + [f"theme:{t}" for t in sorted(corr["signal"]["themes"])]

    def cv(kind, name):
        d = corr[kind]["themes"] if name.startswith("theme:") else corr[kind]["members"]
        return d.get(name[6:] if name.startswith("theme:") else name)
    fig4 = C.bar_chart(names, [{"name": "signal", "color": "s2", "values": [cv("signal", x) for x in names]},
                               {"name": "pnl", "color": "s1", "values": [cv("pnl", x) for x in names]}],
                       horizontal=True, value_fmt="+.2f", label_w=190, aria="correlation to admitted members")
    chk_rows = [{"h": h, **{k: _r(v) for k, v in card["runner_check"][h].items()}} for h in card["runner_check"]]
    chk = C.table([{"key": "h", "label": "h", "kind": "text"}, {"key": "card_mean", "label": "card mean", "fmt": "+.5f"},
                   {"key": "runner_mean", "label": "runner mean", "fmt": "+.5f"},
                   {"key": "abs_mean_diff", "label": "|diff|", "fmt": ".2e"},
                   {"key": "max_abs_daily_diff", "label": "max daily |diff|", "fmt": ".2e"},
                   {"key": "valid_date_mismatches", "label": "valid-date mismatches", "fmt": "int"}],
                  chk_rows, sortable=False)
    adm_rows = [{"k": k, "v": json.dumps(q(v), sort_keys=True) if isinstance(v, (dict, list)) else
                 ("" if v is None else str(_r(v)))} for k, v in sorted(adm.items())]
    admt = C.table([{"key": "k", "label": "field", "kind": "mono"}, {"key": "v", "label": "value", "kind": "mono"}],
                   adm_rows, sortable=False)
    body = (C.section(1, "Key figures", kpis) +
            C.section(2, "Rank IC by year", C.figure(1, fig1, "runner daily rank IC, mean per year (h = 5, 21, 63)")) +
            C.section(3, "Decay", C.figure(2, fig2, "lagged one-day rank IC by h and the exponential fit") + chk) +
            C.section(4, "Size and industry", C.figure(3, fig3, "rank IC 21d by size tercile and FF12")) +
            C.section(5, "Correlation", C.figure(4, fig4, "signal and PnL correlation to admitted members and themes")) +
            C.section(6, "Admission", admt))
    meta = [("theme", str(card["theme"])), ("family", str(card["family"])), ("DSL sha256", card["dsl_sha256"]),
            ("payload sha256", card["payload_sha256"]), ("runner sign", str(card["runner_sign"]))]
    return _page(card["id"], meta, body)


def index_html(index: dict) -> str:
    cols = [{"key": "rank", "label": "#", "fmt": "int"},
            {"key": "id", "label": "candidate", "kind": "html",
             "fmt": lambda v, r: f'<a href="card-{C.esc(v)}.html">{C.esc(v)}</a>'},
            {"key": "theme", "label": "theme", "kind": "text"}, {"key": "status", "label": "admission", "kind": "text"},
            {"key": "ic21", "label": "IC 21d", "fmt": "+.4f"}, {"key": "ir21", "label": "IR 21d", "fmt": "+.3f"},
            {"key": "half_life", "label": "half-life", "fmt": ".1f"}, {"key": "turnover", "label": "turnover", "fmt": ".3f"},
            {"key": "sharpe", "label": "Sharpe", "fmt": "+.2f"}, {"key": "fitness", "label": "fitness", "fmt": "+.2f"},
            {"key": "margin_bps", "label": "margin bps", "fmt": "+.2f"}, {"key": "coverage", "label": "coverage", "fmt": "pct1"},
            {"key": "max_abs_signal_rho", "label": "max |rho| signal", "fmt": "+.2f"},
            {"key": "max_abs_signal_rho_with", "label": "with", "kind": "text"},
            {"key": "max_abs_pnl_rho", "label": "max |rho| pnl", "fmt": "+.2f"}]
    rows = [{k: _r(v) for k, v in row.items()} for row in index["candidates"]]
    table = C.table(cols, rows, sortable=True, tid="t-cards")
    w = index["window"]
    meta = [("candidates", str(len(rows))), ("admitted", str(len(index["admitted"]))),
            ("decision rows", str(w["decision_rows"])), ("role", index["inputs"]["role_manifest_sha256"]),
            ("ranking", index["ranking"])]
    return _page("Alpha report cards", meta, C.section(1, "Candidates", table))


# ------------------------------------------------------------------------------------------------ CLI
def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    p.add_argument("--u-pass", type=Path, required=True, help="TRAIN-only IC runner output dir (unweighted pass)")
    p.add_argument("--train", type=Path, required=True, help="TRAIN role manifest.json")
    p.add_argument("--train-sha256", required=True)
    p.add_argument("--admission", type=Path, required=True, help="admission.json of fit_composition_weights")
    p.add_argument("--admission-sha256", default=None)
    p.add_argument("--ids", default=None, help="comma-separated subset of candidate ids")
    p.add_argument("--workers", type=int, default=4, choices=range(1, 9), metavar="1..8",
                   help="threads over candidates and decision rows (output bytes do not depend on it)")
    p.add_argument("--output", type=Path, required=True, help="new output directory (never overwritten)")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    try:
        require(not Path(args.output).exists(), f"output exists; refusing overwrite: {args.output}")
        files, summary = build(args, log=lambda line: print(line, file=sys.stderr, flush=True))
        fcw.publish_directory(args.output, files)
    except (CardError, fcw.FitError) as exc:
        print(f"alpha_report_card: {exc}", file=sys.stderr)
        return EXIT_REFUSED
    print(json.dumps(summary, sort_keys=True))
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
