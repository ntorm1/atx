"""Backtest-integrity statistics for nav_summ (platform v7, lane L2; pure Python + numpy, no scipy/sklearn).

  trial ledger    atx.trial-ledger/v1 JSON lines (R5.1): one line per cell/run with its kind (admission, composition,
                  construction, universe, data; mining-campaign is a campaign line, below), cell dir, pins, window,
                  daily net series path + SHA-256 and S2 net SR.
                  Appends are idempotent on (kind, daily series SHA-256): an identity re-run (the same daily net
                  series under any cell name, e.g. a research_cycle --suffix re-run) never adds a trial.
                  Only TRAIN series (every session before the research window's TRAIN end, read from
                  atx-engine/tools/research_window.py) are accepted; anything later is refused.
  v8 fields       (platform v8 V-1; optional, so every older line stays valid and its trial_id rule is unchanged)
                  origin (contract K5: prior | grid | mined), window_id (the research window the series was scored
                  on), defect {invalid, reason}, rerun_of (a trial_id) with rerun_basis window | blind | returns.
  trial count     (v8-prereg items 2 and 7) a line adds its count to N except: kind protocol (W0-3), window
                  re-runs (a ledgered cell re-run on a longer window) and blind re-runs add 0; an invalid cell (defect)
                  adds 0 unless a re-run names it. A re-run never lowers N (review C-5): the replaced invalid cell
                  stays counted and its blind re-run adds 0; a re-run decided because the returns looked wrong is a
                  new trial beside it. Without the v8 fields every line adds its count, as before.
  defect line     (review C-3) kind defect, count 0, defect_of = a ledgered cell's trial_id, reason: that cell is
                  invalid, found after it was ledgered (research_cycle.py ledger-defect). A cell line whose trial_id is
                  ledgered already is skipped only when its defect / re-run flags equal the ledgered line's; flags that
                  would be dropped are refused (ledger_append raises, nav_summ exits non-zero).
  re-run check    (review C-4) rerun_of must be the trial_id of an earlier cell line of the same kind; a window
                  re-run's target is on another window_id (or has none); (review C-5) a blind or returns re-run's
                  target is invalid on an earlier line (its defect flag or a defect line).
  protocol line   (W0-3; written by research_cycle.py ledger-protocol, lane A) kind protocol, count 0, no cell, no
                  series: every reader here skips it when it lists cells or counts N; the hash chain covers it.
  validation line (review C-11; written by holdout_gate.py) kind validation, count 0: a hidden-block read, citing
                  the owner ruling's path and SHA-256. An event line like protocol and defect.
  campaign line   (Ruling E-33; campaign_line) kind mining-campaign, count 0, origin mined: a mining campaign's
                  registry {path, chain_head, count}. It adds 0 to every N (the construction N included); its
                  registry count is the campaign's own budget (pre-registration rule 10) and is printed beside N.
  hash chain      (v8; review C-6) a running head from 64 zeros: a legacy (unchained) line folds into it in its stored
                  form, a chained line's prev_sha256 must equal it and the line's own SHA-256 becomes the head. So the
                  first chained line pins every legacy line before it (the 37 v7 cells), an edited, inserted or
                  removed line before the last chained line breaks the chain, and an unchained line after a chained
                  one is refused. ledger_head is recorded by every cycle verdict and ledger copy (the tail).
  DSR variance   (v8-prereg item 3, OD-4) N = the construction trial count; V[SR] = sample variance of the
                  per-session S2 net SRs of the construction lines scored on the current window (re-runs and new
                  cells; invalid or replaced lines left out); the legacy variance (lines without a window_id, the
                  2020-2022 cells) is reported beside it and gates nothing.
  effective N     ONC clustering (Lopez de Prado & Lewis 2019, QF 19(9); code as Lopez de Prado 2020, "Machine
                  Learning for Asset Managers", snippets 4.1-4.2) of the trials' daily net series on their common
                  sessions: distance sqrt((1 - rho) / 2), k-means over its rows for k = 2..N-1 with n_init seeded
                  restarts, quality = mean / std of the silhouettes, then the recursive re-cluster of below-average
                  clusters. N_eff = number of clusters. Each cluster's series is the inverse-variance-weighted mix of its
                  members (MLAM section 8.8); V[SR_k] = sample variance (ddof 1) of the per-session SRs of the cluster
                  series; SR0 and DSR as nav_summ's (Bailey & Lopez de Prado 2014) with N = N_eff.
  CSCV PBO        (Bailey, Borwein, Lopez de Prado & Zhu 2017, J. Computational Finance 20(4)) S blocks (default 16) of
                  floor(T / S) sessions (the trailing T mod S sessions are dropped, as atx-engine eval/pbo.cpp does),
                  every C(S, S/2) split (12,870 for S = 16; exhaustive by default, a seeded subsample only when asked
                  and then flagged), IS winner = argmax IS SR, its OOS relative rank w = (r + 1) / (N + 1),
                  logit = ln(w / (1 - w)), PBO = share of splits with logit <= 0.
  PSR / MinTRL    (Bailey & Lopez de Prado 2012, J. of Risk 15(2)) PSR = Phi[(SR - SR*) sqrt(T - 1) / sqrt(1 - g3 SR +
                  (g4 - 1) SR^2 / 4)]; MinTRL = 1 + (1 - g3 SR + (g4 - 1) SR^2 / 4) (z_{1-alpha} / (SR - SR*))^2
                  sessions, per-session SRs, g3 skewness, g4 non-excess kurtosis (population moments).

Divergences, stated: (i) SR inside CSCV is mean / population SD with 0 for a zero SD (eval/pbo.cpp's convention; the
paper leaves the metric open); ties in the IS argmax keep the lower index and an OOS tie ranks the lower index below
(pbo.cpp CachedMomentsV2). (ii) ONC: a cluster whose silhouettes have zero dispersion (a singleton) has an undefined
t-stat; MLAM's code lets that NaN silently switch the recursion off, here such clusters are kept as they are and left
out of the mean t-stat (the recursion still runs for the others); a re-cluster needs >= 3 members; k = 1 is never
tried (as MLAM). k-means is Lloyd's algorithm with k-means++ seeding from a numpy Generator(seed), not sklearn's.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import itertools
import json
import math
from pathlib import Path
import sys

import numpy as np

ANNUAL = 252
EULER_GAMMA = 0.5772156649015329
DEFAULT_SEED = 20260927
LEDGER_SCHEMA = "atx.trial-ledger/v1"
MINING_CAMPAIGN = "mining-campaign"       # Ruling E-33: a mined campaign's line; adds 0, carries its registry count
LEDGER_KINDS = ("admission", "composition", "construction", "universe", "data", MINING_CAMPAIGN)
ZERO_TRIAL_KINDS = ("protocol",)          # W0-3: the protocol line records the window change and adds no trial
DEFECT = "defect"                         # review C-3: a defect event line marks a ledgered cell invalid afterwards
VALIDATION = "validation"                 # review C-11: holdout_gate's record of a hidden-block read (cites its ruling)
EVENT_KINDS = ZERO_TRIAL_KINDS + (DEFECT, VALIDATION)  # event lines: no cell, no series, add no trial
FLAG_KEYS = ("defect", "rerun_of", "rerun_basis")   # a cell line's v8 flags (never part of its trial_id)
ORIGINS = ("prior", "grid", "mined")      # contract K5: the registry's origin class, copied into every ledger line
RERUN_BASES = ("window", "blind", "returns")
PRIOR_VALIDATION_READS = "2 (2023-2024)"  # v8-prereg item 1 disclosure (history before v8), not a window constant
DAY_NS = 86_400_000_000_000
PBO_BLOCKS = 16
PBO_MIN_CELLS = 4
POOL_WINDOW = "POOL"                      # task H-1: the window label of a pooled era line
ENGINE_TOOLS = Path(__file__).resolve().parents[2] / "atx-engine" / "tools"


def research_window():
    """The ``research_window`` module (task W0-1, atx-engine/tools): the one source of TRAIN and the seal.

    Loaded as W0-1 prescribes for atx-impl tools, ``from engine_tools import research_window``: a private instance
    (``atx_impl_engine_tools_research_window``) that a test harness rebinding the atx-engine/tools instance cannot
    change. Before engine_tools.py is on the branch, the same private instance is loaded here the same way."""
    try:
        from engine_tools import research_window as rw
        return rw
    except ImportError:
        pass
    return _engine_module("research_window")


def era_pool():
    """The ``era_pool`` module (task H-1, atx-engine/tools): the era pooling rules, loaded as the window is."""
    return _engine_module("era_pool")


def _engine_module(name: str):
    """``atx-engine/tools/<name>.py`` as the private module ``atx_impl_engine_tools_<name>`` (engine_tools.py's
    naming, so both loaders share one instance)."""
    import importlib.util
    private = f"atx_impl_engine_tools_{name}"
    if private in sys.modules:
        return sys.modules[private]
    spec = importlib.util.spec_from_file_location(private, ENGINE_TOOLS / f"{name}.py")
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load {ENGINE_TOOLS / f'{name}.py'}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[private] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        del sys.modules[private]
        raise
    return module


def window_id() -> str:
    """The research window's id (``atx.research-window/v2`` -> ``research-window-v2``)."""
    rw = research_window()
    wid = getattr(rw, "WINDOW_ID", None)
    return wid if wid else rw.load()["schema"].split(".", 1)[1].replace("/", "-")


