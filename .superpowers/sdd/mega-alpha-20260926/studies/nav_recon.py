#!/usr/bin/env python3
"""nav_recon.py -- independent reconciliation of the C++ NAV replay (TRAIN only; diagnostic).

Question: is `atx-equity-strategy-targets nav` (strategy_nav_replay.cpp) correct and the v6
blend simply loses at daily full rebalancing, or is there a defect (timing, marks, guard,
stale/write-off, scale, sign)?

What it recomputes, from the TRAIN role payload + TRAIN saved blend only:
  * W[d]  baseline-v1 desired target at decision d (tied centered rank over blend members,
          demeaned, gross 1) -- same operations as detail::desired_target.
  * miniNAV: a numpy re-implementation of the NAV book (MARK -> EXECUTE -> DECIDE, decide d,
          fill at close d+1, first return row d+2, decision-NAV dollar orders, stale carry K,
          write-off, borrow on pre-mark shorts x calendar days/365, flat or sqrt+cap costs),
          with a guard policy for intervals flagged by the replay guard
          (|adj log| > 1.5 or > |raw log| + .10):
              adj  = realize the adjusted return (what the C++ NAV declares and does)
              raw  = realize the raw-close return instead
              zero = realize nothing (mark jumps to the new close)
  * fixed-weight series  g[t] = sum_i W[t-L, i] * r[t, i],  r[t] = close[t]/close[t-1]-1,
          L = 2 (NAV timing == study timing fwd[d] = r[d+2]), L = 1 / 3 (off-by-one probes),
          guarded cells excluded (study / target-proxy convention) or realized.
  * proxy c5 f.25: the target-replay observed-component at its default cadence 5 /
          fraction .25 (drift-free partial weights), for the earlier "+.36" number.
  * a close/raw factor-jump scan (d log(close/raw) between adjacent present sessions).

Reads ONLY: <role>/manifest.json + payloads, <blend>/<prefix>_combined.*, <nav>/recipe.json +
daily_<scenario>.csv. Refuses any session >= 2023-01-01. numpy only; ~0.5 GB peak; ~10-40 s.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys
import time

import numpy as np

DAY = 86_400_000_000_000
TRAIN_END_NS = 1_672_531_200_000_000_000  # 2023-01-01T00:00Z: TRAIN only
POOL = 'C:/atx-wt/pool-2'
DEF_ROLE = f'{POOL}/build-equity/recent-fast-train-2020-2022-v1'
DEF_BLEND = f'{POOL}/build-equity/recent-fast-ic-train-artifact-v6'
DEF_NAV = f'{POOL}/build-equity/mega-nav-train-v6-c1'
S1, S2 = 'linear-6bps-stale5-v1', 'modeled-1bn-stale5-v1'


# ----------------------------------------------------------------------------- utilities
def peak_rss_mb() -> float:
    try:
        if os.name == 'nt':
            import ctypes
            import ctypes.wintypes as wt

            class PMC(ctypes.Structure):
                _fields_ = [('cb', wt.DWORD), ('PageFaultCount', wt.DWORD),
                            ('PeakWorkingSetSize', ctypes.c_size_t),
                            ('WorkingSetSize', ctypes.c_size_t),
                            ('QuotaPeakPagedPoolUsage', ctypes.c_size_t),
                            ('QuotaPagedPoolUsage', ctypes.c_size_t),
                            ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t),
                            ('QuotaNonPagedPoolUsage', ctypes.c_size_t),
                            ('PagefileUsage', ctypes.c_size_t),
                            ('PeakPagefileUsage', ctypes.c_size_t)]
            pmc = PMC(); pmc.cb = ctypes.sizeof(PMC)
            k32, psapi = ctypes.windll.kernel32, ctypes.windll.psapi
            k32.GetCurrentProcess.restype = wt.HANDLE
            psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.POINTER(PMC), wt.DWORD]
            psapi.GetProcessMemoryInfo(k32.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb)
            return pmc.PeakWorkingSetSize / 2**20
        import resource
        return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024
    except Exception:  # diagnostic only
        return float('nan')


def date_of(ns) -> str:
    return str(np.datetime64(int(ns), 'ns').astype('datetime64[D]'))


def fromfile(path, dtype, shape):
    a = np.fromfile(path, dtype=dtype)
    need = int(np.prod(shape))
    if a.size != need:
        sys.exit(f'nav_recon: {path}: {a.size} elements, expected {need}')
    return a.reshape(shape)


# ----------------------------------------------------------------------------- inputs
def load(args):
    rm = json.load(open(os.path.join(args.role, 'manifest.json')))
    D, N = int(rm['dates']), int(rm['instruments'])
    bj = json.load(open(os.path.join(args.blend, f'{args.prefix}_combined.json')))
    if bj.get('role') != 'train' or int(bj['dates']) != D or int(bj['instruments']) != N:
        sys.exit('nav_recon: blend must be the TRAIN blend on the same role axes')
    if bj.get('role_manifest_sha256') is None:
        sys.exit('nav_recon: blend manifest lacks role binding')
    begin, end = int(bj['score_begin']), int(bj['score_end'])
    rp = lambda f: os.path.join(args.role, f)
    bp = lambda f: os.path.join(args.blend, f'{args.prefix}_combined{f}')
    sess = fromfile(rp('sessions.i64'), '<i8', (D,))
    if not np.array_equal(sess, fromfile(bp('_sessions.i64'), '<i8', (D,))) or \
       not np.array_equal(fromfile(rp('ids.u64'), '<u8', (N,)), fromfile(bp('_ids.u64'), '<u8', (N,))):
        sys.exit('nav_recon: role/blend axes differ')
    if sess[-1] >= TRAIN_END_NS:
        sys.exit('nav_recon: refusing non-TRAIN sessions (>= 2023-01-01)')
    ctx = dict(D=D, N=N, begin=begin, end=end, sess=sess,
               ids=fromfile(rp('ids.u64'), '<u8', (N,)),
               close=fromfile(rp('close.f64'), '<f8', (D, N)),
               raw=fromfile(rp('raw_close.f64'), '<f8', (D, N)),
               present=fromfile(rp('present.u8'), 'u1', (D, N)).astype(bool),
               member=fromfile(bp('_member.u8'), 'u1', (D, N)).astype(bool))
    blend = fromfile(bp('.f64'), '<f8', (D, N))
    if np.any(ctx['member'] & ~ctx['present']) or np.any(~np.isfinite(blend[ctx['member']])):
        sys.exit('nav_recon: blend member must be present with a finite signal')
    ctx['W'] = build_targets(blend, ctx['member'], begin, end)
    del blend
    ctx['volume_path'] = rp('volume.f64')
    return ctx


def desired_target(sig: np.ndarray, mem: np.ndarray) -> np.ndarray:
    """detail::desired_target: centered tied rank over members, demeaned, gross 1."""
    out = np.zeros(sig.shape[0])
    idx = np.flatnonzero(mem)
    n = idx.size
    if n < 2:
        return out
    _, inv, cnt = np.unique(sig[idx], return_inverse=True, return_counts=True)
    start = np.cumsum(cnt) - cnt
    rank = (start + (start + cnt - 1)) / (2.0 * (n - 1)) - 0.5
    x = rank[inv.reshape(-1)]
    x = x - x.sum() / n
    g = np.abs(x).sum()
    if g > 0:
        x = x / g
    out[idx] = x
    return out


def build_targets(blend, member, begin, end):
    W = np.zeros((end - 2 - begin, blend.shape[1]))  # decisions the NAV scores: [begin, end-2)
    for j in range(W.shape[0]):
        W[j] = desired_target(blend[begin + j], member[begin + j])
    return W


def row_pass(ctx, need_liquidity: bool):
    """One pass over sessions: adjacent returns, guard mask, factor-jump scan, and the
    cumulative sums the sqrt-cost liquidity rows need ([t-w, t) windows)."""
    D, N, begin, end = ctx['D'], ctx['N'], ctx['begin'], ctx['end']
    close, raw, present, W = ctx['close'], ctx['raw'], ctx['present'], ctx['W']
    r_all = np.full((D, N), np.nan)
    g_all = np.zeros((D, N), bool)
    scan = np.zeros((D, 6))  # nbig, nsmall, |w|big, |w|small, unflagged factor pnl, guarded w*r_adj
    vol = cN = cR = cR2 = cDV = None
    if need_liquidity:
        vol = fromfile(ctx['volume_path'], '<f8', (D, N))
        cDV, cR, cR2 = (np.zeros((D + 1, N)) for _ in range(3))
        cN = np.zeros((D + 1, N), np.int32)
    with np.errstate(divide='ignore', invalid='ignore'):
        for t in range(D):
            if need_liquidity:
                cDV[t + 1] = cDV[t] + np.where(present[t], raw[t] * vol[t], 0.0)
            if t == 0:
                if need_liquidity:
                    cN[1], cR[1], cR2[1] = cN[0], cR[0], cR2[0]
                continue
            ok = present[t] & present[t - 1]
            al = np.log(close[t]) - np.log(close[t - 1])
            rl = np.log(raw[t]) - np.log(raw[t - 1])
            gd = ok & (~np.isfinite(al) | (np.abs(al) > 1.5) | (np.abs(al) > np.abs(rl) + 0.10))
            r = np.where(ok, close[t] / close[t - 1] - 1.0, np.nan)
            r_all[t], g_all[t] = r, gd
            if need_liquidity:
                pv = ok & ~gd
                rp = np.where(pv, r, 0.0)
                cN[t + 1] = cN[t] + pv
                cR[t + 1] = cR[t] + rp
                cR2[t + 1] = cR2[t] + rp * rp
            dlf = al - rl
            big = ok & (np.abs(dlf) > 0.10) & (np.abs(al) > np.abs(rl) + 0.10)
            small = ok & (np.abs(dlf) > 0.01) & (np.abs(dlf) <= 0.10) & (np.abs(al) > np.abs(rl) + 0.01)
            scan[t, 0], scan[t, 1] = big.sum(), small.sum()
            if begin + 2 <= t < end:
                w = W[t - 2 - begin]
                scan[t, 2] = np.abs(w[big]).sum()
                scan[t, 3] = np.abs(w[small]).sum()
                rr = raw[t] / raw[t - 1] - 1.0
                clean = ok & ~gd
                scan[t, 4] = np.sum(np.where(clean, w * (r - rr), 0.0))
                scan[t, 5] = np.sum(np.where(gd, w * r, 0.0))
    ctx.update(r_all=r_all, g_all=g_all, scan=scan, cN=cN, cR=cR, cR2=cR2, cDV=cDV)
    del vol


# ----------------------------------------------------------------------------- mini NAV
def mini_nav(ctx, sc: dict, policy: str, prm: dict):
    """numpy mirror of strategy_nav_replay.cpp run_book for baseline-target-v1."""
    close, raw, present, member, W, sess = (ctx[k] for k in
                                            ('close', 'raw', 'present', 'member', 'W', 'sess'))
    begin, end, N = ctx['begin'], ctx['end'], ctx['N']
    cad, frac, nav0 = int(prm['cadence']), float(prm['trade_fraction']), float(prm['initial_nav'])
    win, mvp = int(prm['liquidity_window']), int(prm['min_vol_pairs'])
    K = int(sc['stale_exit_sessions'])
    eta_l, eta_s = float(sc['terminal_haircut_long']), float(sc['terminal_haircut_short'])
    brate = float(sc['annual_borrow_bps']) * 1e-4
    sqrt_model = sc['cost_rule'] == 'sqrt-impact-v1'
    flat_rate = float(sc['flat_bps']) * 1e-4
    lin = (float(sc['half_spread_bps']) + float(sc['commission_bps'])) * 1e-4
    y, dl, fb = float(sc['impact_y']), float(sc['impact_delta']), float(sc['fallback_daily_vol'])
    mp = sc['max_participation']
    mp = math.inf if isinstance(mp, str) else float(mp)
    if sqrt_model and ctx.get('cDV') is None:
        raise RuntimeError('sqrt scenario needs liquidity sums')
    h = np.zeros(N); order = np.zeros(N)
    mark = np.full(N, np.nan); rmark = np.full(N, np.nan)
    absent = np.zeros(N, np.int64)
    active = np.zeros(N, bool); woff = np.zeros(N, bool)
    cash = nav_pre = nav_post = nav0
    T = end - begin
    out = {k: np.full(T, np.nan) for k in ('gross', 'net', 'turn', 'gl', 'nl')}
    guarded = np.zeros(T, np.int64)
    max_err, des = 0.0, None
    with np.errstate(divide='ignore', invalid='ignore'):
        for t in range(begin, end):
            j = t - begin
            if t > begin:  # MARK
                bfac = brate * float((sess[t] - sess[t - 1]) // DAY) / 365.0
                borrow = float(np.sum(-h[h < 0] * bfac))
                pres = present[t]
                held = h != 0
                flat = ~held
                fp = flat & pres
                back = fp & woff
                woff[back] = False; absent[back] = 0
                absent[flat & ~pres & woff] += 1
                mark[fp] = close[t, fp]; rmark[fp] = raw[t, fp]
                ha = held & ~pres
                absent[ha] += 1
                wo = ha & (absent >= K)
                writeoff = 0.0
                if wo.any():
                    hw = h[wo]
                    eta = np.where(hw > 0, eta_l, eta_s)
                    writeoff = float(np.sum(hw * eta)); cash += float(np.sum(hw * (1.0 + eta)))
                    h[wo] = 0.0; order[wo] = 0.0; active[wo] = False; woff[wo] = True
                ix = np.flatnonzero(held & pres)
                c_t, r_t = close[t, ix], raw[t, ix]
                al = np.log(c_t) - np.log(mark[ix])
                rl = np.log(r_t) - np.log(rmark[ix])
                gd = ~np.isfinite(al) | (np.abs(al) > 1.5) | (np.abs(al) > np.abs(rl) + 0.10)
                ratio = c_t / mark[ix]
                if policy == 'raw':
                    ratio = np.where(gd, r_t / rmark[ix], ratio)
                elif policy == 'zero':
                    ratio = np.where(gd, 1.0, ratio)
                hi = h[ix]
                nxt = hi * ratio
                pnl = float(np.sum(nxt - hi))
                absent[ix] = 0
                h[ix] = nxt; mark[ix] = c_t; rmark[ix] = r_t
                guarded[j] = int(gd.sum())
                cash -= borrow
                base = nav_pre
                nav_pre = nav_post + pnl + writeoff - borrow
                out['gross'][j] = (pnl + writeoff) / base
                out['net'][j] = nav_pre / base - 1.0
            if t > begin and t + 2 <= end:  # EXECUTE working orders at close t
                ai = np.flatnonzero(active & present[t])
                req = order[ai] - h[ai]
                zero = req == 0
                active[ai[zero]] = False
                ai, req = ai[~zero], req[~zero]
                if sqrt_model:
                    lo = t - win if t > win else 0
                    adv = (ctx['cDV'][t, ai] - ctx['cDV'][lo, ai]) / win
                    n = (ctx['cN'][t, ai] - ctx['cN'][lo, ai]).astype(np.float64)
                    s1 = ctx['cR'][t, ai] - ctx['cR'][lo, ai]
                    s2 = ctx['cR2'][t, ai] - ctx['cR2'][lo, ai]
                    m2 = np.maximum(s2 - s1 * s1 / np.maximum(n, 1.0), 0.0)
                    sig = np.where(n < mvp, fb, np.sqrt(m2 / np.maximum(n - 1.0, 1.0)))
                    usable = np.isfinite(adv) & (adv > 0)
                    a = np.abs(req)
                    fill = np.where(usable, np.minimum(a, mp * adv), 0.0)
                    part_rate = np.where(usable, fill / np.where(usable, adv, 1.0), 0.0)
                    cst = np.where(usable, fill * (lin + y * sig * np.power(part_rate, dl)), 0.0)
                    filled = np.where(usable, np.where(fill == a, req, np.copysign(fill, req)), 0.0)
                else:
                    filled, cst = req.copy(), np.abs(req) * flat_rate
                nz = filled != 0
                comp = nz & (filled == req)
                part = nz & ~comp
                h[ai[comp]] = order[ai[comp]]; active[ai[comp]] = False
                h[ai[part]] += filled[part]
                cost = float(np.sum(cst[nz]))
                cash -= float(np.sum(filled[nz])); cash -= cost
                out['turn'][j] = float(np.sum(np.abs(filled[nz]))) / nav_pre
                nav_post = nav_pre - cost
            else:
                nav_post = nav_pre
            if t + 2 < end:  # DECIDE at t
                reb = (t - begin) % cad == 0
                cur = h / nav_post
                mem = member[t]
                if reb:
                    des = W[j]
                planned = np.where(mem, (cur + frac * (des - cur)) if reb else cur, 0.0)
                chg = planned != cur
                order[chg] = planned[chg] * nav_post; active[chg] = True
                active[~chg & (reb | ~mem)] = False
            lg, sh = float(np.sum(h[h > 0])), float(-np.sum(h[h < 0]))
            out['gl'][j], out['nl'][j] = (lg + sh) / nav_post, (lg - sh) / nav_post
            max_err = max(max_err, abs(cash + lg - sh - nav_post) / nav_post)
    out['guarded'], out['max_err'] = guarded, max_err
    return out


# ----------------------------------------------------------------------------- series
def fixed_series(ctx, lag: int, guard: str):
    begin, end, W, r, g = ctx['begin'], ctx['end'], ctx['W'], ctx['r_all'], ctx['g_all']
    out = np.full(end - begin, np.nan)
    for t in range(begin + lag, end):
        d = t - lag
        if d - begin >= W.shape[0]:
            continue
        rt = np.where(np.isfinite(r[t]), r[t], 0.0)
        if guard == 'excl':
            rt = np.where(g[t], 0.0, rt)
        out[t - begin] = float(W[d - begin] @ rt)
    return out


def proxy_series(ctx, cadence=5, fraction=0.25):
    """target replay observed component: drift-free partial weights, guarded/missing excluded."""
    begin, end, W, r, g, mem = (ctx[k] for k in ('begin', 'end', 'W', 'r_all', 'g_all', 'member'))
    out = np.full(end - begin, np.nan)
    cur = np.zeros(ctx['N'])
    for j in range(W.shape[0]):
        d = begin + j
        if j % cadence == 0:
            cur = np.where(mem[d], cur + fraction * (W[j] - cur), 0.0)
        else:
            cur = np.where(mem[d], cur, 0.0)
        t = d + 2
        rt = np.where(np.isfinite(r[t]) & ~g[t], r[t], 0.0)
        out[t - begin] = float(cur @ rt)
    return out


def read_daily(path, begin, end):
    out = {k: np.full(end - begin, np.nan) for k in ('gross', 'net', 'turn', 'gl', 'nl')}
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            j = int(row['session_index']) - begin
            if not 0 <= j < end - begin:
                continue
            if row['return_observation'] == '1':
                out['gross'][j], out['net'][j] = float(row['gross_return']), float(row['net_return'])
            if row['executed'] == '1':
                out['turn'][j] = float(row['one_way_turnover'])
            out['gl'][j], out['nl'][j] = float(row['gross_leverage']), float(row['net_leverage'])
    return out


def stats(x, mkt):
    mu, sd = x.mean() * 252, x.std(ddof=1) * math.sqrt(252)
    mk = mkt - mkt.mean()
    beta = float(((x - x.mean()) * mk).sum() / (mk * mk).sum())
    return mu, sd, (mu / sd if sd > 0 else float('nan')), beta


def corr(a, b):
    return float(np.corrcoef(a, b)[0, 1]) if a.std() > 0 and b.std() > 0 else float('nan')


# ----------------------------------------------------------------------------- main
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--role', default=DEF_ROLE, help='TRAIN role directory (manifest.json)')
    ap.add_argument('--blend', default=DEF_BLEND, help='directory holding <prefix>_combined.*')
    ap.add_argument('--prefix', default='train')
    ap.add_argument('--nav', default=DEF_NAV, help='NAV output dir (recipe.json, daily_*.csv)')
    ap.add_argument('--no-s2', action='store_true', help='skip the sqrt/cap S2 mirror (no volume load)')
    ap.add_argument('--top', type=int, default=10)
    ap.add_argument('--f-window', default='2021-01-04:2021-04-30',
                    help='inclusive date window for the S2-S1 attribution (section F)')
    a = ap.parse_args(argv)
    t0 = time.perf_counter()
    recipe = json.load(open(os.path.join(a.nav, 'recipe.json')))
    if recipe.get('rule') != 'baseline-target-v1':
        sys.exit('nav_recon: only baseline-target-v1 is mirrored')
    scen = {s['id']: s for s in recipe['scenarios']}
    do_s2 = not a.no_s2 and S2 in scen
    ctx = load(a)
    row_pass(ctx, do_s2)
    begin, end, sess = ctx['begin'], ctx['end'], ctx['sess']
    rows = np.arange(begin + 2, end)          # the NAV's 754 return rows
    J = rows - begin
    dates = [date_of(sess[t]) for t in rows]
    r, g, mem = ctx['r_all'], ctx['g_all'], ctx['member']
    mkt = np.array([np.nanmean(np.where(mem[t - 1] & ~g[t], r[t], np.nan)) for t in rows])

    nav1 = read_daily(os.path.join(a.nav, f'daily_{S1}.csv'), begin, end)
    ref = nav1['gross'][J]
    worst = int(np.argmax(np.abs(ref)))
    ex = np.ones(ref.size, bool); ex[worst] = False
    series, mirrors = {}, {}
    for pol in ('adj', 'raw', 'zero'):
        mirrors[('S1', pol)] = mini_nav(ctx, scen[S1], pol, recipe)
    nav2 = None
    if do_s2:
        nav2 = read_daily(os.path.join(a.nav, f'daily_{S2}.csv'), begin, end)
        for pol in ('adj', 'raw'):
            mirrors[('S2', pol)] = mini_nav(ctx, scen[S2], pol, recipe)

    print(f'nav_recon  TRAIN rows={rows.size} {dates[0]}..{dates[-1]} begin={begin} end={end} '
          f'N={ctx["N"]} rule=baseline c{recipe["cadence"]} f{recipe["trade_fraction"]}  '
          f'worst NAV S1 day={dates[worst]} ({ref[worst]:+.4f})')
    print('A. reproduction of the C++ NAV by the numpy mirror (guard=adj == declared C++ policy)')
    print('   pair                         corr   TE_ann   max|diff| @date      turn mean mirror/NAV  net max|diff|  cash-book err')
    for tag, nav in (('S1', nav1), ('S2', nav2)):
        if nav is None:
            continue
        m = mirrors[(tag, 'adj')]
        x, y = m['gross'][J], nav['gross'][J]
        d = x - y; k = int(np.argmax(np.abs(d)))
        tm = np.nanmean(m['turn'][2:]); tn = np.nanmean(nav['turn'][2:])
        nd = float(np.max(np.abs(m['net'][J] - nav['net'][J])))
        print(f'   mirror {tag} adj vs NAV {tag} gross  {corr(x, y):.6f} {d.std(ddof=1)*math.sqrt(252):.2e} '
              f'{abs(d[k]):.2e} {dates[k]}   {tm:.4f}/{tn:.4f}      {nd:.2e}       {m["max_err"]:.1e}')
    print(f'B. series over the {rows.size} NAV rows      ann_mu  ann_vol     SR   beta  corrNAV1 (ex worst)  TE_NAV1 (ex worst)')

    def line(label, x):
        mu, sd, sr, b = stats(x, mkt)
        series[label] = x
        te = (x - ref).std(ddof=1) * math.sqrt(252); tex = (x[ex] - ref[ex]).std(ddof=1) * math.sqrt(252)
        print(f'   {label:37s} {mu:+.4f}  {sd:.4f}  {sr:+.3f} {b:+.3f}   {corr(x, ref):+.3f}   {corr(x[ex], ref[ex]):+.3f}'
              f'    {te:.4f}  {tex:.4f}')

    line('NAV S1 gross (csv)', ref)
    mu, sd, sr, b = stats(ref[ex], mkt[ex])
    print(f'   {"NAV S1 gross ex worst day":37s} {mu:+.4f}  {sd:.4f}  {sr:+.3f} {b:+.3f}')
    line('mirror S1 guard=adj', mirrors[('S1', 'adj')]['gross'][J])
    line('mirror S1 guard=raw', mirrors[('S1', 'raw')]['gross'][J])
    line('mirror S1 guard=zero', mirrors[('S1', 'zero')]['gross'][J])
    line('fixed W lag2 guarded realized', fixed_series(ctx, 2, 'adj')[J])
    line('fixed W lag2 guard-excl (study c1f1)', fixed_series(ctx, 2, 'excl')[J])
    line('fixed W lag1 guard-excl (same-close)', np.nan_to_num(fixed_series(ctx, 1, 'excl')[J]))
    line('fixed W lag3 guard-excl (late)', np.nan_to_num(fixed_series(ctx, 3, 'excl')[J]))
    line('proxy c5 f.25 guard-excl (T replay)', proxy_series(ctx)[J])
    if nav2 is not None:
        line('NAV S2 gross (csv)', nav2['gross'][J])
        line('mirror S2 guard=adj', mirrors[('S2', 'adj')]['gross'][J])
        line('mirror S2 guard=raw', mirrors[('S2', 'raw')]['gross'][J])

    print('C. by calendar year: ann_mu / SR')
    cols = ['NAV S1 gross (csv)', 'mirror S1 guard=raw', 'fixed W lag2 guard-excl (study c1f1)',
            'proxy c5 f.25 guard-excl (T replay)'] + (['NAV S2 gross (csv)', 'mirror S2 guard=raw'] if nav2 is not None else [])
    print('   year  ' + ' | '.join(f'{c[:18]:>18s}' for c in cols))
    years = np.array([int(d[:4]) for d in dates])
    for yv in sorted(set(years)):
        k = years == yv
        cells = []
        for c in cols:
            v = series[c][k]
            srv = v.mean() / v.std(ddof=1) * math.sqrt(252) if v.size > 2 and v.std() > 0 else float('nan')
            cells.append(f'{v.mean()*252:+.3f}/{srv:+.2f}'.rjust(18))
        print(f'   {yv}  ' + ' | '.join(cells))

    study = series['fixed W lag2 guard-excl (study c1f1)']
    madj = series['mirror S1 guard=adj']
    diff = ref - study
    print(f'D. top-{a.top} days |NAV S1 - study c1f1|: NAV mirror study diff lev(t-1) nGuardHeld | '
          'top names id w(bp) r_adj r_raw')
    lev = nav1['gl']
    for k in np.argsort(-np.abs(diff))[:a.top]:
        t = rows[k]; w = ctx['W'][t - 2 - begin]
        ra = np.where(np.isfinite(r[t]), r[t], 0.0)
        contrib = np.where(g[t], w * ra, 0.0)
        tag = 'guard'
        if not np.any(contrib):
            contrib, tag = w * ra, 'top'
        names = []
        for i in np.argsort(-np.abs(contrib))[:2]:
            rr = ctx['raw'][t, i] / ctx['raw'][t - 1, i] - 1.0
            names.append(f'{ctx["ids"][i]} {w[i]*1e4:+.1f} {ra[i]:+.3f} {rr:+.3f}')
        print(f'   {dates[k]} {ref[k]:+.4f} {madj[k]:+.4f} {study[k]:+.4f} {diff[k]:+.4f} '
              f'{lev[t - 1 - begin]:.2f} {mirrors[("S1", "adj")]["guarded"][t - begin]:3d} | {tag}: ' + '; '.join(names))

    sc = ctx['scan']
    print('E. close/raw factor-jump scan (all present-adjacent names; big: |dlog f|>.10 & |adj|>|raw|+.10,')
    print('   small: .01<|dlog f|<=.10 & |adj|>|raw|+.01; |w| = held target weight t-2; fpnl = unflagged factor P&L)')
    first_of_year = [t for t in range(1, ctx['D']) if date_of(sess[t])[:4] != date_of(sess[t - 1])[:4]]
    top_big = [t for t in np.argsort(-sc[:, 0])[:5] if sc[t, 0] > 0]
    for t in sorted(set(top_big) | set(first_of_year)):
        inwin = begin + 2 <= t < end
        tail = (f' |w|big {sc[t, 2]:.4f} |w|small {sc[t, 3]:.4f} fpnl {sc[t, 4]:+.5f} guarded w*r_adj {sc[t, 5]:+.4f}'
                if inwin else ' (outside return rows)')
        print(f'   {date_of(sess[t])} big {int(sc[t, 0]):4d} small {int(sc[t, 1]):4d}{tail}'
              + ('  <year start' if t in first_of_year else ''))
    fp = sc[rows, 4]
    kk = np.argsort(-np.abs(fp))[:3]
    print(f'   unflagged factor P&L over rows: sum {fp.sum():+.4f}, top days '
          + ', '.join(f'{dates[k]} {fp[k]:+.4f}' for k in kk)
          + f'; median small/day {np.median(sc[rows, 1]):.0f}')
    anyb = sc[:, 0] > 0
    print(f'   sessions with >=1 big jump: {int(anyb.sum())} of {ctx["D"]} (in return rows {int(anyb[rows].sum())}); '
          f'big cells total {int(sc[:, 0].sum())}, excluding the top session {int(sc[:, 0].sum() - sc[:, 0].max())}')

    if nav2 is not None:
        dd = nav2['gross'][J] - ref
        w0, w1 = a.f_window.split(':')
        win = np.array([(w0 <= d <= w1) for d in dates])
        nl2 = nav2['nl'][J - 1]
        x = (nav2['nl'][J - 1] - nav1['nl'][J - 1]) * mkt
        print(f'F. S2-S1 gross (csv): sum {dd.sum():+.4f}, in {w0}..{w1} {dd[win].sum():+.4f}; '
              f'S2 net lev (t-1) window min {nl2[win].min():+.3f} mean {nl2[win].mean():+.3f}, '
              f'outside mean {nl2[~win].mean():+.3f}; corr(diff, dNetLev*mkt) {corr(dd, x):+.3f}')
        for label, v in (('S1', ref), ('S2', nav2['gross'][J])):
            k = ~win & ex
            mu, sd, sr, b = stats(v[k], mkt[k])
            print(f'   NAV {label} gross outside window & ex worst day: ann_mu {mu:+.4f} vol {sd:.4f} SR {sr:+.3f}')
    print(f'runtime {time.perf_counter() - t0:.1f}s  peak RSS {peak_rss_mb():.0f} MB')


if __name__ == '__main__':
    main()
