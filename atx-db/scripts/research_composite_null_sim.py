"""Null simulation behind the frozen composite policy (ruling C-22; P5 review C1).

Evidence for ``p5-composite-v2`` (``seeds/research_composite_policy.json``, content sha256
``28f3405dfe3adfbe3e29d976ab6eea026533150c728e8c82e6037cd367b45fee``; v1 is superseded). The question: does
selecting composite constituents on train + validation evidence only (v2) keep the composite's one holdout
look honest, where selecting on R4's ``qualified_*`` statuses, which already passed the holdout gates (v1), does
not? R4's holdout gate is ``holdout z >= 1.5`` in the fixed direction.

Part A: pure null, fix-1 reproduction (seed 5)
    40 signals with zero predictive power, 300 names, 96 selection months, 24 holdout months. Rules:
    v1 = holdout z >= 1.5 (what R4 ``qualified_*`` implies); v2 loose = selection z >= 1.5 (the v2 mechanism
    with a lenient gate, so that composites form under the null); v2 family gate = R4 gate_significance and
    gate_sign (HLZ |z| >= 3, or BH q <= 0.05 and |z| >= 2; positive). The first 400 sims are exactly the P5
    fix-1 run (``scratchpad/P5-null-sim.py``): v1 332/400, pass 1.000; v2 loose 313/400, pass 0.061; v2 family
    gate 1/400. The composite is built with the committed P5 code (see below) and is rank-identical to that
    run's equal-weight mean, so the equal-weight numbers reproduce exactly.

Part B: real vs fresh holdout under the actual v2 rule (seed 11)
    The re-review design (``scratchpad/rv-p5-v2-selection.py``) with R4's five selection gates: significance
    and sign as above, >= 2 of 3 selection subperiods with a positive mean IC, and a positive mean IC in the
    small AND the large cap tercile (gate_coverage is trivially met). v1 adds R4's holdout gates (holdout sign,
    holdout z >= 1.5). 40 signals, 200 names, 141 selection months (3 subperiods), 24 holdout months, plus an
    independent 24-month "fresh" holdout from the same world (the unbiased reference). Worlds: pure null, and a
    mixed world (5 priced signals, return loading ``--beta``) calibrated so that the true composite's pass rate
    is inside (0.2, 0.8), i.e. informative. Acceptance: for v2 the real-holdout pass rate lies inside the 95%
    CI of the fresh-holdout pass rate; for v1 it clearly does not.

Committed code exercised
    Composites use ``composites.fit_weights`` (``equal`` and ``ic_shrunk``: weights fit on the selection
    sample, the parked residual P-1), ``composites._pair_correlations`` of within-month ranks over the
    selection months (the fit correlation, as ``build_forward``), ``composites._zscore_rows`` and
    ``composites._combine`` (variance-adjusted, re-standardized), under the frozen policy's ``ic_shrinkage``,
    ``min_constituent_share``, ``min_names_per_formation`` and ``min_formations``.

Simplifications
    iid months with an h = 1 rank-IC t statistic stand in for R3b's EWC fixed-b z at h = 3; BH is over the
    simulated signals at one variant and horizon (R3b's family is every catalog cell); the fit sample purges
    the last selection month (its 1-month label window reaches the holdout; production purges by the 3-month
    fit horizon); signals are independent, so no cluster forms and every selected signal is a constituent of
    one class composite; size terciles come from a random cap order. The nominal rate P(t_23 >= 1.5) is
    analytic (regularized incomplete beta), not a Monte Carlo draw. Pass rates carry Wilson 95% intervals.

Run (numpy work only, no store access, no file written; OPENBLAS_NUM_THREADS=1; 183 MiB peak working set
with the atx_db imports; 1,108 s with the defaults):
    OPENBLAS_NUM_THREADS=1 python scripts/research_composite_null_sim.py [--sims-a 2000] [--sims-b 2000]

Expected output (defaults; 2026-09-26, numpy 2.5.2, python 3.12.2; pass = composite holdout t >= 1.5 with its
Wilson 95% CI; [eq] = ``equal`` weights, [icw] = ``ic_shrunk``):
    PART A pure null, nominal P(t_23 >= 1.5) = 0.0736 (analytic)
      first 400 sims (= fix-1 run), [eq]:
        v1            332/400 composites, holdout t mean +3.56, pass 1.000 [0.989, 1.000]
        v2 loose      313/400 composites, holdout t mean -0.13, pass 0.061 [0.039, 0.093]
        v2 family      1/400  composites, holdout t mean -0.96, pass 0.000
      all 2000 sims:
        v1            1615/2000, t mean +3.54, pass 1.000 [0.998, 1.000] (icw: t +3.14, pass 1.000)
        v2 loose      1547/2000, t mean -0.01, pass 0.078 [0.066, 0.093] (icw: 0.078 [0.065, 0.092])
        v2 family        6/2000, t mean -0.40, pass 0.167 [0.030, 0.564]
    PART B pure null: no composite forms under the v2 or v1 rule (0/2000).
    PART B mixed (5 priced, beta 0.015), 2000 sims:
        v2 [eq]   917 composites (k 2.65): real pass 0.516 [0.483, 0.548], fresh 0.481 [0.449, 0.513];
                  paired gap real - fresh +0.035 [-0.011, +0.081], covers 0 (icw: +0.038 [-0.008, +0.084])
        v1 [eq]   152 composites (k 2.11): real pass 1.000 [0.975, 1.000], fresh 0.421 [0.345, 0.501];
                  paired gap +0.579 [+0.500, +0.658], excludes 0 (icw: +0.592 [+0.514, +0.670])
        v2 constituents that are priced signals: 0.949
    Reading: the true composite's (fresh) pass rate is 0.48, inside (0.2, 0.8), so the mixed world is
    informative. v2's real-holdout look is unbiased (the paired gap covers 0; its real rate is 0.003 above the
    fresh rate's own Wilson CI, which a same-distribution pair misses about 17% of the time: the paired gap is
    the test). v1's look is inflated by +0.58.
"""

