"""IC evaluator v2: daily cross-sectional rank IC of candidate features against the forward-return labels.

``python -m atx_db.alpha_panel.ic_eval --features <stage>[:<col,...>] [...] --out <json> [--overlap-with <stage>:<col,...>]``

``<stage>`` is ``panel``, ``characteristics`` or ``gold`` (year-partitioned ``<stage>/year=YYYY/*.parquet`` with
``session_date, security_id``); with no column list every numeric non-key column of ``characteristics``/``gold`` is used
(``panel`` needs an explicit list). Features are taken as delivered: prior-signed (higher = predicted higher return).

Definitions (Global Constraints 1, 2, 5), horizons h in {1, 5, 21, 63}, labels from the ``labels`` stage:

* IC(d) = Spearman(feature, fwd_h) over ``member_equity`` lines (universe ``lo`` = the linked-operating proxy) with both
  values non-null; sessions with fewer than ``MIN_NAMES`` (100) such lines are skipped. Mean IC, Newey-West t (lag h,
  h = 1 uses 5), share of sessions with IC > 0, sessions used and mean names per session are reported for DISCOVERY
  (2019), TRAIN (2020-01-02 .. last decision session whose 1-day label fits the cutoff) and each calendar year.
* Industry-neutral: feature and label ranks demeaned within ``grp_ff49`` (groups < 3 lines dropped), then correlated.
* Risk-adjusted: centred feature rank residualised per session by OLS on the centred ranks of ``beta_252``,
  ``vol_63``, ``log_adv63`` (``characteristics`` stage; skipped with a notice while any is absent), then Spearman
  against the label.
* Non-return statistics over all sessions from 2019-01-02 to the last panel session: coverage (mean daily share of
  ``member_equity`` lines with a finite value, per calendar year), rank autocorrelation at lags 1 and 21 sessions, and on the
  last session of each TRAIN month the pairwise mean |Spearman| between the features (and against ``--overlap-with``).

The evaluator refuses (:class:`HoldoutViolation`) any label whose end session ``d+1+h`` is after ``LABEL_CUTOFF`` or whose
decision session is after the TRAIN end; it never reads a label for a later session. Memory: features are processed in
batches of at most 16 columns, year by year (one year of ``member_equity`` rows in RAM); DuckDB scratch is file-backed.
"""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import itertools
import math
import sys
from array import array
from pathlib import Path
from typing import Any

import numpy as np

from . import common as C

SCHEMA = "atx.alpha-panel.ic-eval/v2"
LABEL_CUTOFF = dt.date(2023, 12, 29)
FIRST_SESSION = dt.date(2019, 1, 2)
DISCOVERY = (dt.date(2019, 1, 2), dt.date(2019, 12, 31))
TRAIN_START = dt.date(2020, 1, 2)
HORIZONS = (1, 5, 21, 63)
MIN_NAMES = 100
NW_MIN_SESSIONS = 30
MAX_BATCH = 16
STAGES = ("panel", "characteristics", "gold")
RISK_COLS = ("beta_252", "vol_63", "log_adv63")
KEYS = ("session_date", "security_id")
NUMERIC = ("TINYINT", "SMALLINT", "INTEGER", "BIGINT", "HUGEINT", "UTINYINT", "USMALLINT", "UINTEGER", "UBIGINT",
           "FLOAT", "DOUBLE", "DECIMAL")
UNIVERSES = ("member_equity", "lo")


class HoldoutViolation(RuntimeError):
    """A label or session that would use a return realized after the cutoff or a decision session after TRAIN end."""


# ---------------------------------------------------------------- statistics

def rank_avg(a: np.ndarray) -> np.ndarray:
    """1-based ranks with ties averaged."""
    n = a.size
    order = np.argsort(a, kind="stable")
    s = a[order]
    ranks = np.empty(n, dtype=float)
    new = np.empty(n, dtype=bool)
    new[0] = True
    new[1:] = s[1:] != s[:-1]
    starts = np.flatnonzero(new)
    ends = np.append(starts[1:], n)
    ranks[order] = np.repeat((starts + ends + 1) / 2.0, ends - starts)
    return ranks