def refuse_sealed(sessions, what: str) -> None:
    """Raise (the window's SealError, a ValueError) when any session (ns) is at or after the research seal."""
    rw = research_window()
    if len(sessions) and rw.is_sealed(int(max(sessions))):
        error = getattr(rw, "SealError", ValueError)
        raise error(f"{what}: a session at or after the research seal {rw.SEAL_DATE} ({window_id()}); refusing "
                    "(sealed)")


# ------------------------------------------------------------------ normal distribution (no scipy)
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


# ------------------------------------------------------------------ moments, DSR pieces, PSR, MinTRL
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


# ------------------------------------------------------------------ CSCV probability of backtest overfitting
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


def cscv_pbo_reference(returns, blocks: int = PBO_BLOCKS) -> list[float]:
    """Slow literal CSCV (one gather per split and candidate): the oracle the vectorised form is tested against."""
    r = np.asarray(returns, dtype=np.float64)
    t, n = r.shape
    width = t // blocks
    out = []
    for combo in itertools.combinations(range(blocks), blocks // 2):
        oos = [b for b in range(blocks) if b not in combo]

        def sr(col, subset):
            v = np.concatenate([r[b * width:(b + 1) * width, col] for b in subset])
            sd = v.std()
            return v.mean() / sd if sd > 0 else 0.0
        is_sr = [sr(c, combo) for c in range(n)]
        oos_sr = [sr(c, oos) for c in range(n)]
        w = int(np.argmax(is_sr))
        rank = sum(1 for c in range(n) if c != w and (oos_sr[c] < oos_sr[w] or (oos_sr[c] == oos_sr[w] and c < w)))
        rel = (rank + 1.0) / (n + 1.0)
        out.append(math.log(rel / (1.0 - rel)))
    return out


# ------------------------------------------------------------------ ONC clustering (Lopez de Prado & Lewis 2019)
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


# ------------------------------------------------------------------ alignment
def align_many(series: list[dict]) -> tuple[list[int], np.ndarray]:
    """Common sessions (sorted session_ns) of several {session_ns: net} maps and the (T, N) matrix on them."""
    if not series:
        return [], np.zeros((0, 0))
    common = set(series[0])
    for s in series[1:]:
        common &= set(s)
    sessions = sorted(common)
    return sessions, np.array([[s[k] for s in series] for k in sessions], dtype=np.float64).reshape(len(sessions),
                                                                                                    len(series))


# ------------------------------------------------------------------ trial ledger (R5.1)
def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def session_date(ns: int) -> dt.date:
    return dt.date(1970, 1, 1) + dt.timedelta(days=int(ns) // DAY_NS)


def window_of(sessions: list[int], era: str | None = None, pool: bool = False) -> dict:
    """The series window; refuses any session outside the research window's TRAIN [begin, end) (the ledger records
    TRAIN trials only; a series ending before the TRAIN end, e.g. a 2020-2022 cell, is a TRAIN series).

    Era rule (task H-1): an era shard (``era`` = its id, label ``ERA <id>``) or a pooled series (``pool``, label
    ``POOL``) may begin before TRAIN, but every session must still lie before the TRAIN end."""
    if not sessions:
        raise ValueError("ledger: empty net series")
    rw = research_window()
    first, last = session_date(min(sessions)), session_date(max(sessions))
    if era is not None or pool:
        if max(sessions) >= rw.TRAIN_END_NS:
            raise ValueError(f"ledger: {'pooled' if pool else f'era {era}'} series ends {last.isoformat()}, on or "
                             f"after the TRAIN end {session_date(rw.TRAIN_END_NS).isoformat()} of {window_id()}")
        return {"label": POOL_WINDOW if pool else f"ERA {era}", "first_session": first.isoformat(),
                "last_session": last.isoformat(), "sessions": len(sessions)}
    if max(sessions) >= rw.TRAIN_END_NS or min(sessions) < rw.TRAIN_BEGIN_NS:
        span = f"[{session_date(rw.TRAIN_BEGIN_NS).isoformat()}, {session_date(rw.TRAIN_END_NS).isoformat()})"
        raise ValueError(f"ledger: series spans {first.isoformat()}..{last.isoformat()}, outside TRAIN {span} of "
                         f"{window_id()}; only TRAIN series are ledgered by this tool")
    return {"label": "TRAIN", "first_session": first.isoformat(), "last_session": last.isoformat(),
            "sessions": len(sessions)}


def summary_pins(summary: dict) -> dict:
    pins = {k: v for k, v in summary.items() if k.endswith("_sha256") and isinstance(v, str)}
    sb = summary.get("source_bindings") or {}
    for k in ("library_sha256", "research_fields_manifest_sha256", "orientation_candidates_sha256",
              "run_recipe_sha256"):
        if isinstance(sb.get(k), str):
            pins[f"source_bindings.{k}"] = sb[k]
    return pins


def ledger_record(kind: str, cell: str, summary_path: Path, daily_path: Path, scenario: str, nets: dict,
                  net_sharpe, *, count: int = 1, note: str | None = None, run: dict | None = None,
                  origin: str | None = None, research_window_id: str | None = None, rerun_of: str | None = None,
                  rerun_basis: str | None = None, defect: str | None = None, era: dict | None = None,
                  era_of: str | None = None) -> dict:
    """One ledger line for a NAV cell (its primary/selected scenario's daily net series).

    The v8 keyword fields are written only when given (a line without them is byte-identical to the v7 layout):
    ``origin`` (K5), ``research_window_id`` (key ``window_id``), ``rerun_of`` + ``rerun_basis`` and
    ``defect`` (the invalid-cell reason). None of them enters ``trial_id``.

    Task H-1: ``era`` = {id, role_sha256} marks an era shard (window rule ``ERA <id>``, see ``window_of``);
    ``era_of`` = the pooled trial_id it belongs to (an era shard of a pooled cell adds no trial)."""
    check_record_fields(kind, count, origin, rerun_of, rerun_basis, defect)
    summary = json.loads(Path(summary_path).read_text(encoding="utf-8"))
    series_sha = sha256_file(daily_path)
    window = window_of(list(nets), era=era["id"] if era else None)
    pins = dict(summary_pins(summary), summary_sha256=sha256_file(summary_path))
    rec = {"schema": LEDGER_SCHEMA, "kind": kind, "count": count, "cell": str(cell).replace("\\", "/"),
           "window": window, "scenario": scenario,
           "series": {"path": str(daily_path).replace("\\", "/"), "sha256": series_sha, "column": "net_return",
                      "rows": "return_observation == 1, first CSV row excluded (nav_summ return rows)"},
           "pins": pins, "s2_net_sr": net_sharpe}
    rec.update(ledger_record_fields(note, run, origin, research_window_id, rerun_of, rerun_basis, defect))
    if era is not None:
        rec["era"] = {"id": era["id"], "role_sha256": era.get("role_sha256")}
    if era_of is not None:
        rec["era_of"] = era_of
    rec["trial_id"] = trial_id(kind, series_sha)
    return rec


def check_record_fields(kind: str, count: int, origin: str | None = None, rerun_of: str | None = None,
                        rerun_basis: str | None = None, defect: str | None = None) -> None:
    """The refusals of a ledger line's kind, count and v8 fields (ledger_record, ledger_pool_records)."""
    if kind not in LEDGER_KINDS:
        raise ValueError(f"ledger: kind must be one of {', '.join(LEDGER_KINDS)}")
    if kind == MINING_CAMPAIGN:
        raise ValueError(f"ledger: a {MINING_CAMPAIGN} line records a campaign, not a NAV cell (campaign_line)")
    if not isinstance(count, int) or count < 1:
        raise ValueError("ledger: count must be a positive integer")
    if origin is not None and origin not in ORIGINS:
        raise ValueError(f"ledger: origin must be one of {', '.join(ORIGINS)} (contract K5), got {origin!r}")
    if (rerun_of is None) != (rerun_basis is None) or (rerun_basis is not None and rerun_basis not in RERUN_BASES):
        raise ValueError(f"ledger: rerun_of and rerun_basis ({', '.join(RERUN_BASES)}) go together")
    if defect is not None and not (isinstance(defect, str) and defect.strip()):
        raise ValueError("ledger: a defect needs a reason")


def is_era_line(rec: dict) -> bool:
    """An era shard line of a pooled cell (``era_of``): recorded for audit, adds no trial."""
    return "era_of" in rec


def is_pool_line(rec: dict) -> bool:
    """A pooled era line (window POOL): one trial for all its eras."""
    return (rec.get("window") or {}).get("label") == POOL_WINDOW


def pooled_trial_id(kind: str, series_sha256s: list[str]) -> str:
    """The trial_id of a pool of era series (date order): one era is that era's trial_id (era_pool.pooled_sha256)."""
    return trial_id(kind, era_pool().pooled_sha256(series_sha256s))


def ledger_pool_records(kind: str, eras: list[dict], pooled_nets: dict, pooled_sr, *, count: int = 1,
                        note: str | None = None, run: dict | None = None, **v8) -> list[dict]:
    """The ledger lines of a pooled era cell (task H-1: an era shard of a registered cell is one trial in total).

    ``eras`` (date order): {id, role_sha256, cell, summary_path, daily_path, scenario, nets, net_sharpe}. One era is a
    single line, the era's own (``ledger_record``; the era block and the ``ERA`` window only when the series begins
    before TRAIN). Two or more: one line per era (``era``, ``era_of`` = the pooled trial_id; adds 0; it carries the
    origin and window id only) and one pooled line (window ``POOL``, ``series.pool[]``, ``eras[]``, ``s2_net_sr`` =
    ``pooled_sr``; adds ``count``; it carries every v8 field, re-run and defect included: it is the trial)."""
    if not eras:
        raise ValueError("ledger: a pool needs an era")
    check_record_fields(kind, count, **{k: v for k, v in v8.items() if k != "research_window_id"})
    shas = [sha256_file(e["daily_path"]) for e in eras]
    if len(eras) == 1:
        e = eras[0]
        history = min(e["nets"]) < research_window().TRAIN_BEGIN_NS
        return [ledger_record(kind, e["cell"], e["summary_path"], e["daily_path"], e["scenario"], e["nets"],
                              e["net_sharpe"], era={"id": e["id"], "role_sha256": e.get("role_sha256")}
                              if history else None, count=count, note=note, run=run, **v8)]
    tid = pooled_trial_id(kind, shas)
    audit = {k: v for k, v in v8.items() if k in ("origin", "research_window_id")}
    lines = [ledger_record(kind, e["cell"], e["summary_path"], e["daily_path"], e["scenario"], e["nets"],
                           e["net_sharpe"], era={"id": e["id"], "role_sha256": e.get("role_sha256")}, era_of=tid,
                           count=count, note=note, run=run, **audit) for e in eras]
    scenarios = sorted({e["scenario"] for e in eras})
    if len(scenarios) != 1:
        raise ValueError(f"ledger: the eras of a pool are scored on different scenarios {scenarios}")
    dirs = [str(e["cell"]).replace("\\", "/") for e in eras]
    rec = {"schema": LEDGER_SCHEMA, "kind": kind, "count": count, "cell": era_pool().pool_label(dirs),
           "window": window_of(list(pooled_nets), pool=True), "scenario": scenarios[0],
           "series": {"pool": [{"id": e["id"], "path": str(e["daily_path"]).replace("\\", "/"), "sha256": s}
                               for e, s in zip(eras, shas)],
                      "sha256": era_pool().pooled_sha256(shas), "column": "net_return",
                      "rows": "each era's return rows (nav_summ), concatenated in date order"},
           "eras": [{"id": e["id"], "role_sha256": e.get("role_sha256"), "cell": d, "trial_id": line["trial_id"],
                     "series_sha256": s} for e, d, line, s in zip(eras, dirs, lines, shas)],
           "pins": {}, "s2_net_sr": pooled_sr}
    rec.update(ledger_record_fields(note=note, run=run, **v8))
    rec["trial_id"] = tid
    return lines + [rec]


def ledger_record_fields(note: str | None = None, run: dict | None = None, origin: str | None = None,
                         research_window_id: str | None = None, rerun_of: str | None = None,
                         rerun_basis: str | None = None, defect: str | None = None) -> dict:
    """The optional ledger_record keys (note, recorded_by and the v8 fields) of a line built outside ledger_record."""
    out: dict = {}
    if note:
        out["note"] = note
    if run:
        out["recorded_by"] = run
    if origin is not None:
        out["origin"] = origin
    if research_window_id is not None:
        out["window_id"] = research_window_id
    if rerun_of is not None:
        out["rerun_of"], out["rerun_basis"] = rerun_of, rerun_basis
    if defect is not None:
        out["defect"] = {"invalid": True, "reason": defect}
    return out


def trial_id(kind: str, series_sha256: str) -> str:
    """A ledger line's identity: the kind and the series bytes (a byte-identical re-run under another cell name is the
    same trial). The v8 fields never enter it."""
    return hashlib.sha256(json.dumps([kind, series_sha256], separators=(",", ":")).encode()).hexdigest()[:16]


def defect_line(target: str, reason: str, date: str | None = None) -> dict:
    """A defect event line (review C-3): the ledgered cell whose trial_id is ``target`` is invalid (v8-prereg item 7),
    found after it was ledgered. kind defect, count 0, no cell and no series; its trial_id is (defect, target), so a
    cell has at most one. ledger_append refuses a target that is not a ledgered cell line or is invalid already."""
    if not (isinstance(target, str) and target):
        raise ValueError("ledger: a defect line names the trial_id of a ledgered cell")
    if not (isinstance(reason, str) and reason.strip()):
        raise ValueError("ledger: a defect needs a reason")
    rec = {"schema": LEDGER_SCHEMA, "kind": DEFECT, "count": 0, "defect_of": target, "reason": reason}
    if date is not None:
        rec["date"] = date
    rec["trial_id"] = trial_id(DEFECT, target)
    return rec


def is_event(rec: dict) -> bool:
    """An event line (protocol, defect, validation): no cell, adds no trial, skipped by every cell listing."""
    return rec.get("kind") in EVENT_KINDS


def positive_int(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 1


def campaign_line(campaign: str, registry_path: str, registry_head: str, registry_count: int, *,
                  registry_bytes: int, research_window_id: str | None = None, date: str | None = None,
                  note: str | None = None) -> dict:
    """A mining campaign's ledger line (Ruling E-33; plan: the campaign registry's chain head copied to the cycle
    ledger). kind mining-campaign, origin mined, count 0: it adds no trial to any N of the ledger, the construction N
    included (a mined member that enters a construction cell is counted by that cell's line). It carries its own
    registry count, ``registry.count``: the candidates the campaign tried, its own budget (pre-registration rule 10,
    the mined-v1 Bonferroni count). Review MINE-1: ``registry.chain_head`` is the SHA-256 of the registry log's first
    ``registry.bytes`` bytes (the append-only log as the campaign left it; atx-equity-strategy-mine writes it, and
    its C++ twin strategy_mine_ledger.cpp writes this line byte for byte). Its trial_id is (mining-campaign, chain
    head)."""
    if not (isinstance(campaign, str) and campaign.strip()):
        raise ValueError("ledger: a mining campaign needs a name")
    if not (isinstance(registry_head, str) and len(registry_head) == 64 and set(registry_head) <= set("0123456789abcdef")):
        raise ValueError("ledger: a mining campaign names its registry's chain head (a SHA-256 hex digest)")
    if not positive_int(registry_count):
        raise ValueError("ledger: a mining campaign's registry count is a positive integer")
    if not positive_int(registry_bytes):
        raise ValueError("ledger: a mining campaign's registry byte count is a positive integer")
    rec = {"schema": LEDGER_SCHEMA, "kind": MINING_CAMPAIGN, "count": 0, "campaign": campaign, "origin": "mined",
           "registry": {"path": str(registry_path).replace("\\", "/"), "chain_head": registry_head,
                        "bytes": registry_bytes, "count": registry_count}}
    rec.update(ledger_record_fields(note=note, research_window_id=research_window_id))
    if date is not None:
        rec["date"] = date
    rec["trial_id"] = trial_id(MINING_CAMPAIGN, registry_head)
    return rec


def is_campaign(rec: dict) -> bool:
    """A mining campaign line (Ruling E-33): no cell, adds no trial; its registry count is its own budget."""
    return rec.get("kind") == MINING_CAMPAIGN


def campaign_registry_count(records: list[dict]) -> int:
    """The registry counts of the ledger's mining campaign lines, summed (never part of N)."""
    return sum(int((r.get("registry") or {}).get("count", 0)) for r in records if is_campaign(r))


def check_line(before: dict, rec: dict) -> bool:
    """Whether ``rec`` is new against the lines before it (``before``: trial_id -> line); raises ValueError when it
    cannot be appended as asked. Review C-3: a line whose trial_id is ledgered already is skipped only when it asks for
    nothing the ledgered line lacks; a defect or re-run flag that would be dropped is refused, and so is a second
    defect line of a cell. A defect line must name a ledgered cell line that is not invalid yet."""
    tid = rec["trial_id"]
    old = before.get(tid)
    if old is not None:
        flags = {k: rec.get(k) for k in FLAG_KEYS}
        if any(v is not None for v in flags.values()) and flags != {k: old.get(k) for k in FLAG_KEYS}:
            raise ValueError(f"ledger: {rec.get('cell')} is already ledgered as trial {tid} without these flags "
                             f"({', '.join(f'{k}={v!r}' for k, v in flags.items() if v is not None)}): they would be "
                             "dropped. A defect found later is a defect line (research_cycle.py ledger-defect "
                             f"--trial-id {tid} --reason ...); a re-run is a new cell line naming --rerun-of")
        if rec.get("kind") == DEFECT and rec.get("reason") != old.get("reason"):
            raise ValueError(f"ledger: trial {rec.get('defect_of')} has a defect line already")
        return False
    if rec.get("kind") == DEFECT:
        target = before.get(rec.get("defect_of"))
        if target is None or is_event(target) or is_campaign(target):
            raise ValueError(f"ledger: defect_of {rec.get('defect_of')!r} is not the trial_id of a ledgered cell line")
        if target.get("defect"):
            raise ValueError(f"ledger: trial {rec['defect_of']} was ledgered invalid already")
    if rec.get("rerun_of") is not None:
        check_rerun(before, rec)
    return True


def check_rerun(before: dict, rec: dict) -> None:
    """Review C-4: ``rerun_of`` names the trial_id of an earlier cell line of the same kind (an era shard line or an
    event line is no cell; a typo, a cell name or another kind's id is refused), and a window re-run re-scores that
    cell on another research window: its target carries another window_id, or none (a legacy line).

    Review C-5: a blind or returns re-run (the defect rule, v8-prereg item 7) names an invalid cell, invalid on an
    earlier line (its own defect flag or a defect line): a valid cell is never replaced (item 5: no retry)."""
    target = before.get(rec["rerun_of"])
    if target is None or is_event(target) or is_era_line(target) or target.get("kind") != rec.get("kind"):
        raise ValueError(f"ledger: rerun_of {rec['rerun_of']!r} is not the trial_id of an earlier {rec.get('kind')} "
                         "cell line in this ledger")
    if rec.get("rerun_basis") == "window" and "window_id" in target and target["window_id"] == rec.get("window_id"):
        raise ValueError(f"ledger: a window re-run re-scores a ledgered cell on another research window; trial "
                         f"{rec['rerun_of']} was scored on {target['window_id']} already")
    if rec.get("rerun_basis") in ("blind", "returns") and not target.get("defect") and \
            trial_id(DEFECT, rec["rerun_of"]) not in before:
        raise ValueError(f"ledger: a {rec['rerun_basis']} re-run replaces an invalid cell (v8-prereg item 7); trial "
                         f"{rec['rerun_of']} has no defect on an earlier line (research_cycle.py ledger-defect "
                         f"--trial-id {rec['rerun_of']} --reason ... first)")


CHAIN_GENESIS = "0" * 64


def line_sha256(line: str) -> str:
    """SHA-256 of one ledger line's UTF-8 bytes without its line end (a CRLF checkout hashes as the LF original)."""
    return hashlib.sha256(line.rstrip("\r\n").encode("utf-8")).hexdigest()


def _ledger_lines(p: Path) -> list[tuple[int, str]]:
    return [(n, line) for n, line in enumerate(p.read_text(encoding="utf-8").splitlines(), 1) if line.strip()]


def fold_head(head: str, line: str) -> str:
    """The chain head after an unchained (legacy) line: SHA-256 of the previous head and the line's own SHA-256, so
    the head covers every line from the first, each hashed in its stored form (review C-6)."""
    return hashlib.sha256((head + line_sha256(line)).encode()).hexdigest()


def chain_head(lines: list[str]) -> str:
    """The chain head after the given non-blank line texts, in order (the head of a ledger holding exactly them)."""
    head = CHAIN_GENESIS
    for line in lines:
        head = line_sha256(line) if "prev_sha256" in json.loads(line) else fold_head(head, line)
    return head


def _walk(p: Path) -> tuple[list[dict], str]:
    """(every line, schema-checked and chain-verified; the chain head after the last line). See ``ledger_read``."""
    out, head, chained = [], CHAIN_GENESIS, 0
    for n, line in _ledger_lines(p):
        rec = json.loads(line)
        if rec.get("schema") != LEDGER_SCHEMA:
            raise ValueError(f"ledger {p}:{n}: schema {rec.get('schema')!r} is not {LEDGER_SCHEMA}")
        link = rec.get("prev_sha256")
        if link is None:
            if chained:
                raise ValueError(f"ledger {p}:{n}: an unchained line after the chained line {chained}: every line "
                                 "after the first chained one carries prev_sha256")
            head = fold_head(head, line)
        elif link != head:
            raise ValueError(f"ledger {p}:{n}: hash chain broken: prev_sha256 {str(link)[:16]} is not the chain head "
                             f"of the lines before it ({head[:16]})")
        else:
            chained = chained or n
            head = line_sha256(line)
        out.append(rec)
    return out, head


def ledger_read(path: Path) -> list[dict]:
    """Every ledger line, schema-checked, with the hash chain verified.

    Chain (v8; review C-6): the chain head starts at 64 zeros; an unchained (legacy, pre-v8) line folds into it
    (``fold_head``: its bytes as stored), a chained line must name the head of every line before it in
    ``prev_sha256`` and becomes the head (``line_sha256`` of its bytes). So the first chained line pins every legacy
    line before it, each chained line pins the whole prefix, and an edited, inserted or removed line anywhere before
    the last chained line breaks the chain. A line without ``prev_sha256`` after a chained one is refused. A ledger
    chained from its first line reads exactly as under ledger-chain-v1 (prev = the previous line's SHA-256)."""
    p = Path(path)
    return _walk(p)[0] if p.exists() else []


def ledger_head(path: Path) -> str:
    """The chain head of the ledger (64 zeros for an empty or absent one), after verifying it: the SHA-256 of the last
    line when it is chained, else the fold of every line (review C-6: a verdict and a ledger copy record it, so an
    edit of the tail is detected against the recorded head)."""
    p = Path(path)
    return _walk(p)[1] if p.exists() else CHAIN_GENESIS


def ledger_append(path: Path, records: list[dict], *, chain: bool = False) -> tuple[list[dict], list[dict]]:
    """Append records not already present (same trial_id); returns (appended, skipped). Never rewrites a line.

    ``chain`` (and any ledger whose last line is already chained) writes ``prev_sha256`` into every appended line;
    without it the lines are written exactly as before v8. Every record is checked against the lines before it
    (``check_line``) before anything is written: one refusal (ValueError) appends nothing."""
    p = Path(path)
    existing, prev = _walk(p) if p.exists() else ([], CHAIN_GENESIS)   # verifies the chain before anything is appended
    before = {r.get("trial_id"): r for r in existing}
    chained = chain or bool(existing and "prev_sha256" in existing[-1])
    appended: list[dict] = []
    skipped: list[dict] = []
    for rec in records:
        (appended if check_line(before, rec) else skipped).append(rec)
        before.setdefault(rec["trial_id"], rec)
    if appended:
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        with Path(path).open("a", encoding="utf-8", newline="\n") as f:
            for k, rec in enumerate(appended):
                if chained:
                    rec = appended[k] = dict(rec, prev_sha256=prev)
                text = json.dumps(rec, sort_keys=True, separators=(",", ":"))
                prev = line_sha256(text) if chained else fold_head(prev, text)
                f.write(text + "\n")
    return appended, skipped


def invalid_ids(records: list[dict]) -> set:
    """The trial_ids of the invalid cells: ledgered with ``defect``, or named by a defect line (review C-3)."""
    own = {r.get("trial_id") for r in records if (r.get("defect") or {}).get("invalid") and not is_event(r)}
    return own | {r.get("defect_of") for r in records if r.get("kind") == DEFECT}


def trial_counts(records: list[dict]) -> list[int]:
    """The trials each ledger line adds to N (v8-prereg item 2 and item 7, the defect rule), in ledger order.

    A line adds its ``count`` (default 1) except: an event line (``protocol``, ``defect``), a window re-run
    (``rerun_basis`` window: a ledgered cell re-run on a longer window) and a blind re-run (``rerun_basis`` blind:
    decided without seeing returns) add 0; an invalid cell (``defect``, or named by a defect line) adds 0 unless a
    re-run names it. A re-run never lowers N (review C-5): the replaced cell stays counted and its blind re-run adds 0
    (one trial for the pair), while a re-run decided because the returns looked wrong (``rerun_basis`` returns) is a
    new trial beside it. Lines without the v8 fields add their count, exactly as ``ledger_counts`` summed them before
    v8. An era shard line (``era_of``, task H-1) adds 0: its pooled line is the trial. A mining campaign line (Ruling
    E-33) adds 0: its registry count is the campaign's own budget, never part of a ledger N."""
    rerun = {r.get("rerun_of") for r in records if r.get("rerun_basis") in ("blind", "returns")}
    invalid = invalid_ids(records)
    out = []
    for rec in records:
        tid = rec.get("trial_id")
        if is_event(rec) or rec.get("rerun_basis") in ("window", "blind") or is_era_line(rec) or is_campaign(rec):
            out.append(0)
        elif tid in invalid and tid not in rerun:
            out.append(0)
        else:
            out.append(int(rec.get("count", 1)))
    return out


def ledger_n(records: list[dict], scored_in_ledger: bool, kind: str = "construction") -> int:
    """The N that gates (PM ruling 2026-09-29; v8-prereg Appendix A rules 2 and 7): the ``kind`` trials of the ledger
    by ``trial_counts`` (the defect rule: event lines, window and blind re-runs, unreplaced invalid cells add 0), plus
    1 for the scored cell when it has no ``kind`` line in the ledger yet. nav_summ --dsr-ledger and research_cycle.py
    (summ.dsr_n "ledger+1") both read N here, so the two print the same N. A line without a kind (a layout
    ledger_record never writes) is read as a ``kind`` line."""
    n = sum(c for rec, c in zip(records, trial_counts(records)) if rec.get("kind", kind) == kind)
    return n + (0 if scored_in_ledger else 1)


def excluded_lines(records: list[dict]) -> list[dict]:
    """The cell lines whose result the defect rule leaves out of V[SR]: invalid cells and cells replaced by a blind
    re-run, unless a returns re-run names them. Of these, only the invalid cells no re-run names are out of N too
    (``trial_counts``: a replaced cell stays counted for its blind re-run, review C-5)."""
    seen = {r.get("rerun_of") for r in records if r.get("rerun_basis") == "returns"}
    replaced = {r.get("rerun_of") for r in records if r.get("rerun_basis") == "blind"}
    invalid = invalid_ids(records)
    return [r for r in records if not is_event(r) and r.get("trial_id") not in seen and
            (r.get("trial_id") in invalid or r.get("trial_id") in replaced)]


def ledger_counts(records: list[dict]) -> dict:
    """Trials by kind and window: {kind: {window key: trials}} (``trial_counts`` per line; default 1 each). Protocol
    and defect lines are events, not trials: they are skipped, and so are mining campaign lines (Ruling E-33)."""
    out: dict = {}
    for rec, trials in zip(records, trial_counts(records)):
        if is_event(rec) or is_era_line(rec) or is_campaign(rec):
            continue
        w = rec.get("window") or {}
        key = f"{w.get('label', '?')} {w.get('first_session', '?')}..{w.get('last_session', '?')}"
        out.setdefault(rec["kind"], {}).setdefault(key, 0)
        out[rec["kind"]][key] += trials
    return out


def appendix_a(records: list[dict], path: str) -> list[str]:
    counts = ledger_counts(records)
    total = sum(sum(v.values()) for v in counts.values())
    lines = [f"Appendix A (trial ledger {path}): {total} trials in {len(records)} ledger lines"]
    for kind in LEDGER_KINDS:
        if kind == MINING_CAMPAIGN:
            continue                        # printed below, only when a campaign is ledgered (Ruling E-33)
        for window, n in sorted((counts.get(kind) or {}).items()):
            lines.append(f"   {kind:12s} {n:5d}  [{window}]")
        if kind not in counts:
            lines.append(f"   {kind:12s} {0:5d}")
    campaigns = [r for r in records if is_campaign(r)]
    if campaigns:
        lines.append(f"   {MINING_CAMPAIGN} {0:5d}  ({len(campaigns)} campaign line(s), registry count "
                     f"{campaign_registry_count(records)}: the campaigns' own budget, not in N)")
    zero = [r for r, c in zip(records, trial_counts(records)) if c == 0 and not is_era_line(r) and not is_campaign(r)]
    if zero:  # only ledgers with v8 fields print this line: a v7 ledger's block is unchanged
        reads = sum(1 for r in zero if r.get("kind") == VALIDATION)
        out = [r for r in zero if r.get("rerun_basis") != "window" and r.get("kind") not in ZERO_TRIAL_KINDS
               and r.get("kind") != VALIDATION]
        lines.append(f"   adding no trial: {len(zero)} line(s) ({len(out)} by the defect rule, "
                     f"{sum(1 for r in zero if r.get('rerun_basis') == 'window')} window re-run(s), "
                     f"{sum(1 for r in zero if r.get('kind') in ZERO_TRIAL_KINDS)} protocol line(s)"
                     + (f", {reads} hidden-block validation read(s)" if reads else "") + ")")
    eras = [r for r in records if is_era_line(r)]
    if eras:  # task H-1: printed only when an era shard is ledgered
        lines.append(f"   era shard line(s): {len(eras)}, adding no trial (each pooled line counts its eras once: "
                     f"{sum(1 for r in records if is_pool_line(r))} pooled line(s))")
    return lines


def appendix_a_v8(records: list[dict]) -> str:
    """The v8 Appendix A block (v8-prereg, 'on every result'): construction trials by the defect rule, this window's
    admission trials, and the window statement, every window date taken from the research window."""
    rw, wid = research_window(), window_id()
    counts = trial_counts(records)
    n = sum(c for r, c in zip(records, counts) if r.get("kind") == "construction")
    k = sum(c for r, c in zip(records, counts) if r.get("kind") == "admission" and r.get("window_id") == wid)
    first = session_date(rw.TRAIN_BEGIN_NS).year
    last = session_date(rw.TRAIN_END_NS - DAY_NS).year
    sealed = session_date(rw.SEAL_NS).year
    never = (rw.load().get("hidden") or {}).get("never_read") or [None]
    never_year = dt.date.fromisoformat(never[0]).year if never[0] else None
    tail = f"; {never_year}+ never read" if never_year else ""
    return (f"TRAIN construction cells {n}; admission trials this sprint {k}; window {wid} ({first}-{last}); "
            f"hidden {sealed}+ unread in this sprint; validation reads before v8: {PRIOR_VALIDATION_READS}{tail}.")


def dsr_variance(records: list[dict], research_window_id: str, kind: str = "construction") -> dict:
    """OD-4 / v8-prereg item 3: N and the cross-trial variance of the deflated Sharpe ratio from the ledger.

    N = the ``kind`` trials by ``trial_counts``. V[SR] = sample variance (ddof 1) of the per-session S2 net SRs
    (``s2_net_sr`` / sqrt(252)) of the ``kind`` lines scored on ``research_window_id`` (the ledgered cells re-run on it
    and the new cells), the defect rule's excluded lines left out; None below two such lines. The legacy variance over
    the ``kind`` lines without a window_id (the cells ledgered before the window change) is reported beside it. Era
    shard and pooled lines (task H-1) are scored on history eras, not on a research window's TRAIN: left out of both."""
    counts = trial_counts(records)
    out_ids = {r.get("trial_id") for r in excluded_lines(records)}

    def srs(select) -> list[float]:
        return [float(r["s2_net_sr"]) / math.sqrt(ANNUAL) for r in records
                if r.get("kind") == kind and select(r) and r.get("trial_id") not in out_ids
                and not is_era_line(r) and not is_pool_line(r)
                and isinstance(r.get("s2_net_sr"), (int, float)) and math.isfinite(r["s2_net_sr"])]
    cur = srs(lambda r: r.get("window_id") == research_window_id)
    old = srs(lambda r: "window_id" not in r)
    return {"n": sum(c for r, c in zip(records, counts) if r.get("kind") == kind), "window_id": research_window_id,
            "variance_sr": float(np.var(cur, ddof=1)) if len(cur) >= 2 else None, "cells": len(cur),
            "variance_source": f"sample variance (ddof 1) of {len(cur)} per-session S2 net SRs ledgered on "
                               f"{research_window_id} (re-runs and new cells; defect-rule exclusions left out)",
            "legacy_variance_sr": float(np.var(old, ddof=1)) if len(old) >= 2 else None, "legacy_cells": len(old)}


def ledger_net_series(records: list[dict], load) -> tuple[list[str], list[dict]]:
    """(cell names, net series) of the ledger lines with a series, in ledger order; ``load(record)`` -> nets map.
    Protocol lines (no cell, no series), era shard lines and pooled lines (no single path; task H-1) are skipped.

    The series file must still hash to the ledgered SHA-256 (a changed file is refused, never silently used)."""
    names, series = [], []
    for rec in records:
        s = rec.get("series")
        if is_event(rec) or is_era_line(rec) or not s or not s.get("path"):
            continue
        if sha256_file(Path(s["path"])) != s["sha256"]:
            raise ValueError(f"ledger: series {s['path']} no longer matches its ledgered SHA-256")
        names.append(rec["cell"])
        series.append(load(rec))
    return names, series
