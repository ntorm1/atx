"""Pure analyses over existing TRAIN outputs for the pitch report (numpy only, no I/O except the parsers).

Every function takes arrays / parsed rows and returns plain dicts or arrays; ``pitch.py`` loads the inputs
through the report ``Registry`` and caches the results per build. Conventions:
- correlations are pairwise-complete Pearson (``pairwise_corr``); a per-day cross-sectional Spearman is the
  Pearson of tie-averaged ranks taken over each signal's own finite members, on the names both signals cover;
- moments are population moments (skew m3/m2^1.5, kurtosis m4/m2^2 raw, normal = 3), as in nav_summ;
- a period "sum" is the arithmetic sum of daily returns (a drag decomposition), a "compounded" value is
  prod(1 + r) - 1.
"""
from __future__ import annotations

import csv
import datetime as dt
import io
import json
import math

import numpy as np


# ----------------------------------------------------------------------------------------------- parsers
def load_daily_ic(text: str, column: str = 'rank_ic') -> dict:
    """``train_daily_ic.csv`` -> {(id, horizon): (session_ns int64 array, value float array)} in file order."""
    acc: dict = {}
    for r in csv.DictReader(io.StringIO(text)):
        try:
            v = float(r[column])
        except (TypeError, ValueError):
            v = float('nan')
        acc.setdefault((r['id'], int(r['horizon'])), ([], []))
        acc[(r['id'], int(r['horizon']))][0].append(int(r['session_ns']))
        acc[(r['id'], int(r['horizon']))][1].append(v)
    return {k: (np.array(s, dtype=np.int64), np.array(v, dtype=np.float64)) for k, (s, v) in acc.items()}


def load_candidates(text: str) -> dict:
    """``train_candidates.jsonl`` complete rows -> {id: {horizon: {rank_mean, rank_se, pearson_mean, pearson_se,
    hac_lag, dates, coverage}}} (coverage = paired_signal_pairs / decision_eligible_pairs)."""
    out = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        o = json.loads(line)
        if 'horizons' not in o:
            continue
        hs = {}
        for h in o['horizons']:
            rk, pe, cov = h.get('rank') or {}, h.get('pearson') or {}, h.get('coverage') or {}
            elig = cov.get('decision_eligible_pairs')
            hs[int(h['horizon'])] = {'rank_mean': rk.get('mean'), 'rank_se': rk.get('standard_error'),
                                     'pearson_mean': pe.get('mean'), 'pearson_se': pe.get('standard_error'),
                                     'hac_lag': rk.get('required_hac_lag'), 'dates': rk.get('valid_dates'),
                                     'coverage': (cov.get('paired_signal_pairs') / elig) if elig else None}
        out[o['id']] = hs
    return out


# ----------------------------------------------------------------------------------------------- correlation
def pairwise_corr(x: np.ndarray, min_n: int = 30) -> tuple[np.ndarray, np.ndarray]:
    """Columns of ``x`` (rows = observations, NaN = missing): pairwise-complete Pearson matrix and pair counts."""
    x = np.asarray(x, dtype=np.float64)
    m = np.isfinite(x).astype(np.float64)
    z = np.where(np.isfinite(x), x, 0.0)
    n = m.T @ m
    sx = z.T @ m
    sxx = (z * z).T @ m
    sxy = z.T @ z
    with np.errstate(invalid='ignore', divide='ignore'):
        mx, my = sx / n, sx.T / n
        cov = sxy / n - mx * my
        vx, vy = sxx / n - mx * mx, sxx.T / n - my * my
        r = cov / np.sqrt(vx * vy)
    r[(n < min_n) | ~np.isfinite(r)] = np.nan
    np.fill_diagonal(r, 1.0)
    return r, n


