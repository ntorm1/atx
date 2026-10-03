"""Lane YOPS (platform v8): the constructs of Kakushadze's "101 Formulaic Alphas" the op catalog lacked, as DSL ops.

Run from the repository root (no arguments; synthetic data only, reads no data payload):
    "C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-v8-20260929/yops_check.py

1. MIRROR: lane XSIG's offline compiler mirror (xsig_check.py) reads the operator table from registry.cpp, which now
   carries the formulaic rows. This file teaches its parser the as-of family (window d and lag j are peeled
   hyperparameters; lookback (d - 1) + j + child), group_delay (a Group shift; lookback d + child) and group_sum's
   Group argument, then prints bars / slots / nodes / bytes / SHA-256 of every frozen string.
2. INTERPRETER: lane XWQ's numpy interpreter of the house semantics (xwq_check.py) gains the seven new ops, each
   written from its registered definition (task-YOPS-report.md section 2), independently of the C++ kernels.
3. ORACLE: each printed formula (arXiv:1601.00991 appendix A.1, verbatim in xwq_check.PRINTED) implemented directly in
   numpy and evaluated at every day t on the history re-adjusted as of t (reading R1 of task-XWQ-report.md: every
   price of every past session is adjusted for the splits and dividends up to t), on a synthetic world with splits,
   dividends, a different factor anchor per line, listings and delistings inside the sample, a halted session and
   industry reclassifications.
4. CHECKS: every frozen string equals its oracle cell for cell (NaN pattern included), is free of the factor anchor,
   differs from the point-in-time reading (each past cross-section on that day's own prices), and every planted
   error fails. #68 is verified in its unrolled form and reported over the DSL byte limit; #92 stays blocked.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import sys

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import xwq_check as xw  # noqa: E402  (lane XWQ: printed formulas, synthetic world, interpreter)

xs = xw.xs  # lane XSIG's compiler mirror (already extended with open_adj / high_adj / low_adj by xwq_check)

# ------------------------------------------------------------------------------------------------ the new ops
ASOF_OUTER = {  # registry name -> the house time-series op applied to the as-of rank window
    "asof_rank_ts_rank": "ts_rank", "asof_rank_ts_min": "ts_min", "asof_rank_decay_linear": "decay_linear",
    "asof_rank_correlation": "correlation", "asof_rank_covariance": "covariance"}
ASOF_PAIR = {"asof_rank_correlation", "asof_rank_covariance"}
NEW_OPS = {**{n: "AsofRank" for n in ASOF_OUTER}, "group_delay": "GroupDelay", "group_sum": "CsSumG"}
REGISTERED = {  # name -> (min arity, max arity, OpCode, out dtype, peeled hparams): what registry.cpp must carry
    "group_sum": (2, 2, "CsSumG", "F64", 0), "group_delay": (2, 2, "GroupDelay", "Group", 0),
    "asof_rank_ts_rank": (4, 4, "AsofRankTsRank", "F64", 2), "asof_rank_ts_min": (4, 4, "AsofRankTsMin", "F64", 2),
    "asof_rank_decay_linear": (4, 4, "AsofRankDecayLinear", "F64", 2),
    "asof_rank_correlation": (5, 5, "AsofRankCorr", "F64", 2), "asof_rank_covariance": (5, 5, "AsofRankCov", "F64", 2)}
xs.GROUP_ARG.add("CsSumG")
xs.FIELDS |= {"grp_ff49", "me_company"}


class YParser(xs.Parser):
    """The XSIG mirror parser plus the as-of family and group_delay (group_sum goes through the generic path)."""

    def primary(self):
        tok = self.peek()
        nxt = self.t[self.i + 1] if self.i + 1 < len(self.t) else None
        if nxt == "(" and (tok in ASOF_OUTER or tok == "group_delay"):
            return self._yops_call()
        return super().primary()

    def _args(self):
        self.take("(")
        args = [self.expr()]
        while self.peek() == ",":
            self.take(",")
            args.append(self.expr())
        self.take(")")
        return args

    def _yops_call(self):
        tok = self.take()
        args = self._args()
        lo, hi, _, _, nh = REGISTERED[tok]
        row = xs.OPS.get(tok)
        if row is None or (row["lo"], row["hi"], row["nh"]) != (lo, hi, nh):
            raise ValueError(f"{tok}: registry.cpp row missing or different: {row}")
        if not lo <= len(args) <= hi:
            raise ValueError(f"arity {len(args)} for {tok}")
        if tok == "group_delay":
            g, w = args
            if g["dt"] != "group" or "num" not in w or w["num"] < 1 or w["num"] != int(w["num"]):
                raise ValueError("group_delay needs (Group, positive integer literal)")
            return xs.node(("call", tok, ()), [g, w], "group", g["lb"] + int(w["num"]))
        ops, hp = args[:len(args) - nh], args[len(args) - nh:]
        if any("num" not in h or h["num"] != int(h["num"]) for h in hp) or hp[0]["num"] < 1 or hp[1]["num"] < 0:
            raise ValueError(f"{tok}: window d >= 1 and lag j >= 0 must be integer literals")
        if any(a["dt"] != "f64" or "num" in a for a in ops):
            raise ValueError(f"{tok}: operands must be numeric vectors")
        d, j = int(hp[0]["num"]), int(hp[1]["num"])
        return xs.node(("call", tok, (float(d), float(j))), ops, "f64", max(a["lb"] for a in ops) + d - 1 + j)


xs.Parser = YParser  # xs.parse / xs.static (and so xwq_check's helpers) now read the new ops


def rank2d(x):
    """Row-wise average-tie percentile over the non-NaN cells (house rank: (less + (eq - 1) / 2) / (n - 1); one cell
    0.5)."""
    x = np.asarray(x, dtype=float)
    ok = ~np.isnan(x)
    with np.errstate(invalid="ignore"):
        less = ((x[:, None, :] < x[:, :, None]) & ok[:, None, :]).sum(axis=2)
        eq = ((x[:, None, :] == x[:, :, None]) & ok[:, None, :]).sum(axis=2)
    n = ok.sum(axis=1, keepdims=True)
    out = np.where(n == 1, 0.5, (less + (eq - 1) / 2.0) / np.maximum(n - 1, 1))
    return np.where(ok, out, np.nan)


def asof_factor(w, t, lo):
    """The as-of rebase factor of row t: per line the latest non-NaN w in rows [lo, t] (ts_backfill)."""
    blk = w[lo:t + 1][::-1]
    has = ~np.isnan(blk)
    first = np.argmax(has, axis=0)
    return np.where(has.any(axis=0), blk[first, np.arange(blk.shape[1])], np.nan)


def asof_rank(name, x, w, d, j, y=None):
    """asof_rank_<outer>(x, w, [y,] d, j): at row t, the house <outer> over sessions s in [t-j-d+1, t-j] of the
    cross-sectional rank of x[s] * w*[t], w*[t] = the latest non-NaN w in [t-j-d+1, t]; NaN while the window does
    not fit. Outer ops follow ts_value_at / ts_pair_at (any NaN in a window -> NaN; corr flat -> NaN)."""
    T, _ = x.shape
    out = np.full(x.shape, np.nan)
    outer = ASOF_OUTER[name]
    for t in range(d + j - 1, T):
        s0 = t - j - d + 1
        R = rank2d(x[s0:s0 + d] * asof_factor(w, t, s0))
        bad = np.isnan(R).any(axis=0)
        with np.errstate(invalid="ignore", divide="ignore"):
            if outer in ("correlation", "covariance"):
                Y = y[s0:s0 + d]
                bad |= np.isnan(Y).any(axis=0) | (d < 2)
                mr, my = R.mean(axis=0), Y.mean(axis=0)
                sab = ((R - mr) * (Y - my)).sum(axis=0)
                if outer == "covariance":
                    val = sab / max(d - 1, 1)
                else:
                    saa, sbb = ((R - mr) ** 2).sum(axis=0), ((Y - my) ** 2).sum(axis=0)
                    flat = xw._flat(saa, mr, d) | xw._flat(sbb, my, d)
                    den = np.sqrt(saa * sbb)
                    bad |= flat | (den == 0)
                    val = sab / den
            elif outer == "ts_rank":
                val = 0.5 * np.ones(R.shape[1]) if d == 1 else xw._tsrank(R)
            elif outer == "ts_min":
                val = R.min(axis=0)
            else:
                val = xw._decay(R)
        out[t] = np.where(bad, np.nan, val)
    return out


def group_agg(x, g, kind):
    """group_sum / group_mean / group_count over the valid set (x and the label non-NaN), broadcast to each valid
    member; the sum runs in ascending instrument order (the kernels' order)."""
    out = np.full(x.shape, np.nan)
    for t in range(x.shape[0]):
        ok = ~np.isnan(x[t]) & ~np.isnan(g[t])
        for lab in np.unique(g[t][ok]):
            m = ok & (g[t] == lab)
            acc = 0.0
            for i in np.flatnonzero(m):
                acc += x[t, i]
            out[t, m] = {"group_sum": acc, "group_mean": acc / m.sum(), "group_count": float(m.sum())}[kind]
    return out


def shift(x, d):
    out = np.full(x.shape, np.nan)
    if d < x.shape[0]:
        out[d:] = x[:x.shape[0] - d]
    return out


def ts_backfill(x, d):
    out = np.full(x.shape, np.nan)
    for t in range(x.shape[0]):
        out[t] = asof_factor(x, t, max(0, t - d + 1))
    return out


_base_ev = xw.ev


EXTRA_OPS = {"ts_backfill", "group_mean", "group_count"}   # house ops xwq_check's interpreter does not carry


def ev(node, env):
    k = node["key"]
    if k[0] != "call" or (k[1] not in NEW_OPS and k[1] not in EXTRA_OPS):
        return _base_ev(node, env)
    a = [ev(c, env) for c in node["kids"]]
    name = k[1]
    if name in ("group_sum", "group_mean", "group_count"):
        return group_agg(a[0], a[1], name)
    if name == "group_delay":
        return shift(a[0], int(a[1]))
    if name == "ts_backfill":
        return ts_backfill(a[0], int(a[1]))
    d, j = int(k[2][0]), int(k[2][1])
    return asof_rank(name, a[0], a[1], d, j, a[2] if name in ASOF_PAIR else None)


xw.ev = ev  # xwq_check's interpreter dispatches through the module-level name `ev`


def run(text, env):
    return xw.run(text, env)


# ------------------------------------------------------------------------------------------------ synthetic world
LISTINGS = ((0, 40), (1, 70), (2, 100))            # (line, first session)
DELISTINGS = ((3, 60), (4, 95), (5, 130))          # (line, first missing session); the factor stays at its last value
RECLASS = ((6, 50), (7, 80), (8, 110), (9, 140))   # (line, session of an industry change)
HALT_SESSION = 90                                  # one session without a bar on the first line with no corporate action
N_INDUSTRIES = 6                                   # a few industries, so every group has several members


def world(seed: int = 20261002, T: int = 160, N: int = 36) -> dict:
    """xwq_check.synthetic (splits, dividends, a per-line factor anchor, raw share volume) plus listings, delistings,
    a halted session and time-varying industry labels (grp_ff49). A line's raw bar and volume are NaN while it does not
    trade; a delisted line's vendor factor stays at its last value (no corporate action after it stops trading)."""
    w = xw.synthetic(seed=seed, T=T, N=N)
    raw = {k: v.copy() for k, v in w["raw"].items()}
    F, V = w["F"].copy(), w["V"].copy()
    rng = np.random.default_rng(seed + 7)
    for i, t0 in LISTINGS:
        for k in raw:
            raw[k][:t0, i] = np.nan
        V[:t0, i] = np.nan
    for i, t1 in DELISTINGS:
        for k in raw:
            raw[k][t1:, i] = np.nan
        V[t1:, i] = np.nan
        F[t1:, i] = F[t1 - 1, i]
    quiet = [i for i in range(10, N) if np.all(F[:, i] == F[0, i])]
    assert quiet, "no line without a corporate action to halt"
    for k in raw:
        raw[k][HALT_SESSION, quiet[0]] = np.nan
    V[HALT_SESSION, quiet[0]] = np.nan
    grp = np.tile(rng.integers(1, N_INDUSTRIES + 1, N).astype(float), (T, 1))
    for i, t2 in RECLASS:
        grp[t2:, i] = grp[t2, i] % N_INDUSTRIES + 1
    grp = np.where(np.isnan(raw["c"]), np.nan, grp)
    return {**w, "raw": raw, "F": F, "V": V, "grp49": grp, "halted": quiet[0]}


def house_env(wd: dict, anchor=None) -> dict:
    env = xw.house_env({**wd, "grp": {}}, anchor=anchor)
    env["grp_ff49"] = wd["grp49"]
    return env


# ------------------------------------------------------------------------------------------------ printed formulas
def _win(x, d):
    return xw._w(x, d)


def d_cov(x, y, d):
    wx, wy = _win(x, d), _win(y, d)
    ok = ~np.isnan(wx).any(axis=-1) & ~np.isnan(wy).any(axis=-1)
    sxy = ((wx - wx.mean(axis=-1, keepdims=True)) * (wy - wy.mean(axis=-1, keepdims=True))).sum(axis=-1)
    return np.where(ok, sxy / (d - 1), np.nan)


def d_corr(x, y, d):
    return xw.d_corr(x, y, d)


def d_std(x, d):
    w = _win(x, d)
    m = w.mean(axis=-1)
    ss = ((w - m[..., None]) ** 2).sum(axis=-1)
    sd = np.where(xw._flat(ss, m, d), 0.0, np.sqrt(ss / (d - 1)))
    return np.where(np.isfinite(w).all(axis=-1), sd, np.nan)


def d_argmax(x, d):
    w = _win(x, d)
    return np.where(~np.isnan(w).any(axis=-1), np.argmax(np.nan_to_num(w, nan=-np.inf), axis=-1) + 1.0, np.nan)


def d_product(x, d):
    w = _win(x, d)
    return np.where(~np.isnan(w).any(axis=-1), np.prod(w, axis=-1), np.nan)


def d_scale(x):
    l1 = np.nansum(np.abs(x), axis=1, keepdims=True)
    with np.errstate(invalid="ignore"):
        return np.where(np.isnan(x), np.nan, x * np.where(l1 == 0, 0.0, 1.0 / np.where(l1 == 0, 1.0, l1)))


def d_indneutralize(x, g):
    out = np.full(x.shape, np.nan)
    for t in range(x.shape[0]):
        ok = ~np.isnan(x[t]) & ~np.isnan(g[t])
        for lab in np.unique(g[t][ok]):
            m = ok & (g[t] == lab)
            out[t, m] = x[t, m] - x[t, m].mean()
    return out


def d_delta(x, d):
    return x - xw.d_shift(x, d)


def d_ret(P):
    return P["c"] / xw.d_shift(P["c"], 1) - 1.0


def d_minp(a, b):
    return np.where(np.isnan(a) | np.isnan(b), np.nan, np.minimum(a, b))


def p001(P, V, RC, G):
    r = d_ret(P)
    v = np.where(np.isnan(r), np.nan, np.where(r < 0, d_std(r, 20), P["c"]))
    return rank2d(d_argmax(np.sign(v) * np.abs(v) ** 2.0, 5)) - 0.5


def p029(P, V, RC, G):
    inner = rank2d(rank2d(-1.0 * rank2d(d_delta(P["c"] - 1.0, 5))))
    with np.errstate(divide="ignore", invalid="ignore"):
        x = d_product(rank2d(rank2d(d_scale(np.log(xw.d_sum(xw.d_min(inner, 2), 1))))), 1)
    return d_ts_min_nan(x, 5) + xw.d_tsrank(xw.d_shift(-1.0 * d_ret(P), 6), 5)


def d_ts_min_nan(x, d):
    w = _win(x, d)
    return np.where(~np.isnan(w).any(axis=-1), w.min(axis=-1), np.nan)


def p031(P, V, RC, G):
    a = rank2d(rank2d(rank2d(xw.d_decay(-1.0 * rank2d(rank2d(d_delta(P["c"], 10))), 10))))
    b = rank2d(-1.0 * d_delta(P["c"], 3))
    return (a + b) + np.sign(d_scale(d_corr(xw.d_mean(RC * V, 20), P["l"], 12)))


def p068(P, V, RC, G):
    a = xw.d_tsrank(d_corr(rank2d(P["h"]), rank2d(xw.d_mean(RC * V, 15)), 8), 13)
    b = rank2d(d_delta(P["c"] * 0.518371 + P["l"] * (1 - 0.518371), 1))
    return -1.0 * xw._cmp_lt(a, b)


def p080(P, V, RC, G):
    inn = d_indneutralize(P["o"] * 0.868128 + P["h"] * (1 - 0.868128), G)
    with np.errstate(invalid="ignore"):
        return -1.0 * np.power(rank2d(np.sign(d_delta(inn, 4))),
                               xw.d_tsrank(d_corr(P["h"], xw.d_mean(RC * V, 10), 5), 5))


def p088(P, V, RC, G):
    a = rank2d(xw.d_decay((rank2d(P["o"]) + rank2d(P["l"])) - (rank2d(P["h"]) + rank2d(P["c"])), 8))
    b = xw.d_tsrank(xw.d_decay(d_corr(xw.d_tsrank(P["c"], 8), xw.d_tsrank(xw.d_mean(RC * V, 60), 20), 8), 6), 2)
    return d_minp(a, b)


PRINTED_NUMPY = {   # each: (P = prices adjusted as of the row, V = share volume, RC = raw close, G = industry) -> alpha
    1: p001,
    3: lambda P, V, RC, G: -1.0 * d_corr(rank2d(P["o"]), rank2d(V), 10),
    4: lambda P, V, RC, G: -1.0 * xw.d_tsrank(rank2d(P["l"]), 9),
    13: lambda P, V, RC, G: -1.0 * rank2d(d_cov(rank2d(P["c"]), rank2d(V), 5)),
    15: lambda P, V, RC, G: -1.0 * xw.d_sum(rank2d(d_corr(rank2d(P["h"]), rank2d(V), 3)), 3),
    16: lambda P, V, RC, G: -1.0 * rank2d(d_cov(rank2d(P["h"]), rank2d(V), 5)),
    29: p029, 31: p031, 68: p068, 80: p080, 88: p088,
}


def printed_alpha(n: int, wd: dict, lookback: int) -> np.ndarray:
    """Alpha #n of the paper on world ``wd``: at each row t every price of rows <= t is adjusted as of t (raw x F(s) /
    F(t)), the printed formula is evaluated on that history (the last lookback + 1 rows suffice) and its row t kept."""
    T, N = wd["T"], wd["N"]
    out = np.full((T, N), np.nan)
    with np.errstate(invalid="ignore", divide="ignore"):
        for t in range(T):
            s0 = max(0, t - lookback - 1)
            g = wd["F"][s0:t + 1] / wd["F"][t]
            P = {k: wd["raw"][k][s0:t + 1] * g for k in "ohlc"}
            out[t] = PRINTED_NUMPY[n](P, wd["V"][s0:t + 1], wd["raw"]["c"][s0:t + 1], wd["grp49"][s0:t + 1])[-1]
    return out


# ------------------------------------------------------------------------------------------------ the transcriptions
M = {**xw.MACROS, "W": "ts_backfill((raw_close / close), 5)",
     "P80": "((open_adj * 0.868128) + (high_adj * (1 - 0.868128)))"}
for _d in (10, 15):
    M[f"ADV{_d}"] = f"ts_mean((raw_close * volume), {_d})"


def t(template: str) -> str:
    return template.format(**M)


def _arg001():
    """Ts_ArgMax(SignedPower(v, 2), 5) with v = (returns < 0) ? stddev(returns, 20) : close, close adjusted as of the
    day. SignedPower(., 2) is increasing on v >= 0 (a stddev or a price), so the argmax of the squares is the argmax
    of v. Split v into its two branches (-1 marks the other branch's sessions): A (stddevs, as of any day) and B
    (closes on the house basis; their as-of-t level is B * K(t), K(t) one positive number per line in the window).
    The overall maximum is max(MA, MB * K); its first position is argmax(B) when the close branch wins, argmax(A)
    when the stddev branch wins, and the earlier of the two on a tie."""
    a = "(({R} < 0) ? stddev({R}, 20) : -1)"
    b = "(({R} < 0) ? -1 : close)"
    c = "(ts_max(" + b + ", 5) * {K})"
    ma = "ts_max(" + a + ", 5)"
    arg_a, arg_b = "ts_argmax(" + a + ", 5)", "ts_argmax(" + b + ", 5)"
    return ("((" + c + " > " + ma + ") ? " + arg_b + " : ((" + ma + " > " + c + ") ? " + arg_a + " : min(" + arg_a
            + ", " + arg_b + ")))")


def _lagged_sum015():
    term = "rank(asof_rank_correlation(high_adj, {K}, rank(volume), 3, %d))"
    return "(-1 * ((" + term % 2 + " + " + term % 1 + ") + " + term % 0 + "))"


def _lagged_min029():
    term = ("product(rank(rank(scale(log(ts_sum(asof_rank_ts_min((-1 * delta((close - 1), 5)), {K}, 2, %d), 1)))))"
            ", 1)")
    acc = term % 4
    for j in (3, 2, 1, 0):
        acc = f"min({acc}, {term % j})"
    return "(" + acc + " + ts_rank(delay((-1 * {R}), 6), 5))"


def _decay088():
    """#88's first term rank(decay_linear(C, 8)), C = (rank(open) + rank(low)) - (rank(high) + rank(close)) with every
    rank re-evaluated as of the day. The decay runs over the COMPOSITE, so one as-of decay per price (decay(a) +
    decay(b) - ...) reassociates the sums and breaks exact ties of the outer rank. Instead C is built per lag k from
    the one-row as-of rank asof_rank_ts_min(x, K, 1, k) (the minimum of a single value is that value) and the decay
    is unrolled in the kernel's order (ts_ops.hpp: acc = 0 + 1 * C[t-7] + 2 * C[t-6] + ... + 8 * C[t], / 36; C is
    never -0, so 0 + 1 * C == 1 * C)."""
    r = "asof_rank_ts_min(%s, {K}, 1, %d)"

    def comp(k):
        return (f"(({r % ('open_adj', k)} + {r % ('low_adj', k)}) - ({r % ('high_adj', k)} + "
                f"{r % ('close', k)}))")
    acc = f"(1 * {comp(7)})"
    for i in range(1, 8):
        acc = f"({acc} + ({i + 1} * {comp(7 - i)}))"
    a = f"rank(({acc} / 36))"
    return ("min(" + a + ", ts_rank(decay_linear(correlation(ts_rank(close, 8), ts_rank({ADV60}, 20), 8), 6), 2))")


def _unrolled068():
    z = "asof_rank_correlation(high_adj, {K}, rank({ADV15}), 8, %d)"
    acc = None
    for j in range(1, 13):
        c = f"(({z % j} < {z % 0}) ? 1 : (({z % j} == {z % 0}) ? 0.5 : 0))"
        acc = c if acc is None else f"({acc} + {c})"
    tr = f"({acc} / 12)"
    return ("(-1 * ((" + tr + " < rank((delta(((close * 0.518371) + (low_adj * (1 - 0.518371))), 1) * {K}))) ? 1 : "
            "0))")


TRANSCRIPTIONS = {   # n -> DSL template (filled by t()); the frozen strings
    1: "(rank(" + _arg001() + ") - 0.5)",
    3: "(-1 * asof_rank_correlation(open_adj, {K}, rank(volume), 10, 0))",
    4: "(-1 * asof_rank_ts_rank(low_adj, {K}, 9, 0))",
    13: "(-1 * rank(asof_rank_covariance(close, {K}, rank(volume), 5, 0)))",
    15: _lagged_sum015(),
    16: "(-1 * rank(asof_rank_covariance(high_adj, {K}, rank(volume), 5, 0)))",
    29: _lagged_min029(),
    31: ("((rank(rank(rank((-1 * asof_rank_decay_linear(delta(close, 10), {K}, 10, 0))))) + rank((-1 * (delta(close, 3)"
         " * {K})))) + sign(scale(correlation({ADV20}, low_adj, 12))))"),
    80: ("(-1 * power(rank(sign((indneutralize(({P80} * {W}), grp_ff49) - indneutralize((delay({P80}, 4) * {W}), "
         "group_delay(grp_ff49, 4))))), ts_rank(correlation(high_adj, {ADV10}, 5), 5)))"),
    88: _decay088(),
}
UNROLLED = {68: _unrolled068()}   # verified, but over the DSL byte limit: not frozen
BLOCKED = {
    68: "three as-of stages: ts_rank over 13 sessions of correlations whose inner rank(high) is re-evaluated as of "
        "the day; expressible only unrolled (13 lagged as-of correlations, 24 comparisons), {bytes} bytes, over the "
        "4,096-byte DSL limit",
    92: "four as-of stages: ts_rank(6) of decay_linear(6) of correlation(7) of rank(low) re-evaluated as of the day "
        "(11 lagged correlations under 6 decays under a 6-way rank, tens of kilobytes unrolled)",
}
HOUSE_FORM = {1: 21, 3: 21, 4: 5, 13: 5, 15: 5, 16: 5, 29: 5, 31: 21, 68: 21, 80: 21, 88: 21}  # PM7-34 (adv{d} = d)

# Point-in-time reading: each past cross-section on that session's own prices (x * raw_close / close of the SAME row)
# instead of the evaluation day's adjustment. Must DIFFER from the printed formula (the as-of ops have teeth).
CAUSAL = {
    1: "(rank(ts_argmax(signedpower((({R} < 0) ? stddev({R}, 20) : (close * {K})), 2), 5)) - 0.5)",
    3: "(-1 * correlation(rank((open_adj * {K})), rank(volume), 10))",
    4: "(-1 * ts_rank(rank((low_adj * {K})), 9))",
    13: "(-1 * rank(covariance(rank((close * {K})), rank(volume), 5)))",
    15: "(-1 * ts_sum(rank(correlation(rank((high_adj * {K})), rank(volume), 3)), 3))",
    16: "(-1 * rank(covariance(rank((high_adj * {K})), rank(volume), 5)))",
    29: ("(ts_min(product(rank(rank(scale(log(ts_sum(ts_min(rank(rank((-1 * rank((delta((close - 1), 5) * {K}))))), 2), "
         "1))))), 1), 5) + ts_rank(delay((-1 * {R}), 6), 5))"),
    31: ("((rank(rank(rank(decay_linear((-1 * rank(rank((delta(close, 10) * {K})))), 10)))) + rank((-1 * (delta(close, "
         "3) * {K})))) + sign(scale(correlation({ADV20}, low_adj, 12))))"),
    80: ("(-1 * power(rank(sign(delta(indneutralize(({P80} * {K}), grp_ff49), 4))), ts_rank(correlation(high_adj, "
         "{ADV10}, 5), 5)))"),
    88: ("min(rank(decay_linear(((rank((open_adj * {K})) + rank((low_adj * {K}))) - (rank((high_adj * {K})) + "
         "rank((close * {K})))), 8)), ts_rank(decay_linear(correlation(ts_rank(close, 8), ts_rank({ADV60}, 20), 8), 6), "
         "2))"),
}


def mutants(n: int) -> list:
    """Plausible transcription errors of #n; each must fail against the printed formula."""
    s = TRANSCRIPTIONS[n]
    out = {
        # Dropping {K} or min -> max in the tie branch is NOT a planted error: an as-of price exceeds a 20-day
        # return stddev on any realistic line, so the cross-branch comparison never turns on the rebase and the
        # two maxima never tie (both kept for exactness; task-YOPS-report.md section 3).
        1: [s.replace("stddev({R}, 20)", "stddev({R}, 19)"),
            s.replace("ts_argmax((({R} < 0) ? -1 : close), 5)", "ts_argmax((({R} < 0) ? -1 : close), 4)"),
            s.replace(") > ts_max(", ") < ts_max(", 1), s.replace("ts_max(", "ts_min(", 1)],
        3: [s.replace(", 10, 0)", ", 9, 0)"), s.replace(", 10, 0)", ", 10, 1)"), s.replace("open_adj", "high_adj"),
            s.replace("{K}", "(close / close)"), s.replace("asof_rank_correlation", "asof_rank_covariance")],
        4: [s.replace(", 9, 0)", ", 8, 0)"), s.replace(", 9, 0)", ", 9, 1)"), s.replace("low_adj", "open_adj"),
            s.replace("{K}", "(close / close)")],
        13: [s.replace(", 5, 0)", ", 4, 0)"), s.replace("rank(volume)", "volume"), s.replace("close, {K}", "open_adj, {K}"),
             s.replace("asof_rank_covariance", "asof_rank_correlation")],
        15: [s.replace("3, 2)", "3, 3)"), s.replace("3, 1)", "3, 0)"), s.replace("high_adj", "low_adj"),
             s.replace(", 3, ", ", 4, ")],
        16: [s.replace(", 5, 0)", ", 6, 0)"), s.replace("high_adj", "low_adj"), s.replace("{K}", "(close / close)")],
        # #29's first term is the constant 0.5 wherever finite (degenerate as printed), so a planted error there
        # shows only through the NaN pattern: the j = 4 lag and the delta window move it, a j = 0 window does not.
        29: [s.replace("2, 4)", "2, 5)"), s.replace("delay((-1 * {R}), 6)", "delay((-1 * {R}), 5)"),
             s.replace("delta((close - 1), 5)", "delta((close - 1), 6)")],
        31: [s.replace("{K}, 10, 0)", "{K}, 9, 0)"), s.replace("delta(close, 10)", "delta(close, 9)"),
             s.replace("(-1 * asof_rank", "(1 * asof_rank"), s.replace("{K}, 10, 0)", "(close / close), 10, 0)")],
        80: [s.replace("group_delay(grp_ff49, 4)", "grp_ff49"), s.replace("{W}", "{K}"),
             s.replace("delay({P80}, 4)", "delay({P80}, 3)"),
             s.replace("group_delay(grp_ff49, 4)", "group_delay(grp_ff49, 3)")],
        88: [s.replace("(8 * ", "(7 * "), s.replace("asof_rank_ts_min(low_adj", "asof_rank_ts_min(high_adj"),
             s.replace("{K}, 1, 3)", "{K}, 1, 4)"), s.replace(") - (asof", ") + (asof"),
             s.replace("ts_rank({ADV60}, 20)", "ts_rank({ADV60}, 19)")],
    }[n]
    assert all(m != s for m in out), f"#{n}: a mutant equals the transcription"
    return out


# ------------------------------------------------------------------------------------------------ checks
def same(a, b, tol=1e-9) -> bool:
    return xw.same(a, b, tol)


def figures(text: str) -> dict:
    s = xs.static(text)
    return {k: s[k] for k in ("bars", "slots", "nodes", "bytes", "extra_fields", "sha256")}


def check_formula(n: int, wd: dict, text: str) -> tuple:
    """The frozen string against the printed formula (cell for cell, NaN pattern included) and the anchor probe.
    Returns (the printed reference panel, its finite cell count)."""
    env = house_env(wd)
    ref = printed_alpha(n, wd, figures(text)["bars"] + 4)
    got = run(text, env)
    finite = int(np.isfinite(ref).sum())
    assert finite > 0, f"#{n}: the oracle has no finite cell"
    if not same(got, ref):
        nan_diff = int((np.isnan(got) != np.isnan(ref)).sum())
        raise AssertionError(f"#{n}: the DSL differs from the printed formula ({nan_diff} NaN-pattern cells, max |d| "
                             f"{np.nanmax(np.abs(got - ref))})")
    other = house_env(wd, anchor=np.exp(np.random.default_rng(3).uniform(-3.0, 3.0, wd["N"])))
    assert same(run(text, other), got), f"#{n}: depends on the vendor factor anchor"
    return ref, finite


def probes(n: int, wd: dict, ref: np.ndarray) -> int:
    """The point-in-time reading and every planted error must differ from the printed formula. Returns the mutant
    count."""
    env = house_env(wd)
    if n in CAUSAL:
        assert not same(run(t(CAUSAL[n]), env), ref), f"#{n}: the point-in-time reading also matches (no teeth)"
    ms = mutants(n)
    for m in ms:
        assert not same(run(t(m), env), ref), f"#{n}: the planted error passes: {m}"
    return len(ms)


def degenerate_029(wd: dict) -> int:
    """#29 as printed: rank in [0, 1] puts a 0 under log, so scale's L1 norm is inf and every finite cell of the first
    term is the tie rank 0.5 (E3, degenerate as printed). Returns the number of finite cells checked."""
    env = house_env(wd)
    first = run(t(TRANSCRIPTIONS[29]).split(" + ts_rank(delay(")[0][1:], env)
    fin = first[np.isfinite(first)]
    assert fin.size > 0 and np.all(fin == 0.5), "#29's first term is not the constant 0.5"
    return int(fin.size)


def group_sum_checks(wd: dict) -> list:
    env = house_env(wd)
    gs = run("group_sum(close, grp_ff49)", env)
    gm = run("(group_mean(close, grp_ff49) * group_count(close, grp_ff49))", env)
    ok = ~np.isnan(gs)
    assert np.array_equal(ok, ~np.isnan(gm)) and np.allclose(gs[ok], gm[ok], rtol=1e-12, atol=0), "group_sum"
    r, w, g = "((close / delay(close, 21)) - 1)", "me_company", "grp_ff49"
    vw_sum = (f"rank(decay_linear(((group_sum(({r} * {w}), {g}) - ({r} * {w})) / (group_sum(({w} + (0 * {r})), {g}) - "
              f"({w} + (0 * {r})))), 21))")
    vw_mean = (f"rank(decay_linear((((group_mean(({r} * {w}), {g}) * group_count(({r} * {w}), {g})) - ({r} * {w})) / "
               f"((group_mean(({w} + (0 * {r})), {g}) * group_count(({w} + (0 * {r})), {g})) - ({w} + (0 * {r})))), 21))")
    inner_sum, inner_mean = vw_sum[len("rank(decay_linear("):-len(", 21))")], vw_mean[len("rank(decay_linear("):-len(", 21))")]
    a, b = run(inner_sum, env), run(inner_mean, env)
    fin = ~np.isnan(a)
    assert np.array_equal(fin, ~np.isnan(b)) and fin.any(), "value-weighted peer return: NaN pattern"
    assert np.allclose(a[fin], b[fin], rtol=1e-9, atol=1e-12), "value-weighted peer return: values"
    fs, fm = figures(vw_sum), figures(vw_mean)
    return [f"group_sum == group_mean x group_count on {int(ok.sum())} cells (rtol 1e-12)",
            f"value-weighted leave-one-out peer return: with group_sum bars {fs['bars']} slots {fs['slots']} nodes "
            f"{fs['nodes']}; with group_mean x group_count bars {fm['bars']} slots {fm['slots']} nodes {fm['nodes']}"]


def registry_rows() -> None:
    for name, (lo, hi, op, dt, nh) in REGISTERED.items():
        row = xs.OPS.get(name)
        assert row is not None and (row["lo"], row["hi"], row["opcode"], row["dtype"], row["nh"]) == (lo, hi, op, dt, nh), \
            f"registry.cpp row for {name}: {row}"


def main():
    registry_rows()
    wd = world()
    print(f"registry: {len(REGISTERED)} formulaic rows match registry.cpp; synthetic world {wd['T']} x {wd['N']}: "
          f"{len(LISTINGS)} listings, {len(DELISTINGS)} delistings, {len(RECLASS)} reclassifications, one halted session")
    for line in group_sum_checks(wd):
        print(line)
    n_mut = 0
    for n, tpl in TRANSCRIPTIONS.items():
        text = t(tpl)
        ref, cells = check_formula(n, wd, text)
        k = probes(n, wd, ref)
        n_mut += k
        f = figures(text)
        fw = figures(f"rank(decay_linear({text}, {HOUSE_FORM[n]}))")
        print(f"#{n:3d} == printed on {cells} cells; anchor-free; point-in-time reading differs; {k} planted errors "
              f"fail; bars {f['bars']} slots {f['slots']} nodes {f['nodes']} bytes {f['bytes']} extra "
              f"{f['extra_fields']} sha256 {f['sha256']}")
        print(f"     house form R(decay_linear(x, {HOUSE_FORM[n]})): bars {fw['bars']} slots {fw['slots']} nodes "
              f"{fw['nodes']} bytes {fw['bytes']} sha256 {fw['sha256']}")
        print(f"     {text}")
    print(f"#29 degenerate as printed: its first term is 0.5 on all {degenerate_029(wd)} finite cells")
    for n, tpl in UNROLLED.items():
        text = t(tpl)
        _, cells = check_formula(n, wd, text)
        print(f"#{n:3d} unrolled == printed on {cells} cells; " + BLOCKED[n].format(bytes=len(text.encode())))
    print(f"#{92:3d} blocked: " + BLOCKED[92])
    print(f"mutation probe: {n_mut} planted errors, every one fails")
    print("yops_check: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
