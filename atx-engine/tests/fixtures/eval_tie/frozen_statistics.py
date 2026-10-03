"""FROZEN copy of the Python statistics of record retired by platform P9 lane B2 (T1 minor; plan section 0.6).

The engine verb atx-research-eval (K-P9-5) computes these statistics now; lane B2's deletion commit removed them from
atx-impl/tools/backtest_integrity.py, nav_summ.py and dsr_total.py. This module keeps them VERBATIM (as they were at
the deletion) for one purpose: the statistics tie fixture's checker (generate_eval_tie.py --check and
test_eval_tie_fixture.py) keeps recomputing expected.json from the code that defined it. Nothing outside this fixture
directory may import it (it lives under fixtures/, out of the G-P5 mirror scan, test_no_python_mirror.py).
The ledger rules it calls (trial_counts, ledger_n, dsr_variance, campaign_registry_count) stay live in
backtest_integrity.py (the ledger's Python writer and readers, ruling SQL-9).
"""
from __future__ import annotations

import itertools
import math
from pathlib import Path
import sys

import numpy as np

_TOOLS = Path(__file__).resolve().parents[4] / "atx-impl" / "tools"
if str(_TOOLS) not in sys.path:
    sys.path.insert(0, str(_TOOLS))
import backtest_integrity as _LEDGER  # noqa: E402  (the live ledger rules)

ANNUAL = 252
EULER_GAMMA = 0.5772156649015329
DEFAULT_SEED = 20260927
PBO_BLOCKS = 16
DEFAULT_DRAWS, DEFAULT_BLOCK = 2000, 21
V8_DRAWS, V8_SEED = 4999, 20260929
DEFAULT_DSR_N = 10
CONSTRUCTION = "construction"
dsr_variance = _LEDGER.dsr_variance
ledger_n = _LEDGER.ledger_n
trial_counts = _LEDGER.trial_counts
campaign_registry_count = _LEDGER.campaign_registry_count


class BI:  # noqa: N801  (the name the moved code calls the ledger module by)
    trial_counts = staticmethod(_LEDGER.trial_counts)
    campaign_registry_count = staticmethod(_LEDGER.campaign_registry_count)
    dsr_variance = staticmethod(_LEDGER.dsr_variance)
    ledger_n = staticmethod(_LEDGER.ledger_n)
    ANNUAL = ANNUAL


# ------------------------------------------------------------------ backtest_integrity.py
def norm_cdf(x: float) -> float:
    return 0.5 * math.erfc(-x / math.sqrt(2.0))

def norm_ppf(p: float) -> float:
    """Standard normal quantile by bisection on ``norm_cdf`` (the same rule as nav_summ.norm_ppf)."""
    if not 0.0 < p < 1.0:
        raise ValueError(f"norm_ppf: p must be in (0, 1), got {p}")
    lo, hi = -40.0, 40.0
    for _ in range(200):
        mid = 0.5 * (lo + hi)
        if norm_cdf(mid) < p:
            lo = mid
        else:
            hi = mid
        if hi - lo <= 1e-15 * max(1.0, abs(mid)):
            break
    return 0.5 * (lo + hi)

def moments(x) -> dict:
    """Per-session SR (ddof 1), skewness and non-excess kurtosis (population moments): nav_summ.net_moments."""
    x = np.asarray(x, dtype=np.float64)
    t = int(x.size)
    e = x - x.mean() if t else x
    m2 = float((e ** 2).mean()) if t else 0.0
    sd = float(x.std(ddof=1)) if t > 1 else 0.0
    return {"sessions": t, "sr_daily": float(x.mean()) / sd if sd > 0 else None,
            "skew": float((e ** 3).mean()) / m2 ** 1.5 if m2 > 0 else None,
            "kurtosis": float((e ** 4).mean()) / m2 ** 2 if m2 > 0 else None}

def expected_max_sr(variance: float, n: int) -> float:
    """SR0 = sqrt(V) [(1 - gamma) PhiInv(1 - 1/N) + gamma PhiInv(1 - 1/(N e))] (Bailey & Lopez de Prado 2014)."""
    if n < 2:
        raise ValueError("expected_max_sr needs N >= 2")
    return math.sqrt(variance) * ((1.0 - EULER_GAMMA) * norm_ppf(1.0 - 1.0 / n) +
                                  EULER_GAMMA * norm_ppf(1.0 - 1.0 / (n * math.e)))