def _corr(a: np.ndarray, b: np.ndarray) -> float:
    a = a - a.mean()
    b = b - b.mean()
    den = math.sqrt(float(a @ a) * float(b @ b))
    return float(a @ b) / den if den > 0 else float("nan")


def spearman(x: np.ndarray, y: np.ndarray) -> float:
    """Spearman rank correlation (average ranks for ties) of two equal-length finite vectors."""
    return _corr(rank_avg(x), rank_avg(y))


def nw_lag(h: int) -> int:
    return 5 if h == 1 else h


def nw_t(x: np.ndarray, lag: int, min_n: int | None = None) -> float:
    """Mean over its Newey-West (Bartlett weights) standard error; NaN with fewer than ``min_n`` finite values."""
    x = np.asarray(x, dtype=float)
    x = x[np.isfinite(x)]
    n = len(x)
    if n < (NW_MIN_SESSIONS if min_n is None else min_n):
        return float("nan")
    e = x - x.mean()
    s = float(e @ e) / n
    for k in range(1, min(lag, n - 1) + 1):
        s += 2 * (1 - k / (lag + 1)) * float(e[k:] @ e[:-k]) / n
    return float(x.mean() / math.sqrt(s / n)) if s > 0 else float("nan")


def session_ics(x: np.ndarray, y: np.ndarray, grp: np.ndarray | None, risk: np.ndarray | None,
                min_names: int) -> tuple[float, float, float] | None:
    """(raw, industry-neutral, risk-adjusted) IC of one session; ``None`` below ``min_names`` usable lines.

    ``grp`` holds integer group codes (< 0 = unclassified), ``risk`` an (n, 3) matrix of the risk inputs; the variants
    that lack their inputs, or the lines to compute on, are NaN."""
    ok = np.isfinite(x) & np.isfinite(y)
    if int(ok.sum()) < min_names:
        return None
    xs, ys = x[ok], y[ok]
    rx, ry = rank_avg(xs), rank_avg(ys)
    raw = _corr(rx, ry)
    ind = risk_ic = float("nan")
    if grp is not None:
        g = grp[ok]
        valid = g >= 0
        if valid.any():
            gi = np.unique(g[valid], return_inverse=True)[1]
            cnt = np.bincount(gi)
            keep = cnt[gi] >= 3
            idx = np.flatnonzero(valid)[keep]
            if idx.size >= min_names:
                gk = gi[keep]
                c = cnt[gk]
                xd = rx[idx] - np.bincount(gk, rx[idx])[gk] / c
                yd = ry[idx] - np.bincount(gk, ry[idx])[gk] / c
                ind = _corr(xd, yd)
    if risk is not None:
        ok2 = ok & np.all(np.isfinite(risk), axis=1)
        if int(ok2.sum()) >= min_names:
            x2, y2 = x[ok2], y[ok2]
            r2 = risk[ok2]
            fx = rank_avg(x2)
            fx = fx - fx.mean()
            cols = []
            for k in range(r2.shape[1]):
                rk = rank_avg(r2[:, k])
                cols.append(rk - rk.mean())
            design = np.column_stack(cols)
            beta = np.linalg.lstsq(design, fx, rcond=None)[0]
            resid = fx - design @ beta
            if float(resid @ resid) > 1e-12:
                risk_ic = spearman(resid, y2)
    return raw, ind, risk_ic


# ---------------------------------------------------------------- inputs

def parse_features(spec: str) -> tuple[str, list[str] | None]:
    stage, _, cols = spec.partition(":")
    if stage not in STAGES:
        raise ValueError(f"feature stage must be one of {STAGES}, got {stage!r}")
    return stage, ([c for c in cols.split(",") if c] if cols else None)


def _stage_glob(stage: str, year: int | str = "*") -> str:
    return (C.build_root() / stage / f"year={year}" / "*.parquet").as_posix()


def _stage_years(stage: str) -> list[int]:
    base = C.build_root() / stage
    return sorted(int(p.name[5:]) for p in base.glob("year=*") if p.is_dir() and any(p.glob("*.parquet")))


def _schema(con, stage: str) -> dict[str, str]:
    years = _stage_years(stage)
    if not years:
        raise FileNotFoundError(f"stage {stage} has no year partitions under {C.build_root()}")
    rows = con.execute(f"DESCRIBE SELECT * FROM read_parquet('{_stage_glob(stage, years[-1])}')").fetchall()
    return {r[0]: r[1] for r in rows}