from __future__ import annotations

import argparse
import datetime as dt
import math
import os
import sys
import time
from collections.abc import Sequence
from statistics import NormalDist

os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")

import numpy as np
import pandas as pd

from atx_db.research import composites as cmp
from atx_db.research import evaluation as ev

POLICY_VERSION = "p5-composite-v2"
POLICY_SHA256 = "28f3405dfe3adfbe3e29d976ab6eea026533150c728e8c82e6037cd367b45fee"
#: R4 r4-qualification-v3 gates the simulation applies.
HLZ_MIN_Z, BH_MAX_Q, BH_MIN_Z, HOLDOUT_MIN_Z, SUBPERIODS_MIN_SAME = 3.0, 0.05, 2.0, 1.5, 2
_NORMAL = NormalDist()


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

def ranks(x: np.ndarray) -> np.ndarray:
    return x.argsort(axis=-1).argsort(axis=-1).astype(float)


def rank_ic(signal_ranks: np.ndarray, return_ranks: np.ndarray) -> np.ndarray:
    x = signal_ranks - signal_ranks.mean(axis=-1, keepdims=True)
    y = return_ranks - return_ranks.mean(axis=-1, keepdims=True)
    return (x * y).sum(axis=-1) / np.sqrt((x * x).sum(axis=-1) * (y * y).sum(axis=-1))


def tstat(ic: np.ndarray) -> np.ndarray:
    return ic.mean(axis=-1) / (ic.std(axis=-1, ddof=1) / np.sqrt(ic.shape[-1]))


def bh(p: np.ndarray, q: float = BH_MAX_Q) -> np.ndarray:
    order = np.argsort(p)
    m = len(p)
    passed = p[order] <= q * np.arange(1, m + 1) / m
    keep = np.zeros(m, dtype=bool)
    if passed.any():
        keep[order[: np.max(np.nonzero(passed)[0]) + 1]] = True
    return keep


def family_gate(z: np.ndarray) -> np.ndarray:
    """R4 gate_significance and gate_sign for signed (+1) hypotheses."""
    p = np.array([2.0 * (1.0 - _NORMAL.cdf(abs(v))) for v in z])
    return ((np.abs(z) >= HLZ_MIN_Z) | (bh(p) & (np.abs(z) >= BH_MIN_Z))) & (z > 0)


def _betacf(a: float, b: float, x: float) -> float:
    tiny, qab, qap, qam = 1e-300, a + b, a + 1.0, a - 1.0
    c, d = 1.0, 1.0 - qab * x / qap
    d = 1.0 / (d if abs(d) > tiny else tiny)
    h = d
    for m in range(1, 400):
        m2 = 2 * m
        aa = m * (b - m) * x / ((qam + m2) * (a + m2))
        d = 1.0 + aa * d
        d = 1.0 / (d if abs(d) > tiny else tiny)
        c = 1.0 + aa / c if abs(1.0 + aa / c) > tiny else tiny
        h *= d * c
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2))
        d = 1.0 + aa * d
        d = 1.0 / (d if abs(d) > tiny else tiny)
        c = 1.0 + aa / c if abs(1.0 + aa / c) > tiny else tiny
        delta = d * c
        h *= delta
        if abs(delta - 1.0) < 1e-15:
            break
    return h