def _denominator(sr: float, skew: float, kurtosis: float) -> float:
    return 1.0 - skew * sr + (kurtosis - 1.0) * sr * sr / 4.0

def psr(sr: float, sr_star: float, t: int, skew: float, kurtosis: float) -> float | None:
    """Probabilistic SR: P[true SR > SR*] (per-session SRs); None when the variance term is not positive or T < 2."""
    denom = _denominator(sr, skew, kurtosis)
    if not (denom > 0 and t > 1):
        return None
    return norm_cdf((sr - sr_star) * math.sqrt(t - 1) / math.sqrt(denom))

def min_trl(sr: float, sr_star: float, skew: float, kurtosis: float, alpha: float = 0.05) -> float | None:
    """Minimum track record length in sessions to reject SR <= SR* at level alpha; None when SR <= SR*."""
    denom = _denominator(sr, skew, kurtosis)
    if not sr > sr_star or not denom > 0:
        return None
    z = norm_ppf(1.0 - alpha)
    return 1.0 + denom * (z / (sr - sr_star)) ** 2

def deflated_sharpe(sr: float, t: int, skew: float, kurtosis: float, sr0: float) -> float | None:
    """DSR = PSR with SR* = SR0 (the expected maximum SR of N trials)."""
    return psr(sr, sr0, t, skew, kurtosis)

def psr_report(x, sr_stars_annual=(0.0, 0.5), alpha: float = 0.05) -> dict:
    """PSR and MinTRL of one daily net series against each annualized benchmark SR*."""
    m = moments(x)
    out = {"sessions": m["sessions"], "sr_daily": m["sr_daily"], "skew": m["skew"], "kurtosis": m["kurtosis"],
           "sr_annual": m["sr_daily"] * math.sqrt(ANNUAL) if m["sr_daily"] is not None else None,
           "alpha": alpha, "benchmarks": []}
    for star in sr_stars_annual:
        s = star / math.sqrt(ANNUAL)
        row = {"sr_star_annual": star, "sr_star_daily": s, "psr": None, "min_trl_sessions": None,
               "min_trl_years": None}
        if m["sr_daily"] is not None and m["skew"] is not None:
            row["psr"] = psr(m["sr_daily"], s, m["sessions"], m["skew"], m["kurtosis"])
            trl = min_trl(m["sr_daily"], s, m["skew"], m["kurtosis"], alpha)
            row["min_trl_sessions"] = trl
            row["min_trl_years"] = trl / ANNUAL if trl is not None else None
        out["benchmarks"].append(row)
    return out