def resolve_features(con, stage: str, cols: list[str] | None) -> list[str]:
    schema = _schema(con, stage)
    if cols is None:
        if stage == "panel":
            raise ValueError("stage panel needs an explicit column list")
        return [c for c, t in schema.items() if c not in KEYS and t.split("(")[0] in NUMERIC]
    missing = [c for c in cols if c not in schema]
    if missing:
        raise ValueError(f"{stage}: unknown columns {missing}")
    return cols


def check_labels(con, cal: list[dt.date]) -> dt.date:
    """Refuse labels that would use returns after ``LABEL_CUTOFF`` or decision sessions after the TRAIN end.

    Returns the TRAIN end (the last decision session whose 1-day label fits the cutoff)."""
    idx_of = {d: i for i, d in enumerate(cal)}
    cut_idx = idx_of[LABEL_CUTOFF]
    train_end = cal[cut_idx - 2]
    glob = _stage_glob("labels")
    if not _stage_years("labels"):
        raise FileNotFoundError("labels stage is missing (build it with atx_db.alpha_panel.labels)")
    top = con.execute(f"SELECT max(session_date) FROM read_parquet('{glob}')").fetchone()[0]
    if top is not None and top > train_end:
        raise HoldoutViolation(f"label decision session {top} is after the TRAIN end {train_end}")
    for h in HORIZONS:
        d = con.execute(f"SELECT max(session_date) FROM read_parquet('{glob}') WHERE fwd_{h} IS NOT NULL").fetchone()[0]
        if d is None:
            continue
        end = idx_of.get(d, -1) + 1 + h
        if d not in idx_of or end > cut_idx:
            raise HoldoutViolation(f"a fwd_{h} label at decision {d} ends after the cutoff {LABEL_CUTOFF}")
    return train_end


def _f(a: Any) -> np.ndarray:
    """fetchnumpy column -> float64 with NaN for NULL."""
    if isinstance(a, np.ma.MaskedArray):
        return a.astype(np.float64).filled(np.nan)
    return np.asarray(a, dtype=np.float64)


def _num(v: float, nd: int = 6) -> float | None:
    return None if v is None or not math.isfinite(v) else round(float(v), nd)


# ---------------------------------------------------------------- aggregation

class Series:
    """Per-session IC series of one (feature, horizon, universe), compact."""

    __slots__ = ("date", "ind", "n", "raw", "risk")

    def __init__(self) -> None:
        self.date, self.n = array("i"), array("i")
        self.raw, self.ind, self.risk = array("d"), array("d"), array("d")

    def add(self, date: int, n: int, res: tuple[float, float, float]) -> None:
        self.date.append(date)
        self.n.append(n)
        self.raw.append(res[0])
        self.ind.append(res[1])
        self.risk.append(res[2])


def _stat(s: Series, sel: np.ndarray, h: int) -> dict[str, Any] | None:
    if not sel.any():
        return None
    lag = nw_lag(h)
    raw = np.frombuffer(s.raw, dtype=np.float64)[sel]
    ind = np.frombuffer(s.ind, dtype=np.float64)[sel]
    risk = np.frombuffer(s.risk, dtype=np.float64)[sel]
    n = np.frombuffer(s.n, dtype=np.int32)[sel]

    def m(v: np.ndarray) -> float:
        v = v[np.isfinite(v)]
        return float(v.mean()) if v.size else float("nan")

    return {"sessions": int(raw.size), "names_mean": _num(float(n.mean()), 2), "ic_mean": _num(m(raw)),
            "nw_t": _num(nw_t(raw, lag), 3), "pos_share": _num(float((raw > 0).mean()), 4),
            "ind_ic_mean": _num(m(ind)), "ind_nw_t": _num(nw_t(ind, lag), 3),
            "risk_ic_mean": _num(m(risk)), "risk_nw_t": _num(nw_t(risk, lag), 3)}