def betainc(a: float, b: float, x: float) -> float:
    """Regularized incomplete beta I_x(a, b) (continued fraction)."""
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    front = math.exp(math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b) + a * math.log(x) + b * math.log1p(-x))
    if x < (a + 1.0) / (a + b + 2.0):
        return front * _betacf(a, b, x) / a
    return 1.0 - front * _betacf(b, a, 1.0 - x) / b


def t_upper_tail(t: float, df: int) -> float:
    """P(T_df >= t) for t >= 0, analytic."""
    return 0.5 * betainc(df / 2.0, 0.5, df / (df + t * t))


def wilson(successes: int, n: int, z: float = 1.959964) -> tuple[float, float]:
    if n == 0:
        return math.nan, math.nan
    p = successes / n
    centre = (p + z * z / (2 * n)) / (1 + z * z / n)
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / (1 + z * z / n)
    return centre - half, centre + half


def rate(values: Sequence[float]) -> str:
    values = np.asarray(values, dtype=float)
    if not len(values):
        return "no composite formed"
    k = int((values >= HOLDOUT_MIN_Z).sum())
    lo, hi = wilson(k, len(values))
    return (f"holdout t mean {values.mean():+.2f}, median {np.median(values):+.2f}, pass (t>=1.5) "
            f"{k / len(values):.3f} [95% CI {lo:.3f}, {hi:.3f}]")


# ---------------------------------------------------------------------------
# The composite, built by the committed P5 code
# ---------------------------------------------------------------------------