def rank_avg(x: np.ndarray) -> np.ndarray:
    """Tie-averaged ranks 1..k over the finite entries (NaN elsewhere)."""
    out = np.full(x.shape, np.nan)
    m = np.isfinite(x)
    v = x[m]
    if v.size == 0:
        return out
    order = np.argsort(v, kind='mergesort')
    sv = v[order]
    new = np.r_[True, sv[1:] != sv[:-1]]
    starts = np.flatnonzero(new)
    ends = np.r_[starts[1:], sv.size]
    avg = (starts + ends - 1) / 2.0 + 1.0
    r = np.empty(v.size)
    r[order] = avg[np.cumsum(new) - 1]
    out[m] = r
    return out


def ic_matrix(ic: dict, ids: list[str], horizon: int, min_n: int = 60) -> dict:
    """Correlation across dates of the candidates' daily IC series at ``horizon`` (union of sessions)."""
    sess = sorted({int(s) for cid in ids if (cid, horizon) in ic for s in ic[(cid, horizon)][0]})
    pos = {s: i for i, s in enumerate(sess)}
    x = np.full((len(sess), len(ids)), np.nan)
    for j, cid in enumerate(ids):
        if (cid, horizon) not in ic:
            continue
        s, v = ic[(cid, horizon)]
        x[[pos[int(a)] for a in s], j] = v
    r, n = pairwise_corr(x, min_n)
    return {'ids': ids, 'r': r, 'n': n, 'dates': len(sess)}


def signal_corr(read_row, ids: list[str], day_rows: list[int], members) -> dict:
    """Average over ``day_rows`` of the per-day cross-sectional Spearman matrix of the candidate signals.

    ``read_row(k, d)`` -> the float row (all instruments) of candidate k at date row d; ``members(d)`` -> bool mask
    of the day's members. Returns the mean matrix, the per-pair day counts and the mean member count."""
    k = len(ids)
    s = np.zeros((k, k))
    c = np.zeros((k, k))
    names = []
    for d in day_rows:
        mask = members(d)
        if not mask.any():
            continue
        x = np.column_stack([rank_avg(np.asarray(read_row(j, d), dtype=np.float64)[mask]) for j in range(k)])
        names.append(int(mask.sum()))
        r, _ = pairwise_corr(x, 50)
        ok = np.isfinite(r)
        s[ok] += r[ok]
        c[ok] += 1
    with np.errstate(invalid='ignore', divide='ignore'):
        mean = np.where(c > 0, s / np.maximum(c, 1), np.nan)
    return {'ids': ids, 'r': mean, 'days': c, 'mean_names': float(np.mean(names)) if names else None,
            'n_days': len(names)}


def group_average(r: np.ndarray, groups: list) -> dict:
    """Mean off-diagonal correlation within each group, between each pair of groups, and overall within/between."""
    g = list(dict.fromkeys(groups))
    idx = {x: [i for i, y in enumerate(groups) if y == x] for x in g}
    mat = np.full((len(g), len(g)), np.nan)
    within, between = [], []
    for a, ga in enumerate(g):
        for b, gb in enumerate(g):
            vals = [r[i, j] for i in idx[ga] for j in idx[gb] if i != j and np.isfinite(r[i, j])]
            if vals:
                mat[a, b] = float(np.mean(vals))
    n = r.shape[0]
    for i in range(n):
        for j in range(i + 1, n):
            if np.isfinite(r[i, j]):
                (within if groups[i] == groups[j] else between).append(r[i, j])
    return {'groups': g, 'mat': mat, 'within': float(np.mean(within)) if within else None,
            'between': float(np.mean(between)) if between else None, 'n_within': len(within), 'n_between': len(between),
            'abs_within': float(np.mean(np.abs(within))) if within else None,
            'abs_between': float(np.mean(np.abs(between))) if between else None}


def offdiag_stats(r: np.ndarray, ids: list[str]) -> dict:
    """Mean, mean |r|, and the largest positive / negative off-diagonal pair."""
    n = r.shape[0]
    pairs = [(r[i, j], ids[i], ids[j]) for i in range(n) for j in range(i + 1, n) if np.isfinite(r[i, j])]
    if not pairs:
        return {}
    v = np.array([p[0] for p in pairs])
    hi, lo = max(pairs), min(pairs)
    return {'mean': float(v.mean()), 'mean_abs': float(np.abs(v).mean()), 'n_pairs': len(pairs),
            'max': float(hi[0]), 'max_pair': f'{hi[1]} / {hi[2]}', 'min': float(lo[0]), 'min_pair': f'{lo[1]} / {lo[2]}',
            'share_abs_gt_50': float(np.mean(np.abs(v) > 0.5))}