def _summaries(s: Series, h: int) -> dict[str, Any]:
    days = np.frombuffer(s.date, dtype=np.int32)
    d64 = days.astype("datetime64[D]")

    def win(a: dt.date, b: dt.date) -> np.ndarray:
        return (d64 >= np.datetime64(a)) & (d64 <= np.datetime64(b))

    out: dict[str, Any] = {"DISCOVERY": _stat(s, win(*DISCOVERY), h),
                           "TRAIN": _stat(s, d64 >= np.datetime64(TRAIN_START), h), "years": {}}
    yrs = d64.astype("datetime64[Y]").astype(int) + 1970
    for y in sorted(set(yrs.tolist())):
        out["years"][str(y)] = _stat(s, yrs == y, h)
    return out


# ---------------------------------------------------------------- main run

def _batches(feats: list[tuple[str, str]]) -> list[tuple[str, list[str]]]:
    out: list[tuple[str, list[str]]] = []
    for stage in STAGES:
        cols = [c for s, c in feats if s == stage]
        for i in range(0, len(cols), MAX_BATCH):
            out.append((stage, cols[i:i + MAX_BATCH]))
    return out


def _fetch_block(con, stage: str, year: int, cols: list[str], n: int) -> np.ndarray:
    if year not in _stage_years(stage):
        return np.full((n, len(cols)), np.nan)
    sel = ", ".join(f"CAST(f.{c} AS DOUBLE) AS c{i}" for i, c in enumerate(cols))
    got = con.execute(f"""SELECT {sel} FROM meta m LEFT JOIN read_parquet('{_stage_glob(stage, year)}') f
                          ON f.session_date = m.session_date AND f.security_id = m.security_id ORDER BY m.rn""").fetchnumpy()
    X = np.column_stack([_f(got[f"c{i}"]) for i in range(len(cols))])
    X[~np.isfinite(X)] = np.nan
    return X


def _month_ends(cal: list[dt.date], train_end: dt.date) -> list[dt.date]:
    last: dict[tuple[int, int], dt.date] = {}
    for d in cal:
        if TRAIN_START <= d <= train_end:
            last[(d.year, d.month)] = d
    return sorted(last.values())


def _pair_rho(ri: np.ndarray, rj: np.ndarray, xi: np.ndarray, xj: np.ndarray, fi: np.ndarray, fj: np.ndarray) -> float:
    common = fi & fj
    if int(common.sum()) < MIN_NAMES:
        return float("nan")
    if int(common.sum()) == int(fi.sum()) == int(fj.sum()):
        return _corr(ri[common], rj[common])
    return spearman(xi[common], xj[common])