def month_ends(first: dt.date, months: int) -> list[dt.date]:
    out = []
    for k in range(months + 1):
        total = first.month - 1 + k + 1
        out.append(dt.date(first.year + total // 12, total % 12 + 1, 1) - dt.timedelta(days=1))
    return out


class CompositeMaker:
    """Weights (``fit_weights`` on the members' monthly selection ICs, last month purged), the fit-sample
    correlation (``_pair_correlations`` of within-month ranks) and ``_combine`` of ``_zscore_rows`` blocks."""

    def __init__(self, policy: cmp.CompositePolicy, selection_months: int, split: ev.FrozenSplit,
                 first: dt.date) -> None:
        self.policy, self.split = policy, split
        ends = month_ends(first, selection_months)
        self.fit_months = selection_months - 1  # the last selection month's label window reaches the holdout
        self.formations, self.label_ends = ends[:self.fit_months], ends[1:self.fit_months + 1]

    def weights(self, selection_ic: np.ndarray, weighting: str) -> list[float]:
        members = [f"m{i}" for i in range(len(selection_ic))]
        rows = pd.DataFrame({"member_id": np.repeat(members, self.fit_months),
                             "formation_date": self.formations * len(members),
                             "label_end": self.label_ends * len(members),
                             "ic": selection_ic[:, :self.fit_months].ravel()})
        fitted = cmp.fit_weights(rows, members, self.split, weighting=weighting, shrinkage=self.policy.ic_shrinkage)
        return [fitted[m]["weight"] for m in members]

    def correlation(self, selection_values: np.ndarray) -> np.ndarray:
        stacked = np.stack([cmp._rank_rows(block) for block in selection_values])
        total, used = cmp._pair_correlations(stacked, self.policy.min_names)
        rho = np.where(used >= self.policy.min_formations, total / np.maximum(used, 1), 0.0)
        rho = np.clip(np.nan_to_num(rho, nan=0.0), -0.99, 0.99)
        np.fill_diagonal(rho, 1.0)
        return rho

    def holdout_t(self, values: np.ndarray, weights: Sequence[float], rho: np.ndarray,
                  return_ranks: np.ndarray) -> float:
        blocks = [cmp._zscore_rows(block) for block in values]
        clocks = [np.full(block.shape, -1, dtype=np.int64) for block in values]
        z, _, _ = cmp._combine(blocks, clocks, list(weights), rho.tolist(), self.policy.min_constituent_share)
        return float(tstat(rank_ic(ranks(z), return_ranks)))


# ---------------------------------------------------------------------------
# Part A: pure null, fix-1 reproduction
# ---------------------------------------------------------------------------

def part_a(policy: cmp.CompositePolicy, sims: int) -> None:
    names, selection, holdout, signals = 300, 96, 24, 40
    rng = np.random.default_rng(5)
    split = ev.freeze_split("p5_null_sim_a", [("train", "2012-01-01", "2017-12-31"),
                                              ("validation", "2018-01-01", "2019-12-31"),
                                              ("holdout", "2020-01-01", "2021-12-31")])
    maker = CompositeMaker(policy, selection, split, dt.date(2012, 1, 31))
    rules = ("v1: holdout z>=1.5 (R4 qualified)", "v2 loose: selection z>=1.5",
             "v2 family gate: HLZ |z|>=3 or BH q<=.05 & |z|>=2, positive")
    results = {(rule, w): [] for rule in rules for w in (cmp.WEIGHT_EQUAL, cmp.WEIGHT_IC)}
    counts: dict[str, list[int]] = {rule: [] for rule in rules}
    for sim in range(sims):
        returns = ranks(rng.standard_normal((selection + holdout, names)))
        values = rng.standard_normal((signals, selection + holdout, names))
        ic = rank_ic(ranks(values), returns[None])
        z_sel, z_hold = tstat(ic[:, :selection]), tstat(ic[:, selection:])
        chosen = {rules[0]: z_hold >= HOLDOUT_MIN_Z, rules[1]: z_sel >= 1.5, rules[2]: family_gate(z_sel)}
        for rule, mask in chosen.items():
            counts[rule].append(int(mask.sum()))
            if mask.sum() < 2:
                continue
            members = values[mask]
            rho = maker.correlation(members[:, :selection])
            for weighting in (cmp.WEIGHT_EQUAL, cmp.WEIGHT_IC):
                weights = maker.weights(ic[mask, :selection], weighting)
                results[(rule, weighting)].append(
                    (sim, maker.holdout_t(members[:, selection:], weights, rho, returns[selection:])))
    nominal = t_upper_tail(HOLDOUT_MIN_Z, holdout - 1)
    print(f"PART A  pure null: {signals} signals, {names} names, {selection} selection + {holdout} holdout "
          f"months, seed 5, {sims} sims; nominal P(t_{holdout - 1} >= 1.5) = {nominal:.4f} (analytic)")
    for label, limit in (("first 400 sims (= fix-1 run)", 400), (f"all {sims} sims", sims)):
        if limit > sims:
            continue
        print(f"  {label}:")
        for rule in rules:
            for weighting in (cmp.WEIGHT_EQUAL, cmp.WEIGHT_IC):
                ts = [t for sim, t in results[(rule, weighting)] if sim < limit]
                print(f"    {rule} [{weighting}]: composites {len(ts)}/{limit} (mean k "
                      f"{np.mean(counts[rule][:limit]):.2f}); {rate(ts)}")


# ---------------------------------------------------------------------------
# Part B: real vs fresh holdout under the actual v2 rule
# ---------------------------------------------------------------------------

def part_b(policy: cmp.CompositePolicy, sims: int, n_true: int, beta: float, label: str,
           rng: np.random.Generator) -> None:
    names, selection, holdout, signals = 200, 141, 24, 40
    subperiods = (slice(0, 47), slice(47, 94), slice(94, 141))
    split = ev.freeze_split("p5_null_sim_b", [("train", "2012-01-01", "2019-12-31"),
                                              ("validation", "2020-01-01", "2023-09-30"),
                                              ("holdout", "2023-10-01", "2025-09-30")])
    maker = CompositeMaker(policy, selection, split, dt.date(2012, 1, 31))

    def world(months: int) -> tuple[np.ndarray, np.ndarray]:
        values = rng.standard_normal((signals, months, names))
        noise = rng.standard_normal((months, names))
        returns = (beta * values[:n_true]).sum(axis=0) + noise if n_true else noise
        return values, returns

    out = {(rule, w, kind): [] for rule in ("v2", "v1") for w in (cmp.WEIGHT_EQUAL, cmp.WEIGHT_IC)
           for kind in ("real", "fresh")}
    ks: dict[str, list[int]] = {"v2": [], "v1": []}
    true_share: list[float] = []
    for _ in range(sims):
        values, returns = world(selection + holdout)
        fresh_values, fresh_returns = world(holdout)
        order = rng.permutation(names)  # cap order: terciles micro / small / large
        small, large = order[names // 3: 2 * names // 3], order[2 * names // 3:]
        value_ranks, return_ranks = ranks(values), ranks(returns)
        ic = rank_ic(value_ranks, return_ranks[None])
        ic_sel = ic[:, :selection]
        z_sel = tstat(ic_sel)
        subs = np.stack([ic_sel[:, part].mean(axis=1) > 0 for part in subperiods]).sum(axis=0) >= SUBPERIODS_MIN_SAME
        buckets = [rank_ic(ranks(values[:, :selection][:, :, b]), ranks(returns[:selection][:, b])[None]).mean(axis=1)
                   > 0 for b in (small, large)]
        v2 = family_gate(z_sel) & subs & buckets[0] & buckets[1]
        ic_hold = ic[:, selection:]
        v1 = v2 & (ic_hold.mean(axis=1) > 0) & (tstat(ic_hold) >= HOLDOUT_MIN_Z)
        for rule, mask in (("v2", v2), ("v1", v1)):
            if mask.sum() < 2:
                continue
            ks[rule].append(int(mask.sum()))
            if rule == "v2":
                true_share.append(float(mask[:n_true].sum() / mask.sum()) if n_true else 0.0)
            rho = maker.correlation(values[mask, :selection])
            for weighting in (cmp.WEIGHT_EQUAL, cmp.WEIGHT_IC):
                weights = maker.weights(ic_sel[mask], weighting)
                out[(rule, weighting, "real")].append(
                    maker.holdout_t(values[mask, selection:], weights, rho, return_ranks[selection:]))
                out[(rule, weighting, "fresh")].append(
                    maker.holdout_t(fresh_values[mask], weights, rho, ranks(fresh_returns)))
    print(f"PART B  {label}: {signals} signals ({n_true} priced, beta {beta}), {names} names, {selection} selection "
          f"(3 subperiods) + {holdout} holdout months + a fresh {holdout}-month holdout, {sims} sims")
    for rule in ("v2", "v1"):
        for weighting in (cmp.WEIGHT_EQUAL, cmp.WEIGHT_IC):
            real, fresh = out[(rule, weighting, "real")], out[(rule, weighting, "fresh")]
            print(f"  {rule} [{weighting}]: composites {len(real)}/{sims} (mean k when formed "
                  f"{np.mean(ks[rule]) if ks[rule] else math.nan:.2f})")
            print(f"      real  holdout: {rate(real)}")
            print(f"      fresh holdout: {rate(fresh)}")
            if real:
                passed_real = np.asarray(real) >= HOLDOUT_MIN_Z
                passed_fresh = np.asarray(fresh) >= HOLDOUT_MIN_Z
                lo, hi = wilson(int(passed_fresh.sum()), len(fresh))
                inside = lo <= passed_real.mean() <= hi
                gap = passed_real.astype(float) - passed_fresh.astype(float)  # paired: one composite, two holdouts
                half = 1.959964 * gap.std(ddof=1) / math.sqrt(len(gap)) if len(gap) > 1 else math.nan
                covers = gap.mean() - half <= 0.0 <= gap.mean() + half
                print(f"      real pass {'INSIDE' if inside else 'OUTSIDE'} the fresh 95% CI; paired gap real - fresh "
                      f"{gap.mean():+.3f} [95% CI {gap.mean() - half:+.3f}, {gap.mean() + half:+.3f}] "
                      f"({'covers' if covers else 'excludes'} 0)")
    if true_share:
        print(f"  v2 constituents that are priced signals: {np.mean(true_share):.3f} on average")


def _peak_mib() -> str:
    try:
        import ctypes
        from ctypes import wintypes

        class Counters(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                        ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                        ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
                        ("PrivateUsage", ctypes.c_size_t)]

        counters = Counters()
        counters.cb = ctypes.sizeof(counters)
        get = ctypes.WinDLL("kernel32").K32GetProcessMemoryInfo
        get.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
        get(ctypes.WinDLL("kernel32").GetCurrentProcess(), ctypes.byref(counters), counters.cb)
        return f"{counters.PeakWorkingSetSize / 2**20:.0f} MiB working set"
    except (AttributeError, OSError):  # not Windows
        import resource

        return f"{resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024:.0f} MiB max RSS"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--sims-a", type=int, default=2000)
    parser.add_argument("--sims-b", type=int, default=2000)
    parser.add_argument("--beta", type=float, default=0.015, help="return loading of each priced signal (mixed)")
    args = parser.parse_args(argv)
    policy = cmp.load_composite_policy()
    if (policy.version, policy.sha256) != (POLICY_VERSION, POLICY_SHA256):
        raise SystemExit(f"this simulation is evidence for {POLICY_VERSION} {POLICY_SHA256}, not {policy.version} "
                         f"{policy.sha256}")
    print(f"policy {policy.version} sha256 {policy.sha256}; numpy {np.__version__}; python {sys.version.split()[0]}")
    started = time.perf_counter()
    part_a(policy, args.sims_a)
    rng = np.random.default_rng(11)
    part_b(policy, args.sims_b, 0, 0.0, "pure null", rng)
    part_b(policy, args.sims_b, 5, args.beta, "mixed", rng)
    print(f"done in {time.perf_counter() - started:.0f} s; peak {_peak_mib()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
