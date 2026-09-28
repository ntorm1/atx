"""Stat-arb cluster study (TRAIN 2020-2022 only; pre-registered in ../statarb-prereg.md).

Rolling unsupervised clusters (PCA embedding + balanced k-means, groups of K) -> stock-vs-group and
group-vs-neighbour-group divergence features -> daily rank-IC / Fama-MacBeth significance -> FIT 2020-21 / HOLD 2022 model.

Stages (root runs each under scripts/run_bounded_research.py, <=180 s, <=1536 MiB):
  clusters <c30|c10|c100|rnd30|ff49>  -> OUT/groups_<g>.npz
  features <grouping|controls>        -> OUT/feat/<name>.npy  (f32 [score_days, ever-member columns])
  analyze                             -> OUT/analyze.json
  model_fit <ols|hgb:CTRL+FF49|hgb:FULL> -> OUT/pred/<model>.npy (FIT 2020-21 minus 7-session embargo)
  model_eval                          -> OUT/model.json (FIT in-sample and HOLD 2022 IC, neutral books, costs)
Seal: refuses any session >= 2023-01-01. Clock: features at decision d use closes <= d; clusters formed at t_p use
closes <= t_p and serve d in [t_p, t_p+21). Targets y1 = r[d+1] (reference only), y2 = r[d+2] (NAV convention),
y5 = sum r[d+2..d+6].
"""
import hashlib
import json
import sys
import time
from pathlib import Path

import numpy as np
from scipy.special import ndtri
from scipy.stats import rankdata

ROOT = Path("C:/atx-wt/pool-2")
ROLE = ROOT / "build-equity/recent-fast-train-2020-2022-v2"
FIELDS = ROOT / "build-equity/recent-fast-train-2020-2022-v2-fields-v6"
OUT = ROOT / "build-equity/statarb-cluster-v1"
ROLE_SHA = "210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de"
SEAL_NS = 1672531200000000000  # 2023-01-01T00:00Z
W_CLUST, STEP, N_PC, MIN_OBS, L_REG, LOOK, CLIP = 252, 21, 15, 200, 60, 260, 0.5
GROUPINGS = {"c30": ("kmeans", 30), "c10": ("kmeans", 10), "c100": ("kmeans", 100), "rnd30": ("random", 30),
             "ff49": ("ff49", 0)}
FULL = ["dev_1", "dev_5", "dev_21", "dev_5z", "sscore", "kappa", "beta_g", "cohesion", "grp_dev_5", "grp_dev_21",
        "grp_sscore", "grp_mkt_5", "grp_mkt_21", "grp_mom_12_1", "nbr_mkt_1", "nbr_mkt_5", "grp_disp_5", "grp_tight"]
CORE = ["dev_5", "dev_21", "sscore", "grp_dev_5", "grp_mkt_5"]
CONTROLS = ["rev_1", "rev_5", "rev_21", "mom_12_1", "vol63", "beta252", "ladv63", "size"]
TARGETS = {"y1": (1, 1), "y2": (2, 1), "y5": (2, 5)}
NW_LAGS = {"y1": 5, "y2": 5, "y5": 10}
BASIS = ["beta252", "vol63", "ladv63"]  # price-risk-v1 neutraliser columns (plus intercept)


def need(cond, msg):
    if not cond:
        raise SystemExit(f"statarb: {msg}")


# ---------------------------------------------------------------- numerics (synthetic-tested)
def onehot(lab, n_groups):
    m = np.zeros((lab.size, n_groups))
    m[np.arange(lab.size), lab] = 1.0
    return m


def rsum(a, w, minfin):
    """Trailing w-row sum along axis 0 aligned to the window's last row; NaN unless >= minfin finite rows."""
    ok = np.isfinite(a)
    cs = np.concatenate([np.zeros((1,) + a.shape[1:]), np.cumsum(np.where(ok, a, 0.0), 0)])
    cn = np.concatenate([np.zeros((1,) + a.shape[1:]), np.cumsum(ok, 0)])
    out = np.full(a.shape, np.nan)
    if a.shape[0] >= w:
        out[w - 1:] = np.where(cn[w:] - cn[:-w] >= minfin, cs[w:] - cs[:-w], np.nan)
    return out


def capacity_assign(dist, cap):
    """Greedy balanced assignment: points with the largest regret (2nd-best minus best distance) choose first."""
    n, k = dist.shape
    part = np.partition(dist, 1, axis=1)
    order = np.argsort(part[:, 0] - part[:, 1], kind="stable")
    dm = dist.copy()
    fill = np.zeros(k, np.int64)
    lab = np.empty(n, np.int64)
    for i in order:
        j = int(np.argmin(dm[i]))
        lab[i] = j
        fill[j] += 1
        if fill[j] >= cap:
            dm[:, j] = np.inf
    return lab