def _redundancy(con, cal, train_end, feats: list[tuple[str, str]], overlap: list[tuple[str, str]],
                notices: list[str]) -> tuple[dict, dict]:
    """Mean |Spearman| on each TRAIN month-end: feature pairs and feature vs overlap columns."""
    allcols: list[tuple[str, str]] = list(feats) + list(overlap)
    nf = len(feats)
    sums: dict[tuple[int, int], list[float]] = collections.defaultdict(lambda: [0.0, 0])
    months = _month_ends(cal, train_end)
    for d in months:
        yrs = _stage_years("panel")
        if d.year not in yrs:
            continue
        ids = con.execute(f"""SELECT security_id FROM read_parquet('{_stage_glob("panel", d.year)}')
                              WHERE session_date = DATE '{d}' AND member_equity ORDER BY security_id""").fetchnumpy()["security_id"]
        if len(ids) < MIN_NAMES:
            continue
        M = np.full((len(ids), len(allcols)), np.nan)
        con.execute("CREATE OR REPLACE TABLE mids AS SELECT security_id, row_number() OVER (ORDER BY security_id) AS rn "
                    f"FROM read_parquet('{_stage_glob('panel', d.year)}') WHERE session_date = DATE '{d}' AND member_equity")
        for stage in STAGES:
            pos = [(k, c) for k, (s, c) in enumerate(allcols) if s == stage]
            if not pos or d.year not in _stage_years(stage):
                continue
            for i in range(0, len(pos), 40):
                chunk = pos[i:i + 40]
                sel = ", ".join(f"CAST(f.{c} AS DOUBLE) AS c{k}" for k, c in chunk)
                got = con.execute(f"""SELECT {sel} FROM mids m LEFT JOIN read_parquet('{_stage_glob(stage, d.year)}') f
                                      ON f.security_id = m.security_id AND f.session_date = DATE '{d}' ORDER BY m.rn""").fetchnumpy()
                for k, _c in chunk:
                    M[:, k] = _f(got[f"c{k}"])
        M[~np.isfinite(M)] = np.nan
        F = np.isfinite(M)
        R = np.full(M.shape, np.nan)
        for k in range(M.shape[1]):
            if F[:, k].sum() >= MIN_NAMES:
                R[F[:, k], k] = rank_avg(M[F[:, k], k])
        for i in range(nf):
            if F[:, i].sum() < MIN_NAMES:
                continue
            for j in range(i + 1, nf):
                if F[:, j].sum() < MIN_NAMES:
                    continue
                v = _pair_rho(R[:, i], R[:, j], M[:, i], M[:, j], F[:, i], F[:, j])
                if math.isfinite(v):
                    cell = sums[(i, j)]
                    cell[0] += abs(v)
                    cell[1] += 1
            for k in range(nf, len(allcols)):
                if allcols[k] == feats[i] or F[:, k].sum() < MIN_NAMES:
                    continue
                v = _pair_rho(R[:, i], R[:, k], M[:, i], M[:, k], F[:, i], F[:, k])
                if math.isfinite(v):
                    cell = sums[(i, k)]
                    cell[0] += abs(v)
                    cell[1] += 1
    con.execute("DROP TABLE IF EXISTS mids")
    names = [f"{s}:{c}" for s, c in allcols]
    pairs = {f"{names[i]}|{names[j]}": _num(v[0] / v[1], 4) for (i, j), v in sums.items() if j < nf and v[1]}
    red: dict[str, Any] = {}
    ovl: dict[str, Any] = {}
    for i in range(nf):
        partners = []
        for (a, b), v in sums.items():
            if b < nf and v[1] and i in (a, b):
                partners.append((names[b if a == i else a], v[0] / v[1]))
        if partners:
            best = max(partners, key=lambda t: t[1])
            red[names[i]] = {"max_abs_spearman": _num(best[1], 4), "with": best[0],
                             "mean_abs_spearman": _num(float(np.mean([p[1] for p in partners])), 4), "months": len(months)}
        ov = [(names[k], v[0] / v[1]) for (a, k), v in sums.items() if a == i and k >= nf and v[1]]
        if ov:
            best = max(ov, key=lambda t: t[1])
            ovl[names[i]] = {"max_abs_spearman": _num(best[1], 4), "with": best[0], "months": len(months)}
    if not months:
        notices.append("no TRAIN month-end sessions: redundancy/overlap not computed")
    return red, {"pairs": pairs, "months": [str(m) for m in months], "overlap": ovl}