# ----------------------------------------------------------------------------------------------- IC
def ic_by_year(ic: dict, ids: list[str], horizon: int, year_of) -> dict:
    """{id: {year: mean IC}} over each candidate's daily IC at ``horizon``."""
    out = {}
    for cid in ids:
        if (cid, horizon) not in ic:
            continue
        s, v = ic[(cid, horizon)]
        acc: dict = {}
        for a, b in zip(s, v):
            if np.isfinite(b):
                acc.setdefault(year_of(int(a)), []).append(b)
        out[cid] = {y: float(np.mean(x)) for y, x in acc.items()}
    return out


def nw_se(x: np.ndarray, lag: int) -> float | None:
    """Newey-West (Bartlett) standard error of the mean of ``x`` (finite entries, file order)."""
    x = np.asarray(x, dtype=np.float64)
    x = x[np.isfinite(x)]
    n = x.size
    if n < 3:
        return None
    d = x - x.mean()
    lrv = float(d @ d) / n
    for k in range(1, min(lag, n - 1) + 1):
        lrv += 2.0 * (1.0 - k / (lag + 1.0)) * float(d[k:] @ d[:-k]) / n
    return math.sqrt(lrv / n) if lrv > 0 else None


def ic_mean(ic: dict, cid: str, horizon: int) -> float | None:
    if (cid, horizon) not in ic:
        return None
    v = ic[(cid, horizon)][1]
    v = v[np.isfinite(v)]
    return float(v.mean()) if v.size else None


# ----------------------------------------------------------------------------------------------- returns
def drawdowns(dates: list, nav: np.ndarray, k: int = 5) -> list[dict]:
    """Top-k peak-to-trough episodes of ``nav`` (depth, peak / trough / recovery dates, lengths in sessions)."""
    eps, peak_i, trough_i = [], 0, None
    for i in range(1, len(nav)):
        if nav[i] >= nav[peak_i]:
            if trough_i is not None:
                eps.append((peak_i, trough_i, i))
                trough_i = None
            peak_i = i
        elif trough_i is None or nav[i] < nav[trough_i]:
            trough_i = i
    if trough_i is not None:
        eps.append((peak_i, trough_i, None))
    rows = []
    for p, t, rcv in eps:
        rows.append({'depth': float(nav[t] / nav[p] - 1.0), 'peak': dates[p], 'trough': dates[t],
                     'recovery': dates[rcv] if rcv is not None else None, 'down': t - p,
                     'up': (rcv - t) if rcv is not None else None, 'total': (rcv - p) if rcv is not None else None})
    return sorted(rows, key=lambda r: r['depth'])[:k]


def moments(x: np.ndarray) -> dict:
    x = np.asarray(x, dtype=np.float64)
    x = x[np.isfinite(x)]
    if x.size < 3:
        return {}
    m = x.mean()
    d = x - m
    m2, m3, m4 = (d ** 2).mean(), (d ** 3).mean(), (d ** 4).mean()
    return {'n': int(x.size), 'mean': float(m), 'sd': float(x.std(ddof=1)), 'skew': float(m3 / m2 ** 1.5) if m2 > 0 else None,
            'kurt': float(m4 / m2 ** 2) if m2 > 0 else None, 'min': float(x.min()), 'max': float(x.max()),
            'pos_share': float(np.mean(x > 0)), 'p5': float(np.quantile(x, .05)), 'p50': float(np.quantile(x, .5)),
            'p95': float(np.quantile(x, .95))}


def worst(dates: list, r: np.ndarray, k: int = 5) -> list[tuple]:
    idx = np.argsort(r)[:k]
    return [(dates[i], float(r[i])) for i in idx]