def balanced_kmeans(emb, k_size, rng, iters=20):
    """Groups of ~k_size: N = round(n/k_size) centres, capacity ceil(n/N); returns contiguous labels 0..N'-1."""
    n = emb.shape[0]
    n_groups = max(2, int(round(n / k_size)))
    cap = -(-n // n_groups)
    cen = np.empty((n_groups, emb.shape[1]))
    cen[0] = emb[rng.integers(n)]
    d2 = ((emb - cen[0]) ** 2).sum(1)
    for j in range(1, n_groups):
        p = d2 / d2.sum() if d2.sum() > 0 else None
        cen[j] = emb[rng.choice(n, p=p)]
        d2 = np.minimum(d2, ((emb - cen[j]) ** 2).sum(1))
    lab = None
    for _ in range(iters):
        dist = (emb * emb).sum(1)[:, None] + (cen * cen).sum(1)[None] - 2.0 * emb @ cen.T
        new = capacity_assign(dist, cap)
        if lab is not None and np.array_equal(new, lab):
            break
        lab = new
        m = onehot(lab, n_groups)
        sz = m.sum(0)
        cen = np.where(sz[:, None] > 0, (m.T @ emb) / np.maximum(sz, 1)[:, None], cen)
    return np.unique(lab, return_inverse=True)[1]


def ou_fit(eps, ok):
    """AR(1) on X = cumsum(eps) per column (Avellaneda-Lee 2010): returns m, sigma_eq, kappa (NaN if not reverting)."""
    x = np.cumsum(eps, 0)
    x0, x1 = x[:-1], x[1:]
    m0, m1 = x0.mean(0), x1.mean(0)
    with np.errstate(invalid="ignore", divide="ignore"):
        b = ((x0 - m0) * (x1 - m1)).sum(0) / ((x0 - m0) ** 2).sum(0)
        a = m1 - b * m0
        v = (x1 - a - b * x0).var(0)
        good = (b > 0) & (b < 0.9999) & (ok.sum(0) >= 50)
        m = np.where(good, a / (1 - b), np.nan)
        seq = np.where(good, np.sqrt(v / (1 - b * b)), np.nan)
        kap = np.where(good, -np.log(np.where(good, b, 0.5)) * 252, np.nan)
    return m, seq, kap, x[-1]


def ou_stock(y, x):
    """60d regression of y (stock) on x (LOO group mean); OU s-score of the residual; also beta, cohesion, sd(y-x)."""
    ok = np.isfinite(y) & np.isfinite(x)
    n = ok.sum(0)
    y0, x0 = np.where(ok, y, 0.0), np.where(ok, x, 0.0)
    with np.errstate(invalid="ignore", divide="ignore"):
        mx, my = x0.sum(0) / n, y0.sum(0) / n
        xc, yc = np.where(ok, x0 - mx, 0.0), np.where(ok, y0 - my, 0.0)
        sxx, sxy, syy = (xc * xc).sum(0), (xc * yc).sum(0), (yc * yc).sum(0)
        beta = sxy / sxx
        eps = yc - beta * xc
        m, seq, kap, xl = ou_fit(eps, ok)
        s = (xl - (m - np.nanmean(m))) / seq
        coh = sxy / np.sqrt(sxx * syy)
        dd = np.where(ok, y0 - x0, np.nan)
        sd = np.sqrt(np.nanmean((dd - np.nanmean(dd, 0)) ** 2, 0))
    bad = n < 50
    return [np.where(bad, np.nan, v) for v in (s, kap, beta, coh, sd)]


def ou_spread(sp):
    """OU s-score of a spread series per column (demeaned over the window, as the group-vs-neighbour signal)."""
    ok = np.isfinite(sp)
    with np.errstate(invalid="ignore"):
        eps = np.where(ok, sp - np.nanmean(sp, 0), 0.0)
        m, seq, _, xl = ou_fit(eps, ok)
        return (xl - (m - np.nanmean(m))) / seq


def gauss(x):
    """Normal scores of the finite entries (average ranks); NaN stays NaN."""
    out = np.full(x.shape, np.nan)
    ok = np.isfinite(x)
    k = int(ok.sum())
    if k:
        out[ok] = ndtri((rankdata(x[ok]) - 0.5) / k)
    return out


def nw_t(x, lags):
    x = np.asarray(x, float)
    x = x[np.isfinite(x)]
    t = x.size
    if t < 20:
        return None
    u = x - x.mean()
    s = u @ u / t + 2 * sum((1 - lag / (lags + 1)) * (u[lag:] @ u[:-lag]) / t for lag in range(1, lags + 1))
    return float(x.mean() / np.sqrt(s / t)) if s > 0 else None


def sr(x):
    x = np.asarray(x, float)
    x = x[np.isfinite(x)]
    return float(x.mean() / x.std(ddof=1) * np.sqrt(252)) if x.size > 20 and x.std() > 0 else None


# ---------------------------------------------------------------- data
class Role:
    def __init__(self, prices=True):
        mb = (ROLE / "manifest.json").read_bytes()
        need(hashlib.sha256(mb).hexdigest() == ROLE_SHA, "TRAIN role manifest pin mismatch")
        m = json.loads(mb)
        self.d, self.n, self.begin, self.end = m["dates"], m["instruments"], m["score_begin"], m["score_end"]
        self.sessions = np.fromfile(ROLE / "sessions.i64", dtype="<i8")
        need(self.sessions.size == self.d and int(self.sessions.max()) < SEAL_NS, "seal: session >= 2023-01-01")
        self.member = self._rd("member.u8", "u1").astype(bool)
        self.cols = np.flatnonzero(self.member[self.begin:self.end].any(0))
        self.years = (1970 + self.sessions.astype("datetime64[ns]").astype("datetime64[Y]").astype(int))
        if prices:
            present = self._rd("present.u8", "u1").astype(bool)
            close = self._rd("close.f64", "<f8")
            self.r = np.full((self.d, self.n), np.nan)
            with np.errstate(invalid="ignore", divide="ignore"):
                self.r[1:] = np.where(present[1:] & present[:-1], close[1:] / close[:-1] - 1.0, np.nan)

    def _rd(self, name, dt):
        return np.fromfile(ROLE / name, dtype=dt).reshape(self.d, self.n)

    def field(self, name):
        man = json.loads((FIELDS / "manifest.json").read_bytes())
        spec = next(f for f in man["fields"] if f["name"] == name)
        need(spec["layout"] == "date-major" and spec["shape"] == [self.d, self.n], f"field layout {name}")
        return np.fromfile(FIELDS / spec["file"], dtype="<f8").reshape(self.d, self.n)


def save(name, arr):
    (OUT / "feat").mkdir(parents=True, exist_ok=True)
    np.save(OUT / "feat" / f"{name}.npy", np.asarray(arr, np.float32))


# ---------------------------------------------------------------- stage: clusters
def stage_clusters(g):
    kind, k_size = GROUPINGS[g]
    R = Role()
    rc = np.clip(R.r, -CLIP, CLIP)
    ff49 = R.field("grp_ff49") if kind == "ff49" else None
    ff12 = R.field("grp_ff12")
    starts = np.arange(R.begin, R.end, STEP)
    labels = np.full((starts.size, R.n), -1, np.int16)
    nbrs, tights, diag, prev = [], [], [], None
    rng = np.random.default_rng(20260927)
    for p, t in enumerate(starts):
        win = rc[t - W_CLUST + 1:t + 1]
        cand = R.member[t] & (np.isfinite(win).sum(0) >= MIN_OBS)
        if ff49 is not None:
            cand &= np.isfinite(ff49[t])
        cols = np.flatnonzero(cand)
        x = win[:, cols]
        with np.errstate(invalid="ignore"):
            x = x - np.nanmean(x, axis=1, keepdims=True)
            z = np.nan_to_num(np.clip((x - np.nanmean(x, 0)) / np.nanstd(x, 0), -6, 6))
        ev = None
        if kind == "kmeans":
            _, s, vt = np.linalg.svd(z, full_matrices=False)
            ev = float((s[:N_PC] ** 2).sum() / (s ** 2).sum())
            emb = vt[:N_PC].T * s[:N_PC]
            emb /= np.linalg.norm(emb, axis=1, keepdims=True)
            lab = balanced_kmeans(emb, k_size, rng)
        elif kind == "random":
            lab = rng.permutation(cols.size) % max(2, int(round(cols.size / k_size)))
        else:
            lab = np.unique(ff49[t, cols].astype(np.int64), return_inverse=True)[1]
        n_groups = int(lab.max()) + 1
        m = onehot(lab, n_groups)
        ok = np.isfinite(x)
        with np.errstate(invalid="ignore", divide="ignore"):
            gm = (np.where(ok, x, 0.0) @ m) / (ok @ m)
            c = np.corrcoef(np.nan_to_num(gm).T)
        c = np.where(np.isfinite(c), c, -np.inf)
        np.fill_diagonal(c, -np.inf)
        nbr = c.argmax(1)
        sz = m.sum(0)
        v = ((z @ m) / sz).var(0)
        tight = np.where(sz > 1, (sz * v - 1) / np.maximum(sz - 1, 1), np.nan)
        labels[p, cols] = lab
        nbrs.append(nbr)
        tights.append(tight)
        f12 = ff12[t, cols]
        pur = []
        for j in range(n_groups):
            v12 = f12[(lab == j) & np.isfinite(f12)]
            if v12.size:
                pur.append(np.unique(v12, return_counts=True)[1].max() / v12.size)
        ari = None
        if prev is not None:
            from sklearn.metrics import adjusted_rand_score
            both = (prev >= 0) & (labels[p] >= 0)
            ari = float(adjusted_rand_score(prev[both], labels[p][both]))
        prev = labels[p].copy()
        diag.append({"t": int(t), "n": int(cols.size), "groups": n_groups, "size_min": int(sz.min()),
                     "size_max": int(sz.max()), "tight_mean": float(np.nanmean(tight)), "ff12_purity": float(np.mean(pur)),
                     "pc_var_explained": ev, "ari_vs_prev": ari})
    width = max(x.size for x in nbrs)
    pad = lambda rows, fill, dt: np.array([np.concatenate([r, np.full(width - r.size, fill)]) for r in rows], dt)
    OUT.mkdir(parents=True, exist_ok=True)
    np.savez(OUT / f"groups_{g}.npz", starts=starts, labels=labels, nbr=pad(nbrs, -1, np.int32),
             tight=pad(tights, np.nan, np.float64))
    summ = {k: float(np.mean([d[k] for d in diag if d[k] is not None])) for k in
            ("n", "groups", "size_min", "size_max", "tight_mean", "ff12_purity")}
    aris = [d["ari_vs_prev"] for d in diag if d["ari_vs_prev"] is not None]
    summ["ari_vs_prev_mean"] = float(np.mean(aris)) if aris else None
    evs = [d["pc_var_explained"] for d in diag if d["pc_var_explained"] is not None]
    summ["pc15_var_explained_mean"] = float(np.mean(evs)) if evs else None
    (OUT / f"groups_{g}.json").write_text(json.dumps({"grouping": g, "summary": summ, "periods": diag}, indent=1))
    print(g, json.dumps(summ))


# ---------------------------------------------------------------- stage: features
def stage_features(g):
    R = Role()
    if g == "controls":
        return stage_controls(R)
    z = np.load(OUT / f"groups_{g}.npz")
    starts, labels, nbrs, tights = z["starts"], z["labels"], z["nbr"], z["tight"]
    names = FULL if g == "c30" else CORE
    t_count = R.end - R.begin
    pos_of = np.full(R.n, -1)
    pos_of[R.cols] = np.arange(R.cols.size)
    F = {k: np.full((t_count, R.cols.size), np.nan, np.float32) for k in names}
    rc = np.clip(R.r, -CLIP, CLIP)
    for p, t in enumerate(starts):
        t1 = int(starts[p + 1]) if p + 1 < starts.size else R.end
        cols = np.flatnonzero(labels[p] >= 0)
        lab = labels[p][cols].astype(np.int64)
        n_groups = int(lab.max()) + 1
        nb, tg = nbrs[p][:n_groups], tights[p][:n_groups]
        pos = pos_of[cols]
        need((pos >= 0).all(), "cluster member outside ever-member columns")
        a = t - LOOK
        x = rc[a:t1, cols]
        ok = np.isfinite(x)
        x0 = np.where(ok, x, 0.0)
        m = onehot(lab, n_groups)
        s_g, c_g = x0 @ m, ok.astype(float) @ m
        with np.errstate(invalid="ignore", divide="ignore"):
            gm = s_g / c_g
            mkt = x0.sum(1) / ok.sum(1)
            den = c_g[:, lab] - ok
            gloo = np.where(den >= 2, (s_g[:, lab] - x0) / den, np.nan)
        dev = x - gloo
        gx = gm - mkt[:, None]
        spread = gm - gm[:, nb]
        li = np.arange(t, t1) - a
        rows = np.arange(t, t1) - R.begin
        blk = {}
        blk["dev_1"] = dev[li]
        blk["dev_5"] = rsum(dev, 5, 4)[li]
        blk["dev_21"] = rsum(dev, 21, 17)[li]
        blk["grp_dev_5"] = rsum(spread, 5, 4)[li][:, lab]
        blk["grp_dev_21"] = rsum(spread, 21, 17)[li][:, lab]
        blk["grp_mkt_5"] = rsum(gx, 5, 4)[li][:, lab]
        blk["grp_mkt_21"] = rsum(gx, 21, 17)[li][:, lab]
        blk["grp_mom_12_1"] = rsum(gx, 231, 200)[li - 21][:, lab]
        blk["nbr_mkt_1"] = gx[li][:, nb][:, lab]
        blk["nbr_mkt_5"] = rsum(gx, 5, 4)[li][:, nb][:, lab]
        blk["grp_tight"] = np.broadcast_to(tg[lab], (li.size, lab.size))
        r5 = rsum(x, 5, 4)[li]
        o5 = np.isfinite(r5)
        r50 = np.where(o5, r5, 0.0)
        with np.errstate(invalid="ignore", divide="ignore"):
            cnt = o5.astype(float) @ m
            mu, m2 = (r50 @ m) / cnt, (r50 * r50 @ m) / cnt
            blk["grp_disp_5"] = np.sqrt(np.maximum(m2 - mu * mu, 0))[:, lab]
        per_day = {k: np.full((li.size, lab.size), np.nan) for k in ("sscore", "kappa", "beta_g", "cohesion", "dev_5z",
                                                                     "grp_sscore")}
        for k, l in enumerate(li):
            s, kap, beta, coh, sd = ou_stock(x[l - L_REG + 1:l + 1], gloo[l - L_REG + 1:l + 1])
            per_day["sscore"][k], per_day["kappa"][k], per_day["beta_g"][k], per_day["cohesion"][k] = s, kap, beta, coh
            with np.errstate(invalid="ignore", divide="ignore"):
                per_day["dev_5z"][k] = blk["dev_5"][k] / (sd * np.sqrt(5))
            if "grp_sscore" in names:
                per_day["grp_sscore"][k] = ou_spread(spread[l - L_REG + 1:l + 1])[lab]
        blk.update(per_day)
        for k in names:
            F[k][np.ix_(rows, pos)] = blk[k]
    for k in names:
        save(f"{g}_{k}", F[k])
    print(g, {k: round(float(np.isfinite(F[k]).mean()), 3) for k in names})


def stage_controls(R):
    b, e, cols = R.begin, R.end, R.cols
    rc = np.clip(R.r, -CLIP, CLIP)
    sl = lambda a: a[b:e][:, cols]
    save("rev_1", sl(rc))
    save("rev_5", sl(rsum(rc, 5, 4)))
    save("rev_21", sl(rsum(rc, 21, 17)))
    mom = np.full(rc.shape, np.nan)
    mom[21:] = rsum(rc, 231, 200)[:-21]
    save("mom_12_1", sl(mom))
    del mom
    ok = np.isfinite(rc)
    n63 = rsum(ok.astype(float), 63, 0)
    with np.errstate(invalid="ignore", divide="ignore"):
        s1, s2 = rsum(rc, 63, 50), rsum(rc * rc, 63, 50)
        save("vol63", sl(np.sqrt(np.maximum(s2 / n63 - (s1 / n63) ** 2, 0))))
        del s1, s2, n63
        mk = np.array([np.nanmean(rc[d, R.member[d]]) if R.member[d].any() else np.nan for d in range(R.d)])
        mf = np.where(ok, mk[:, None], np.nan)
        n = rsum(ok.astype(float), 252, 0)
        sx, sm, sxm, smm = rsum(rc, 252, 200), rsum(mf, 252, 200), rsum(rc * mf, 252, 200), rsum(mf * mf, 252, 200)
        save("beta252", sl((sxm - sx * sm / n) / (smm - sm * sm / n)))
        del sx, sm, sxm, smm, mf, n
        dv = R._rd("raw_close.f64", "<f8") * R._rd("volume.f64", "<f8")
        save("ladv63", sl(np.log(rsum(dv, 63, 50) / rsum(np.isfinite(dv).astype(float), 63, 0))))
        del dv
        save("size", sl(np.log(R.field("me_company"))))
    for name, (lag, h) in TARGETS.items():
        y = np.full(R.r.shape, np.nan)
        s = rsum(R.r, h, max(1, h - 1))  # window of h rows ending at d+lag+h-1
        y[:R.d - (lag + h - 1)] = s[lag + h - 1:]
        save(f"target_{name}", sl(y))
    print("controls + targets saved", cols.size)


# ---------------------------------------------------------------- stage: analyze
def feature_names():
    names = [f"c30_{k}" for k in FULL] + [f"{g}_{k}" for g in ("c10", "c100", "rnd30", "ff49") for k in CORE]
    return names + CONTROLS


def load_feats(names):
    return {k: np.load(OUT / "feat" / f"{k}.npy", mmap_mode="r") for k in names}


def day_gauss(feats, names, k, mem):
    g = np.full((len(names), mem.size), np.nan)
    for i, nm in enumerate(names):
        f = np.asarray(feats[nm][k], float)
        ok = mem & np.isfinite(f)
        if ok.sum() >= 300:
            g[i, ok] = gauss(f[ok])
    return g


def stage_analyze():
    t0 = time.time()
    R = Role(prices=False)
    names = feature_names()
    feats = load_feats(names)
    ys = {t: np.load(OUT / "feat" / f"target_{t}.npy", mmap_mode="r") for t in TARGETS}
    mem_all = R.member[R.begin:R.end][:, R.cols]
    years = R.years[R.begin:R.end]
    t_count, nf = mem_all.shape[0], len(names)
    ic = {t: np.full((t_count, nf), np.nan) for t in TARGETS}
    book = np.full((t_count, nf), np.nan)
    tau = np.full((t_count, nf), np.nan)
    wprev = np.zeros((nf, R.cols.size))
    idx = {nm: i for i, nm in enumerate(names)}
    ctrl = [idx[c] for c in CONTROLS]
    c30f, ff49c = [idx[f"c30_{k}"] for k in FULL], [idx[f"ff49_{k}"] for k in CORE]
    core = lambda g: [idx[f"{g}_{k}"] for k in CORE]
    models = {"M0_ctrl": ctrl, "M1_ctrl+ff49": ctrl + ff49c, "M2_ctrl+ff49+c30full": ctrl + ff49c + c30f,
              "M3_ctrl+c30full": ctrl + c30f, "M4_ctrl+rnd30": ctrl + core("rnd30"), "M5_ctrl+c30core": ctrl + core("c30"),
              "M6_ctrl+c10core": ctrl + core("c10"), "M7_ctrl+c100core": ctrl + core("c100")}
    fm = {(mn, t): np.full((t_count, len(cols) + 1), np.nan) for mn, cols in models.items() for t in ("y2", "y5")}
    corr_acc, corr_n = np.zeros((nf, nf)), 0
    for k in range(t_count):
        mem = mem_all[k]
        g = day_gauss(feats, names, k, mem)
        gy = {}
        for t in TARGETS:
            y = np.asarray(ys[t][k], float)
            ok = mem & np.isfinite(y)
            gy[t] = np.full(mem.size, np.nan)
            if ok.sum() >= 300:
                gy[t][ok] = gauss(y[ok])
                both = np.isfinite(g) & ok[None]
                for i in range(nf):
                    if both[i].sum() >= 300:
                        ic[t][k, i] = np.corrcoef(g[i, both[i]], gy[t][both[i]])[0, 1]
        y2 = np.nan_to_num(np.asarray(ys["y2"][k], float))
        w = np.nan_to_num(g)
        w -= np.where(np.isfinite(g), 1.0, 0.0) * (w.sum(1, keepdims=True) / np.maximum(np.isfinite(g).sum(1, keepdims=True), 1))
        l1 = np.abs(w).sum(1, keepdims=True)
        w = np.where(l1 > 0, w / np.where(l1 > 0, l1, 1), 0.0)
        book[k] = np.where(l1[:, 0] > 0, w @ y2, np.nan)
        if k > 0:
            tau[k] = np.abs(w - wprev).sum(1)
        wprev = w
        if k % 5 == 0:
            gg = np.nan_to_num(g[:, mem])
            corr_acc += np.nan_to_num(np.corrcoef(gg))
            corr_n += 1
        for t in ("y2", "y5"):
            ok = np.isfinite(gy[t])
            if ok.sum() < 300:
                continue
            for mn, cols in models.items():
                xm = np.column_stack([np.ones(ok.sum())] + [np.nan_to_num(g[c, ok]) for c in cols])
                fm[(mn, t)][k] = np.linalg.lstsq(xm, gy[t][ok], rcond=None)[0]
        if k % 100 == 0:
            print(f"day {k}/{t_count} {time.time() - t0:.0f}s", flush=True)
    n_tests = nf * len(TARGETS)
    from scipy.stats import norm
    bonf = float(norm.ppf(1 - 0.025 / n_tests))
    uni = []
    for i, nm in enumerate(names):
        row = {"feature": nm}
        for t in TARGETS:
            s = ic[t][:, i]
            by = {int(y): float(np.nanmean(s[years == y])) for y in (2020, 2021, 2022)}
            tt = nw_t(s, NW_LAGS[t])
            same = len({np.sign(v) for v in by.values()}) == 1
            row[t] = {"ic": float(np.nanmean(s)), "nw_t": tt, "by_year": by, "same_sign_all_years": same,
                      "significant": bool(tt is not None and abs(tt) > bonf and same)}
        b_ = book[:, i]
        mt = float(np.nanmean(tau[:, i]))
        row["book_y2"] = {"sr": sr(b_), "sr_by_year": {int(y): sr(b_[years == y]) for y in (2020, 2021, 2022)},
                          "mean_daily_bps": float(np.nanmean(b_) * 1e4), "tau": mt,
                          "breakeven_bps": float(abs(np.nanmean(b_)) / mt * 1e4) if mt > 0 else None}
        uni.append(row)
    fm_out = {}
    for (mn, t), coefs in fm.items():
        cols = ["intercept"] + [names[c] for c in models[mn]]
        fm_out.setdefault(mn, {})[t] = {c: {"coef": float(np.nanmean(coefs[:, j])), "nw_t": nw_t(coefs[:, j], NW_LAGS[t])}
                                        for j, c in enumerate(cols)}
    corr = corr_acc / max(corr_n, 1)
    out = {"n_days": t_count, "n_features": nf, "n_tests": n_tests, "bonferroni_abs_t": bonf, "univariate": uni,
           "fama_macbeth": fm_out, "mean_xs_corr": {"names": names, "matrix": np.round(corr, 3).tolist()},
           "seconds": time.time() - t0}
    (OUT / "analyze.json").write_text(json.dumps(out, indent=1))
    np.savez(OUT / "analyze_series.npz", names=np.array(names), book=book, tau=tau, **{f"ic_{t}": ic[t] for t in TARGETS})
    print(f"analyze done {time.time() - t0:.0f}s bonf|t|>{bonf:.2f}")


# ---------------------------------------------------------------- stage: model (fit <ols|hgb:SET>, then eval)
MODEL_SETS = {"CTRL": CONTROLS,
              "CTRL+FF49": CONTROLS + [f"ff49_{k}" for k in CORE],
              "FULL": CONTROLS + [f"ff49_{k}" for k in CORE] + [f"c30_{k}" for k in FULL]
              + [f"c10_{k}" for k in CORE] + [f"c100_{k}" for k in CORE]}
CHUNK = 250_000


def panel(R, names):
    """Pooled member-day rows (day-major, member columns ascending): per-day normal scores, NaN -> 0; target gauss(y5)."""
    mem_all = R.member[R.begin:R.end][:, R.cols]
    offs = np.concatenate([[0], np.cumsum(mem_all.sum(1))])
    X = np.zeros((int(offs[-1]), len(names)), np.float32)
    Y = np.full(int(offs[-1]), np.nan, np.float32)
    for j, nm in enumerate(names + ["target_y5"]):
        arr = np.load(OUT / "feat" / f"{nm}.npy")
        for k in range(mem_all.shape[0]):
            f = arr[k][mem_all[k]].astype(float)
            ok = np.isfinite(f)
            if ok.sum() < 300:
                continue
            if nm == "target_y5":
                v = np.full(f.size, np.nan)
                v[ok] = gauss(f[ok])
                Y[offs[k]:offs[k + 1]] = v
            else:
                v = np.zeros(f.size)
                v[ok] = gauss(f[ok])
                X[offs[k]:offs[k + 1], j] = v
        del arr
    return X, Y, offs, mem_all


def split_days(R):
    years = R.years[R.begin:R.end]
    return np.flatnonzero(years < 2022)[:-7], np.flatnonzero(years == 2022)  # embargo: FIT y5 must end in 2021


def stage_model_fit(which):
    t0 = time.time()
    R = Role(prices=False)
    names = MODEL_SETS["FULL"]
    X, Y, offs, _ = panel(R, names)
    fit_days, _ = split_days(R)
    fit_rows = np.concatenate([np.arange(offs[k], offs[k + 1]) for k in fit_days])
    fit_rows = fit_rows[np.isfinite(Y[fit_rows])]
    print(f"panel {X.shape} fit rows {fit_rows.size} {time.time() - t0:.0f}s", flush=True)
    (OUT / "pred").mkdir(parents=True, exist_ok=True)
    meta = {}
    if which == "ols":
        for sname, cols in MODEL_SETS.items():
            ci = [names.index(c) for c in cols]
            xtx, xty = np.zeros((len(ci) + 1,) * 2), np.zeros(len(ci) + 1)
            for s in range(0, fit_rows.size, CHUNK):
                r_ = fit_rows[s:s + CHUNK]
                xc = np.column_stack([np.ones(r_.size), X[r_][:, ci].astype(float)])
                xtx += xc.T @ xc
                xty += xc.T @ Y[r_].astype(float)
            beta = np.linalg.solve(xtx, xty)
            pred = np.concatenate([np.column_stack([np.ones(min(CHUNK, X.shape[0] - s)), X[s:s + CHUNK][:, ci]]) @ beta
                                   for s in range(0, X.shape[0], CHUNK)])
            np.save(OUT / "pred" / f"ols_{sname}.npy", pred.astype(np.float32))
            meta[f"ols_{sname}"] = dict(zip(["intercept"] + cols, np.round(beta, 5).tolist()))
    else:
        from sklearn.ensemble import HistGradientBoostingRegressor
        sname = which.split(":", 1)[1]
        ci = [names.index(c) for c in MODEL_SETS[sname]]
        sub = np.sort(np.random.default_rng(0).choice(fit_rows, size=min(1_000_000, fit_rows.size), replace=False))
        hgb = HistGradientBoostingRegressor(learning_rate=0.05, max_iter=300, max_leaf_nodes=31, min_samples_leaf=5000,
                                            l2_regularization=1.0, max_bins=64, early_stopping=False, random_state=0)
        hgb.fit(X[sub][:, ci], Y[sub])
        print(f"hgb fit {time.time() - t0:.0f}s", flush=True)
        pred = np.concatenate([hgb.predict(X[s:s + CHUNK][:, ci]) for s in range(0, X.shape[0], CHUNK)])
        np.save(OUT / "pred" / f"hgb_{sname}.npy", pred.astype(np.float32))
        meta[f"hgb_{sname}"] = {"subsample": int(sub.size), "n_iter": int(hgb.n_iter_)}
    mp = OUT / "pred" / "meta.json"
    old = json.loads(mp.read_text()) if mp.exists() else {}
    old.update(meta)
    mp.write_text(json.dumps(old, indent=1))
    print(f"model fit {which} done {time.time() - t0:.0f}s")


def stage_model_eval():
    t0 = time.time()
    R = Role(prices=False)
    mem_all = R.member[R.begin:R.end][:, R.cols]
    offs = np.concatenate([[0], np.cumsum(mem_all.sum(1))])
    fit_days, hold_days = split_days(R)
    period = np.full(mem_all.shape[0], "", object)
    period[fit_days], period[hold_days] = "FIT", "HOLD"
    models = sorted(p.stem for p in (OUT / "pred").glob("*.npy"))
    preds = {m: np.load(OUT / "pred" / f"{m}.npy") for m in models}
    basis = {b: np.load(OUT / "feat" / f"{b}.npy", mmap_mode="r") for b in BASIS}
    ys = {t: np.load(OUT / "feat" / f"target_{t}.npy", mmap_mode="r") for t in ("y2", "y5")}
    hls = (0, 2, 5, 10)
    ic = {(m, t): np.full(mem_all.shape[0], np.nan) for m in models for t in ys}
    ret = {(m, h): np.full(mem_all.shape[0], np.nan) for m in models for h in hls}
    tau = {(m, h): np.full(mem_all.shape[0], np.nan) for m in models for h in hls}
    state = {}
    for k in range(mem_all.shape[0]):
        if not period[k]:
            continue
        if k in (fit_days[0], hold_days[0]):
            state = {}  # no smoothing state crosses the FIT/HOLD boundary
        mem = mem_all[k]
        b = np.stack([np.asarray(basis[x][k], float)[mem] for x in BASIS])
        b = np.where(np.isfinite(b), b, np.nanmedian(b, axis=1, keepdims=True))
        q, _ = np.linalg.qr(np.column_stack([np.ones(mem.sum())] + [gauss(r) for r in b]))
        yk = {t: np.asarray(ys[t][k], float)[mem] for t in ys}
        gy = {t: gauss(yk[t]) for t in ys}
        y2 = np.nan_to_num(yk["y2"])
        for m in models:
            p = gauss(preds[m][offs[k]:offs[k + 1]].astype(float))
            for t in ys:
                ok = np.isfinite(gy[t])
                ic[(m, t)][k] = np.corrcoef(p[ok], gy[t][ok])[0, 1]
            for h in hls:
                a = 1.0 if h == 0 else 1 - 2 ** (-1 / h)
                prev_s, prev_w = state.get((m, h), (None, None))
                full = np.zeros(R.cols.size)
                full[mem] = p
                s = full if prev_s is None else (1 - a) * prev_s + a * full
                x = s[mem] - q @ (q.T @ s[mem])
                w = np.zeros(R.cols.size)
                w[mem] = x / np.abs(x).sum()
                ret[(m, h)][k] = w[mem] @ y2
                if prev_w is not None:
                    tau[(m, h)][k] = np.abs(w - prev_w).sum()
                state[(m, h)] = (s, w)
        if k % 100 == 0:
            print(f"eval day {k} {time.time() - t0:.0f}s", flush=True)
    out = {"fit_days": int(fit_days.size), "hold_days": int(hold_days.size),
           "meta": json.loads((OUT / "pred" / "meta.json").read_text()), "models": {}}
    for m in models:
        res = {}
        for per, days in (("FIT", fit_days), ("HOLD", hold_days)):
            r_ = {"ic_y2": float(np.nanmean(ic[(m, "y2")][days])), "nw_t_y2": nw_t(ic[(m, "y2")][days], 5),
                  "ic_y5": float(np.nanmean(ic[(m, "y5")][days])), "nw_t_y5": nw_t(ic[(m, "y5")][days], 10), "books": {}}
            for h in hls:
                rr, tt = ret[(m, h)][days], tau[(m, h)][days]
                mt = float(np.nanmean(tt))
                r_["books"][f"hl{h}"] = {
                    "sr_gross": sr(rr), "mean_bps": float(np.nanmean(rr) * 1e4), "tau": mt,
                    "breakeven_bps": float(np.nanmean(rr) / mt * 1e4),
                    "sr_net": {f"{c}bps": sr(rr - c * 1e-4 * np.nan_to_num(tt)) for c in (1, 2, 5, 10)}}
            res[per] = r_
        out["models"][m] = res
    pairs = {}
    for a_, b_ in (("hgb_FULL", "hgb_CTRL+FF49"), ("ols_FULL", "ols_CTRL+FF49"), ("ols_CTRL+FF49", "ols_CTRL"),
                   ("hgb_FULL", "ols_FULL")):
        if a_ in models and b_ in models:
            d = ic[(a_, "y2")][hold_days] - ic[(b_, "y2")][hold_days]
            pairs[f"{a_} - {b_}"] = {"diff": float(np.nanmean(d)), "nw_t": nw_t(d, 5)}
    out["paired_hold_ic_y2"] = pairs
    out["seconds"] = time.time() - t0
    (OUT / "model.json").write_text(json.dumps(out, indent=1))
    np.savez(OUT / "model_series.npz", sessions=R.sessions[R.begin:R.end], period=period.astype(str),
             **{f"ret|{m}|hl{h}": ret[(m, h)] for m in models for h in hls},
             **{f"tau|{m}|hl{h}": tau[(m, h)] for m in models for h in hls},
             **{f"ic|{m}|{t}": ic[(m, t)] for m in models for t in ys})
    print(f"model eval done {time.time() - t0:.0f}s")


if __name__ == "__main__":
    stage = sys.argv[1]
    if stage == "clusters":
        stage_clusters(sys.argv[2])
    elif stage == "features":
        stage_features(sys.argv[2])
    elif stage == "analyze":
        stage_analyze()
    elif stage == "model_fit":
        stage_model_fit(sys.argv[2])
    elif stage == "model_eval":
        stage_model_eval()
    else:
        raise SystemExit(f"unknown stage {stage}")