def run(features: list[tuple[str, list[str] | None]], out: Path | None = None,
        overlap: tuple[str, list[str] | None] | None = None, memory: str = "450MB", threads: int = 2) -> dict[str, Any]:
    root = C.build_root()
    con = C.connect(memory=memory, threads=threads, db_file="ic_eval.duckdb")
    cal = C.load_calendar(con)
    train_end = check_labels(con, cal)
    notices: list[str] = []
    feats: list[tuple[str, str]] = []
    for stage, cols in features:
        for c in resolve_features(con, stage, cols):
            if (stage, c) not in feats:
                feats.append((stage, c))
    ovl_cols: list[tuple[str, str]] = []
    if overlap:
        ovl_cols = [(overlap[0], c) for c in resolve_features(con, overlap[0], overlap[1])]
    risk_ok = False
    if _stage_years("characteristics"):
        sch = _schema(con, "characteristics")
        risk_ok = all(c in sch for c in RISK_COLS)
    if not risk_ok:
        notices.append(f"risk adjustment skipped: the characteristics stage does not (yet) provide {list(RISK_COLS)}")
    last_panel = con.execute(f"SELECT max(session_date) FROM read_parquet('{_stage_glob('panel')}')").fetchone()[0]
    years = [y for y in _stage_years("panel") if FIRST_SESSION.year <= y <= last_panel.year]
    batches = _batches(feats)
    series: dict[tuple[int, int, str], Series] = {}
    cov_sum: dict[int, dict[int, list[float]]] = collections.defaultdict(lambda: collections.defaultdict(lambda: [0.0, 0]))
    ac: dict[int, dict[int, list[float]]] = collections.defaultdict(lambda: {1: [], 21: []})
    bufs: dict[int, collections.deque] = {bi: collections.deque(maxlen=21) for bi in range(len(batches))}
    gpos = {f: i for i, f in enumerate(feats)}
    for year in years:
        con.execute(f"""
            CREATE OR REPLACE TABLE meta AS
            SELECT row_number() OVER (ORDER BY p.session_date, p.security_id) - 1 AS rn, p.session_date, p.security_id,
                   CASE WHEN p.grp_ff49 IS NULL THEN -1 ELSE dense_rank() OVER (ORDER BY p.grp_ff49) END AS grp,
                   coalesce(p.cik IS NOT NULL AND p.link_tier <> 'backfill' AND p.is_issuer_primary AND p.is_common, false) AS lo
            FROM read_parquet('{_stage_glob('panel', year)}') p
            WHERE p.member_equity AND p.session_date >= DATE '{FIRST_SESSION}'""")
        base = con.execute("SELECT session_date, security_id, grp, lo FROM meta ORDER BY rn").fetchnumpy()
        n = len(base["security_id"])
        if n == 0:
            continue
        dates = np.asarray(base["session_date"]).astype("datetime64[D]").astype(np.int32)
        ids = np.asarray(base["security_id"], dtype=np.int64)
        grp = np.asarray(base["grp"], dtype=np.int64)
        lo = np.asarray(base["lo"], dtype=bool)
        del base
        edges = np.concatenate([[0], np.flatnonzero(np.diff(dates)) + 1, [n]])
        has_labels = year in _stage_years("labels") and dt.date(year, 1, 1) <= train_end
        Y: dict[int, np.ndarray] = {}
        risk = None
        if has_labels:
            sel = ", ".join(f"CASE WHEN m.session_date <= DATE '{train_end}' THEN l.fwd_{h} END AS fwd_{h}" for h in HORIZONS)
            got = con.execute(f"""SELECT {sel} FROM meta m LEFT JOIN read_parquet('{_stage_glob('labels', year)}') l
                                  ON l.session_date = m.session_date AND l.security_id = m.security_id
                                  ORDER BY m.rn""").fetchnumpy()
            Y = {h: _f(got[f"fwd_{h}"]) for h in HORIZONS}
            if risk_ok and year in _stage_years("characteristics"):
                got = con.execute(f"""SELECT {', '.join('c.' + r for r in RISK_COLS)} FROM meta m
                    LEFT JOIN read_parquet('{_stage_glob('characteristics', year)}') c
                    ON c.session_date = m.session_date AND c.security_id = m.security_id ORDER BY m.rn""").fetchnumpy()
                risk = np.column_stack([_f(got[r]) for r in RISK_COLS])
            else:
                risk = None
        stage_sessions: dict[str, set[int]] = {}
        for stage in {b[0] for b in batches}:
            if year in _stage_years(stage):
                got = con.execute(f"SELECT DISTINCT session_date FROM read_parquet('{_stage_glob(stage, year)}')").fetchnumpy()
                stage_sessions[stage] = set(np.asarray(got["session_date"]).astype("datetime64[D]").astype(np.int32).tolist())
            else:
                stage_sessions[stage] = set()
        train_end_i = int(np.datetime64(train_end).astype("datetime64[D]").astype(np.int32))
        first_i = int(np.datetime64(FIRST_SESSION).astype("datetime64[D]").astype(np.int32))
        for bi, (stage, cols) in enumerate(batches):
            X = _fetch_block(con, stage, year, cols, n)
            gj = [gpos[(stage, c)] for c in cols]
            for a, b in itertools.pairwise(edges):
                di = int(dates[a])
                sl = slice(a, b)
                if di in stage_sessions[stage]:
                    share = np.isfinite(X[sl]).mean(axis=0)
                    for k, g in enumerate(gj):
                        cell = cov_sum[g][year]
                        cell[0] += float(share[k])
                        cell[1] += 1
                cur_ids, cur_X = ids[sl], X[sl]
                buf = bufs[bi]
                for lag in (1, 21):
                    if len(buf) >= lag:
                        pid, pX = buf[-lag][1], buf[-lag][2]
                        _c, ia, ib = np.intersect1d(cur_ids, pid, assume_unique=True, return_indices=True)
                        if ia.size >= MIN_NAMES:
                            for k, g in enumerate(gj):
                                u, v = cur_X[ia, k], pX[ib, k]
                                okk = np.isfinite(u) & np.isfinite(v)
                                if int(okk.sum()) >= MIN_NAMES:
                                    ac[g][lag].append(spearman(u[okk], v[okk]))
                buf.append((di, cur_ids.copy(), cur_X.copy()))
                if not has_labels or di > train_end_i or di < first_i:
                    continue
                for u_name in UNIVERSES:
                    um = np.ones(b - a, dtype=bool) if u_name == "member_equity" else lo[sl]
                    if int(um.sum()) < MIN_NAMES:
                        continue
                    gsub = grp[sl][um]
                    rsub = risk[sl][um] if risk is not None else None
                    for h in HORIZONS:
                        ysub = Y[h][sl][um]
                        if int(np.isfinite(ysub).sum()) < MIN_NAMES:
                            continue
                        for k, g in enumerate(gj):
                            xs = cur_X[um, k]
                            res = session_ics(xs, ysub, gsub, rsub, MIN_NAMES)
                            if res is None:
                                continue
                            nn = int((np.isfinite(xs) & np.isfinite(ysub)).sum())
                            series.setdefault((g, h, u_name), Series()).add(di, nn, res)
            del X
        con.execute("DROP TABLE IF EXISTS meta")
        print(f"ic_eval: year {year} done", flush=True)
    red, pair_info = _redundancy(con, cal, train_end, feats, ovl_cols, notices) if feats else ({}, {"pairs": {}, "months": [], "overlap": {}})
    con.close()
    (root / "_tmp" / "ic_eval.duckdb").unlink(missing_ok=True)
    result: dict[str, Any] = {
        "schema": SCHEMA, "generated_utc": dt.datetime.now(dt.UTC).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "label_cutoff": str(LABEL_CUTOFF), "train_end": str(train_end),
        "windows": {"DISCOVERY": [str(DISCOVERY[0]), str(DISCOVERY[1])], "TRAIN": [str(TRAIN_START), str(train_end)]},
        "horizons": list(HORIZONS), "universes": list(UNIVERSES), "min_names": MIN_NAMES,
        "risk_adjustment": {"available": risk_ok, "inputs": list(RISK_COLS)},
        "last_panel_session": str(last_panel), "notices": notices,
        "features_requested": [f"{s}:{c}" for s, c in feats], "features": {}}
    for g, (stage, col) in enumerate(feats):
        entry: dict[str, Any] = {"stage": stage, "column": col, "ic": {}}
        entry["coverage"] = {str(y): _num(v[0] / v[1], 4) for y, v in sorted(cov_sum[g].items()) if v[1]}
        entry["rank_autocorr"] = {f"lag{k}": _num(float(np.mean(ac[g][k])), 4) if ac[g][k] else None for k in (1, 21)}
        for h in HORIZONS:
            entry["ic"][f"h{h}"] = {u: _summaries(series[(g, h, u)], h) for u in UNIVERSES if (g, h, u) in series}
        key = f"{stage}:{col}"
        entry["overlap"] = pair_info["overlap"].get(key)
        entry["redundancy"] = red.get(key)
        result["features"][key] = entry
    result["pairwise_redundancy"] = {"months": pair_info["months"], "pairs": pair_info["pairs"]}
    if out is not None:
        C.write_json_atomic(out, result)
    return result


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    ap.add_argument("--features", nargs="+", required=True, help="<stage>[:<col,...>] (stage: panel|characteristics|gold)")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--overlap-with", default=None, help="<stage>:<col,...>")
    ap.add_argument("--memory", default="450MB")
    a = ap.parse_args(argv)
    res = run([parse_features(s) for s in a.features], a.out,
              overlap=parse_features(a.overlap_with) if a.overlap_with else None, memory=a.memory)
    print(f"ic_eval: {len(res['features'])} features -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