def split_matrix(blocks: int) -> np.ndarray:
    """(C(S, S/2), S) boolean IS masks in itertools.combinations (lexicographic) order."""
    if blocks < 2 or blocks % 2:
        raise ValueError("CSCV needs an even number of blocks >= 2")
    combos = list(itertools.combinations(range(blocks), blocks // 2))
    mask = np.zeros((len(combos), blocks), dtype=bool)
    for i, c in enumerate(combos):
        mask[i, list(c)] = True
    return mask

def _subset_sr(block_sum: np.ndarray, block_sq: np.ndarray, masks: np.ndarray, count: int,
               offset: np.ndarray) -> np.ndarray:
    """Per-split, per-candidate mean / population SD over the union of the masked blocks (0 for a zero SD).

    block sums are of the column-centered values (x - offset), so the variance has no large-offset cancellation."""
    s = masks.astype(np.float64) @ block_sum          # (splits, N)
    q = masks.astype(np.float64) @ block_sq
    m = s / count
    var = np.maximum(q / count - m * m, 0.0)
    mean = offset[None, :] + m
    with np.errstate(invalid="ignore", divide="ignore"):
        sr = np.where(var > 0, mean / np.sqrt(var), 0.0)
    return sr

def cscv_pbo(returns, blocks: int = PBO_BLOCKS, max_splits: int | None = None, seed: int = DEFAULT_SEED) -> dict:
    """PBO by CSCV on a (T, N) matrix of aligned per-session returns (N candidates / cells)."""
    r = np.asarray(returns, dtype=np.float64)
    if r.ndim != 2 or r.shape[1] < 2:
        raise ValueError("cscv_pbo: need a (T, N) matrix with N >= 2")
    t, n = r.shape
    if blocks < 2 or blocks % 2 or blocks > t:
        raise ValueError("cscv_pbo: blocks must be even, >= 2 and <= T")
    width = t // blocks
    used = width * blocks
    x = r[:used]
    if not np.all(np.isfinite(x)):
        raise ValueError("cscv_pbo: nonfinite return in the used sessions")
    offset = x.mean(axis=0)
    xc = (x - offset).reshape(blocks, width, n)
    block_sum, block_sq = xc.sum(axis=1), (xc * xc).sum(axis=1)
    masks = split_matrix(blocks)
    total = masks.shape[0]
    subsampled = False
    if max_splits is not None and 0 < max_splits < total:
        rng = np.random.default_rng(seed)
        masks = masks[np.sort(rng.choice(total, size=max_splits, replace=False))]
        subsampled = True
    count = (blocks // 2) * width
    is_sr = _subset_sr(block_sum, block_sq, masks, count, offset)
    oos_sr = _subset_sr(block_sum, block_sq, ~masks, count, offset)
    winner = np.argmax(is_sr, axis=1)                      # first maximum: ties keep the lower index
    rows = np.arange(masks.shape[0])
    ow = oos_sr[rows, winner]
    idx = np.arange(n)[None, :]
    below = (oos_sr < ow[:, None]) | ((oos_sr == ow[:, None]) & (idx < winner[:, None]))
    below[rows, winner] = False
    rank = below.sum(axis=1)                               # 0 = worst OOS, n - 1 = best OOS
    rel = (rank + 1.0) / (n + 1.0)
    logits = np.log(rel / (1.0 - rel))
    iw = is_sr[rows, winner]
    slope = None
    if masks.shape[0] > 1 and np.var(iw) > 0:
        slope = float(np.cov(iw, ow, ddof=1)[0, 1] / np.var(iw, ddof=1))
    q = np.quantile(logits, [0.05, 0.25, 0.5, 0.75, 0.95])
    ranks = np.bincount(rank, minlength=n)   # the exact logit distribution: logit is a function of the rank alone
    return {"pbo": float(np.mean(logits <= 0.0)), "n_candidates": n, "sessions": t, "blocks": blocks,
            "block_width": width, "dropped_tail_sessions": t - used, "splits": int(masks.shape[0]),
            "splits_total": int(total), "exhaustive": not subsampled, "seed": seed if subsampled else None,
            "logit_mean": float(logits.mean()), "logit_quantiles": dict(zip(("p5", "p25", "p50", "p75", "p95"),
                                                                              map(float, q))),
            "logit_distribution": {"oos_rank": list(range(n)),
                                   "logit": [math.log((k + 1.0) / (n - k)) for k in range(n)],
                                   "splits": [int(c) for c in ranks]},
            "winner_is_sr_annual_mean": float(iw.mean() * math.sqrt(ANNUAL)),
            "winner_oos_sr_annual_mean": float(ow.mean() * math.sqrt(ANNUAL)),
            "prob_winner_oos_loss": float(np.mean(ow < 0.0)),
            "degradation_slope": slope,
            "winner_counts": [int(c) for c in np.bincount(winner, minlength=n)],
            "logits": logits}

def corr_distance(corr) -> np.ndarray:
    c = np.nan_to_num(np.asarray(corr, dtype=np.float64), nan=0.0)
    return np.sqrt(np.clip((1.0 - c) / 2.0, 0.0, None))

def _pairwise(x: np.ndarray) -> np.ndarray:
    g = np.sum(x * x, axis=1)
    d2 = np.maximum(g[:, None] + g[None, :] - 2.0 * (x @ x.T), 0.0)
    np.fill_diagonal(d2, 0.0)
    return np.sqrt(d2)

def kmeans(x: np.ndarray, k: int, rng: np.random.Generator, max_iter: int = 300) -> np.ndarray:
    """Lloyd's k-means with k-means++ seeding (one initialisation); returns labels 0..k-1 (every cluster non-empty)."""
    n = x.shape[0]
    centers = np.empty((k, x.shape[1]))
    centers[0] = x[rng.integers(n)]
    d2 = np.sum((x - centers[0]) ** 2, axis=1)
    for j in range(1, k):
        total = d2.sum()
        pick = int(rng.integers(n)) if total <= 0 else int(rng.choice(n, p=d2 / total))
        centers[j] = x[pick]
        d2 = np.minimum(d2, np.sum((x - centers[j]) ** 2, axis=1))
    labels = np.full(n, -1)
    for _ in range(max_iter):
        dist = np.sum((x[:, None, :] - centers[None, :, :]) ** 2, axis=2)
        new = np.argmin(dist, axis=1)
        for j in range(k):  # an empty cluster takes the point farthest from its current center
            if not np.any(new == j):
                far = int(np.argmax(dist[np.arange(n), new]))
                new[far] = j
        if np.array_equal(new, labels):
            break
        labels = new
        for j in range(k):
            sel = labels == j
            if sel.any():
                centers[j] = x[sel].mean(axis=0)
    return labels

def silhouette_samples(x: np.ndarray, labels: np.ndarray) -> np.ndarray:
    """Euclidean silhouettes of the rows of x (sklearn's definition; a singleton's silhouette is 0)."""
    d = _pairwise(x)
    labels = np.asarray(labels)
    out = np.zeros(len(labels))
    uniq = np.unique(labels)
    for i in range(len(labels)):
        own = labels == labels[i]
        size = int(own.sum())
        if size <= 1 or len(uniq) < 2:
            continue
        a = d[i, own].sum() / (size - 1)
        b = min(d[i, labels == u].mean() for u in uniq if u != labels[i])
        m = max(a, b)
        out[i] = (b - a) / m if m > 0 else 0.0
    return out

def _quality(s: np.ndarray) -> float:
    sd = float(np.std(s))
    mu = float(np.mean(s))
    if sd > 0:
        return mu / sd
    return math.inf if mu > 0 else (-math.inf if mu < 0 else 0.0)

def _groups(labels: np.ndarray, members: list) -> list[list]:
    return [[members[i] for i in np.flatnonzero(labels == u)] for u in np.unique(labels)]

def onc_base(corr: np.ndarray, max_k: int, n_init: int, rng: np.random.Generator):
    """MLAM snippet 4.1: best (mean / std of silhouettes) k-means partition of the distance rows, k = 2..max_k."""
    x = corr_distance(corr)
    best, best_q = None, None
    for _ in range(n_init):
        for k in range(2, max_k + 1):
            labels = kmeans(x, k, rng)
            s = silhouette_samples(x, labels)
            q = _quality(s)
            if best is None or q > best_q:
                best, best_q = (labels, s), q
    return best

def _cluster_tstats(silh: dict, clusters: list[list]) -> list[float]:
    out = []
    for c in clusters:
        v = np.array([silh[m] for m in c])
        sd = float(np.std(v))
        out.append(float(np.mean(v)) / sd if sd > 0 else math.nan)
    return out

def onc(corr, members: list | None = None, max_k: int | None = None, n_init: int = 10,
        seed: int = DEFAULT_SEED, _rng: np.random.Generator | None = None) -> dict:
    """MLAM snippet 4.2 (clusterKMeansTop): returns {"clusters": [[member, ...], ...], "silhouettes": {m: s}}."""
    c = np.asarray(corr, dtype=np.float64)
    n = c.shape[0]
    members = list(range(n)) if members is None else list(members)
    rng = np.random.default_rng(seed) if _rng is None else _rng
    if n < 3:  # k = 2..n-1 is empty: every series is its own cluster
        return {"clusters": [[m] for m in members], "silhouettes": {m: 0.0 for m in members}}
    max_k = n - 1 if max_k is None else min(max_k, n - 1)
    labels, s = onc_base(c, max_k, n_init, rng)
    clusters = _groups(labels, members)
    silh = dict(zip(members, s.tolist()))
    tstats = _cluster_tstats(silh, clusters)
    finite = [t for t in tstats if math.isfinite(t)]
    if not finite:
        return {"clusters": clusters, "silhouettes": silh}
    mean_t = sum(finite) / len(finite)
    redo = [i for i, t in enumerate(tstats) if math.isfinite(t) and t < mean_t]
    keys = [m for i in redo for m in clusters[i]]
    if len(redo) <= 1 or len(keys) < 3:
        return {"clusters": clusters, "silhouettes": silh}
    pos = [members.index(m) for m in keys]
    sub = onc(c[np.ix_(pos, pos)], keys, max_k=min(max_k, len(keys) - 1), n_init=n_init, _rng=rng)
    kept = [clusters[i] for i in range(len(clusters)) if i not in redo]
    new_clusters = kept + sub["clusters"]
    new_labels = np.zeros(n, dtype=int)
    for j, cl in enumerate(new_clusters):
        for m in cl:
            new_labels[members.index(m)] = j
    new_s = silhouette_samples(corr_distance(c), new_labels)
    new_silh = dict(zip(members, new_s.tolist()))
    new_t = [t for t in _cluster_tstats(new_silh, new_clusters) if math.isfinite(t)]
    old_mean = float(np.mean([tstats[i] for i in redo]))
    if not new_t or float(np.mean(new_t)) <= old_mean:
        return {"clusters": clusters, "silhouettes": silh}
    return {"clusters": new_clusters, "silhouettes": new_silh}

def ivp_series(returns: np.ndarray, columns: list[int]) -> np.ndarray:
    """Inverse-variance-weighted mix of the given columns (equal weights if every variance is zero)."""
    sub = returns[:, columns]
    v = sub.var(axis=0, ddof=1) if sub.shape[0] > 1 else np.zeros(len(columns))
    w = np.where(v > 0, 1.0 / np.where(v > 0, v, 1.0), 0.0)
    w = w / w.sum() if w.sum() > 0 else np.full(len(columns), 1.0 / len(columns))
    return sub @ w

def effective_trials(returns, names: list, n_init: int = 10, seed: int = DEFAULT_SEED) -> dict:
    """ONC on the (T, N) aligned series -> N_eff clusters, per-cluster IVP series SRs and V[SR_k] (ddof 1)."""
    r = np.asarray(returns, dtype=np.float64)
    if r.ndim != 2 or r.shape[1] != len(names) or r.shape[0] < 3:
        raise ValueError("effective_trials: need a (T >= 3, N) matrix and N names")
    with np.errstate(invalid="ignore", divide="ignore"):
        corr = np.corrcoef(r.T) if r.shape[1] > 1 else np.ones((1, 1))
    corr = np.nan_to_num(np.atleast_2d(corr), nan=0.0)
    np.fill_diagonal(corr, 1.0)
    res = onc(corr, list(range(len(names))), n_init=n_init, seed=seed)
    clusters = res["clusters"]
    srs = []
    for cl in clusters:
        m = moments(ivp_series(r, cl))
        srs.append(m["sr_daily"])
    defined = [s for s in srs if s is not None]
    var = float(np.var(defined, ddof=1)) if len(defined) >= 2 else None
    return {"n_trials": len(names), "n_eff": len(clusters), "sessions": int(r.shape[0]),
            "clusters": [[names[i] for i in cl] for cl in clusters], "cluster_sr_daily": srs,
            "variance_sr": var, "n_init": n_init, "seed": seed,
            "mean_offdiag_corr": float((corr.sum() - len(names)) / max(len(names) * (len(names) - 1), 1))}

def effective_n_dsr(cell: dict, eff: dict) -> dict:
    """DSR of one cell (its net_moments) with N = N_eff and V[SR_k] of the cluster series."""
    k, var = eff["n_eff"], eff["variance_sr"]
    row = {"n_eff": k, "n_trials": eff["n_trials"], "variance_sr": var, "dsr": None, "sr0_daily": None,
           "sr0_annual": None,
           "variance_source": f"sample variance of {k} ONC cluster IVP-series per-session SRs "
                              f"({eff['n_trials']} trial series, {eff['sessions']} common sessions)"}
    sr, t = cell.get("sr_daily"), cell.get("sessions")
    if sr is None or cell.get("skew") is None or var is None or k < 2 or not t or t < 2:
        row["variance_source"] += "; undefined (N_eff < 2 or no defined SR)"
        return row
    sr0 = expected_max_sr(var, k)
    row.update(sr0_daily=sr0, sr0_annual=sr0 * math.sqrt(ANNUAL),
               dsr=deflated_sharpe(sr, t, cell["skew"], cell["kurtosis"], sr0))
    return row

def lo_null_dsr(cell: dict, n: int) -> dict:
    """Single-cell DSR under the Lo (2002) sampling variance (1 + SR^2 / 2) / T with N = n (nav_summ's 1-dir rule)."""
    sr, t = cell.get("sr_daily"), cell.get("sessions")
    row = {"n": n, "dsr": None, "sr0_daily": None, "sr0_annual": None, "variance_sr": None,
           "variance_source": "Lo (2002) sampling variance (1 + SR^2/2)/T: single cell"}
    if sr is None or cell.get("skew") is None or not t or t < 2 or n < 2:
        return row
    var = (1.0 + sr * sr / 2.0) / t
    sr0 = expected_max_sr(var, n)
    row.update(variance_sr=var, sr0_daily=sr0, sr0_annual=sr0 * math.sqrt(ANNUAL),
               dsr=deflated_sharpe(sr, t, cell["skew"], cell["kurtosis"], sr0))
    return row


# ------------------------------------------------------------------ nav_summ.py (net_moments and the paired dSR)
def sharpe(x: np.ndarray) -> float:
    x = np.asarray(x, dtype=np.float64)
    sd = x.std(ddof=1) if x.size > 1 else 0.0
    return float(x.mean() / sd * math.sqrt(ANNUAL)) if sd > 0 else float("nan")

def memmel_se(sr_a: float, sr_b: float, rho: float, t: int) -> float:
    """Annualized SE of SR_a - SR_b (Memmel 2003 correction of Jobson-Korkie); SRs annualized in, per-session inside."""
    a, b = sr_a / math.sqrt(ANNUAL), sr_b / math.sqrt(ANNUAL)
    var = (2.0 - 2.0 * rho + 0.5 * (a * a + b * b - 2.0 * a * b * rho * rho)) / t
    return math.sqrt(max(var, 0.0) * ANNUAL)

def cbb_indices(t: int, draws: int, block: int, rng: np.random.Generator) -> np.ndarray:
    """(draws, t) circular block bootstrap indices: ceil(t / block) uniform starts, blocks wrap, truncated to t."""
    blocks = -(-t // block)
    starts = rng.integers(0, t, size=(draws, blocks))
    idx = (starts[:, :, None] + np.arange(block)[None, None, :]).reshape(draws, blocks * block)[:, :t]
    return idx % t

def _dsr_population(m: np.ndarray):
    """dSR (per session, population SD) and its gradient in (mu_a, mu_b, E a^2, E b^2); m has shape (..., 4)."""
    mu_a, mu_b, g_a, g_b = m[..., 0], m[..., 1], m[..., 2], m[..., 3]
    va, vb = g_a - mu_a * mu_a, g_b - mu_b * mu_b
    with np.errstate(invalid="ignore", divide="ignore"):
        d = mu_a / np.sqrt(va) - mu_b / np.sqrt(vb)
        grad = np.stack([g_a / va ** 1.5, -g_b / vb ** 1.5, -mu_a / (2 * va ** 1.5), mu_b / (2 * vb ** 1.5)], axis=-1)
    return d, grad

def bartlett_psi(y: np.ndarray, lag: int) -> np.ndarray:
    """Bartlett-kernel HAC long-run covariance of the rows of y (T x k), autocovariances / T."""
    e = y - y.mean(axis=0)
    t = e.shape[0]
    psi = e.T @ e / t
    for ell in range(1, min(lag, t - 1) + 1):
        g = e[ell:].T @ e[:-ell] / t
        psi += (1.0 - ell / (lag + 1.0)) * (g + g.T)
    return psi

def paired_stats(a: np.ndarray, b: np.ndarray, draws: int = DEFAULT_DRAWS, block: int = DEFAULT_BLOCK,
                 seed: int = DEFAULT_SEED, one_sided: bool = False) -> dict:
    """Paired dSR(net) = SR(a) - SR(b) on aligned daily nets: Memmel SE, CBB percentile CI, LW studentized CI.

    ``one_sided`` adds lw["p_one_sided"] (H0: dSR <= 0) from the same resamples: the share of signed studentized draws
    (ds - d0) / ss at or above d0 / s0, (k + 1) / (valid + 1); without it the result is unchanged."""
    a, b = np.asarray(a, dtype=np.float64), np.asarray(b, dtype=np.float64)
    t = a.size
    if t != b.size or t < 3:
        raise ValueError("paired_stats: need two aligned series of >= 3 sessions")
    sr_a, sr_b = sharpe(a), sharpe(b)
    sa, sb = a.std(ddof=1), b.std(ddof=1)
    rho = float(np.corrcoef(a, b)[0, 1]) if sa > 0 and sb > 0 else float("nan")
    dsr = sr_a - sr_b
    se = memmel_se(sr_a, sr_b, rho, t)
    rng = np.random.default_rng(seed)
    idx = cbb_indices(t, draws, block, rng)
    xa, xb = a[idx], b[idx]
    with np.errstate(invalid="ignore", divide="ignore"):
        boot = (xa.mean(axis=1) / xa.std(axis=1, ddof=1) - xb.mean(axis=1) / xb.std(axis=1, ddof=1)) * math.sqrt(ANNUAL)
    ok = np.isfinite(boot)
    lo, hi = np.quantile(boot[ok], [0.025, 0.975]) if ok.any() else (float("nan"), float("nan"))
    # Ledoit-Wolf studentized: population-moment dSR, delta-method SE
    y = np.column_stack([a, b, a * a, b * b])
    d0, g0 = _dsr_population(y.mean(axis=0))
    d0 = float(d0)
    s0 = math.sqrt(max(float(g0 @ bartlett_psi(y, block - 1) @ g0), 0.0) / t) if np.all(np.isfinite(g0)) else 0.0
    ys = y[idx]                                                  # draws x t x 4
    ms = ys.mean(axis=1)
    ds, gs = _dsr_population(ms)
    blocks = -(-t // block)
    padded = np.zeros((draws, blocks * block, 4))
    padded[:, :t] = ys - ms[:, None, :]
    sums = padded.reshape(draws, blocks, block, 4).sum(axis=2)  # block sums S_j of the centered resample
    psi = np.einsum("dni,dnj->dij", sums, sums) / t
    with np.errstate(invalid="ignore", divide="ignore"):
        ss = np.sqrt(np.einsum("di,dij,dj->d", gs, psi, gs) / t)
        tstar = np.abs(ds - d0) / ss
    good = np.isfinite(tstar) & (ss > 0)
    lw = {"dsr_population": d0 * math.sqrt(ANNUAL), "se": s0 * math.sqrt(ANNUAL), "ci95": [None, None],
          "p_value": None, "valid_draws": int(good.sum())}
    if good.any() and s0 > 0 and math.isfinite(d0):
        c = float(np.quantile(tstar[good], 0.95))
        lw["ci95"] = [(d0 - c * s0) * math.sqrt(ANNUAL), (d0 + c * s0) * math.sqrt(ANNUAL)]
        lw["p_value"] = (int((tstar[good] >= abs(d0) / s0).sum()) + 1) / (int(good.sum()) + 1)
    if one_sided:
        lw["p_one_sided"] = None
        if good.any() and s0 > 0 and math.isfinite(d0):
            signed = (ds[good] - d0) / ss[good]
            lw["p_one_sided"] = (int((signed >= d0 / s0).sum()) + 1) / (int(good.sum()) + 1)
    return {"sessions": t, "sr": sr_a, "sr_reference": sr_b, "dsr": dsr, "rho": rho, "memmel_se": se,
            "t": dsr / se if se > 0 else None, "cbb_ci95": [float(lo), float(hi)], "cbb_valid_draws": int(ok.sum()),
            "lw": lw, "draws": draws, "block": block, "seed": seed}

def net_moments(x: np.ndarray) -> dict:
    """Per-session Sharpe (ddof 1), skewness and non-excess kurtosis (population moments) of the daily nets."""
    x = np.asarray(x, dtype=np.float64)
    t = int(x.size)
    e = x - x.mean() if t else x
    m2 = float((e ** 2).mean()) if t else 0.0
    sd = float(x.std(ddof=1)) if t > 1 else 0.0
    return {"sessions": t, "sr_daily": float(x.mean()) / sd if sd > 0 else None,
            "skew": float((e ** 3).mean()) / m2 ** 1.5 if m2 > 0 else None,
            "kurtosis": float((e ** 4).mean()) / m2 ** 2 if m2 > 0 else None}

def dsr_rows(moments: list[dict], n: int) -> list[dict]:
    """DSR per NAV dir; V[SR_n] from the dirs' per-session SRs (ddof 1), or Lo (2002) with a single dir."""
    if n < 2:
        raise ValueError("DSR needs N >= 2")
    srs = [m["sr_daily"] for m in moments if m["sr_daily"] is not None]
    cross = len(moments) >= 2 and len(srs) >= 2
    v_cross = float(np.var(srs, ddof=1)) if cross else None
    out = []
    for m in moments:
        sr, t = m["sr_daily"], m["sessions"]
        row = {"n": n, "sessions": t, "sr_daily": sr, "skew": m["skew"], "kurtosis": m["kurtosis"],
               "sr_annual": sr * math.sqrt(ANNUAL) if sr is not None else None}
        if sr is None or m["skew"] is None or t < 2:
            out.append(dict(row, dsr=None, sr0_daily=None, sr0_annual=None, variance_sr=None,
                            variance_source="undefined (constant or too short net series)"))
            continue
        if cross:
            variance, source = v_cross, f"cross-cell sample variance of {len(srs)} per-session SRs"
        else:
            variance, source = (1.0 + sr * sr / 2.0) / t, "Lo (2002) sampling variance (1 + SR^2/2)/T: single cell"
        sr0 = expected_max_sr(variance, n)
        out.append(dict(row, dsr=deflated_sharpe(sr, t, m["skew"], m["kurtosis"], sr0), sr0_daily=sr0,
                        sr0_annual=sr0 * math.sqrt(ANNUAL), variance_sr=variance, variance_source=source))
    return out


# ------------------------------------------------------------------ dsr_total.py
def trial_breakdown(records: list[dict]) -> dict:
    """N_tot of the lines and its parts: the trials of each kind by the defect rule (``trial_counts``; a line
    without a kind is a construction line, as in ``ledger_n``), and the campaigns' registry counts."""
    counts = BI.trial_counts(records)
    by_kind: dict[str, int] = {}
    for rec, c in zip(records, counts):
        if c:
            kind = rec.get("kind", CONSTRUCTION)
            by_kind[kind] = by_kind.get(kind, 0) + c
    m = BI.campaign_registry_count(records)
    return {"by_kind": dict(sorted(by_kind.items())), "ledger_trials": sum(counts), "campaign_registry": m,
            "n_tot": sum(counts) + m}

def dsr_at(moments: dict, n: int, variance: float | None) -> dict:
    """SR0 and DSR of one book at N trials and cross-trial variance V (per-session units). Values are None when V is
    undefined (fewer than two cells on the window), when the series has no defined moments, or when N < 2."""
    row = {"n": n, "sr0_daily": None, "sr0_annual": None, "dsr": None}
    sr, t, skew, kurt = moments["sr_daily"], moments["sessions"], moments["skew"], moments["kurtosis"]
    if variance is None or sr is None or skew is None or kurt is None or t < 2 or n < 2:
        return row
    sr0 = expected_max_sr(variance, n)
    row.update(sr0_daily=sr0, sr0_annual=sr0 * math.sqrt(ANNUAL),
               dsr=deflated_sharpe(sr, t, skew, kurt, sr0))
    return row

def ledger_rows(moments: dict, records: list[dict], in_ledger: bool, window_id: str) -> dict:
    """The ``tot`` and ``v8`` rows of one book over these ledger lines, with N_tot's parts and V.

    ``in_ledger`` says whether the book has a construction line among ``records``. When it has none, both counts add
    1 for the book itself, as ledger_n does."""
    v = BI.dsr_variance(records, window_id)
    parts = trial_breakdown(records)
    n_tot = parts["n_tot"] + (0 if in_ledger else 1)
    n_c = BI.ledger_n(records, in_ledger)
    return {"window_id": window_id, "in_ledger": in_ledger, "variance_sr": v["variance_sr"], "cells_in_v": v["cells"],
            "variance_source": v["variance_source"], "parts": parts,
            "tot": dsr_at(moments, n_tot, v["variance_sr"]), "v8": dsr_at(moments, n_c, v["variance_sr"])}