def lo_se(sr_annual: float, years: float) -> float:
    """Lo (2002) iid standard error of an annualised Sharpe estimated over ``years``."""
    return math.sqrt((1.0 + sr_annual * sr_annual / 2.0) / years)


# ----------------------------------------------------------------------------------------------- costs
def cost_by_year(cols: dict, dates_all: list, ret_mask: np.ndarray) -> dict:
    """Per calendar year of the return row: arithmetic sums of the daily return components.

    The trading cost debited in return row t comes from the fills of row t-1: it is split into linear and
    impact by that row's dollar shares. The short financing (borrow_return) is split into GC / warm / special by
    the same row's tier dollars. ``gross`` includes write-offs; ``gross_ex_wo`` excludes them."""
    out: dict = {}
    n = len(dates_all)

    def col(name):
        return cols.get(name, np.full(n, np.nan))
    lin, imp = col('linear_cost_dollars'), col('impact_cost_dollars')
    gc, warm, spec = col('short_financing_gc_dollars'), col('short_financing_warm_dollars'), col('short_financing_special_dollars')
    for t in np.flatnonzero(ret_mask):
        y = dates_all[t].year
        o = out.setdefault(y, {k: 0.0 for k in ('gross', 'writeoff', 'gross_ex_wo', 'linear', 'impact', 'trade', 'long_fin',
                                                'short_fin', 'short_gc', 'short_warm', 'short_special', 'net', 'net_comp',
                                                'sessions')})
        tc = float(cols['trade_cost_return'][t])
        a, b = (float(lin[t - 1]), float(imp[t - 1])) if t > 0 else (0.0, 0.0)
        fl = a / (a + b) if (a + b) > 0 else 0.0
        bw = float(cols['borrow_return'][t])
        g_, w_, s_ = float(gc[t]), float(warm[t]), float(spec[t])
        tot = g_ + w_ + s_
        wo = float(cols['writeoff_return'][t]) if 'writeoff_return' in cols else 0.0
        o['gross'] += float(cols['gross_return'][t])
        o['writeoff'] += wo
        o['gross_ex_wo'] += float(cols['gross_return'][t]) - wo
        o['trade'] += tc
        o['linear'] += tc * fl
        o['impact'] += tc * (1 - fl)
        o['long_fin'] += float(cols['long_financing_return'][t])
        o['short_fin'] += bw
        if tot > 0:
            o['short_gc'] += bw * g_ / tot
            o['short_warm'] += bw * w_ / tot
            o['short_special'] += bw * s_ / tot
        o['net'] += float(cols['net_return'][t])
        o['net_comp'] = (1 + o['net_comp']) * (1 + float(cols['net_return'][t])) - 1
        o['sessions'] += 1
    tr = col('traded_dollars')
    for t in range(n):
        y = dates_all[t].year
        if y in out and np.isfinite(tr[t]) and tr[t] > 0:
            o = out[y]
            o['traded'] = o.get('traded', 0.0) + float(tr[t])
            o['lin_dollars'] = o.get('lin_dollars', 0.0) + float(lin[t])
            o['imp_dollars'] = o.get('imp_dollars', 0.0) + float(imp[t])
    for o in out.values():
        if o.get('traded'):
            o['lin_bps'] = o['lin_dollars'] / o['traded'] * 1e4
            o['imp_bps'] = o['imp_dollars'] / o['traded'] * 1e4
            o['cost_bps'] = o['lin_bps'] + o['imp_bps']
    return out


def rolling_mean(x: np.ndarray, w: int) -> np.ndarray:
    out = np.full(x.size, np.nan)
    if x.size >= w:
        c = np.concatenate([[0.0], np.cumsum(np.nan_to_num(x))])
        out[w - 1:] = (c[w:] - c[:-w]) / w
    return out


def year_of_ns(ns: int) -> int:
    return dt.datetime.fromtimestamp(int(ns) / 1e9, dt.timezone.utc).year
