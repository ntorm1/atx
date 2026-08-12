"""Cross-sectional single-name long-vol ranking — walk-forward evaluation.

Input: the TSV emitted by `atx-vol-vega-panel` (one row per (entry date, symbol);
label = h-session daily-rehedged delta-neutral PnL of a 1y ATMF strangle sized
to a constant structure vega; label_valid=0 rows are features-only).

Game: each entry date, rank all names; BUY the top decile equal-vega. The
portfolio metric is the mean realized label of the chosen decile per entry
date (an overlapping-vintage series when h > 1 — Newey-West / block bootstrap
handle the overlap).

Model: per-date cross-sectional rank transform of features -> ridge (default)
or gradient boosting on pooled history, walk-forward with purge/embargo of
`horizon + embargo` sessions between train labels and test entries, refit
every `refit` sessions.

Baselines: long-ALL equal-vega, random decile, bottom decile, single-feature
ranks (Goyal-Saretto rv252-iv1y, Vasquez term slope, ivrv_1y_63), and the
realized oracle decile.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys

import numpy as np
import pandas as pd
from scipy import stats as sstats
from sklearn.impute import SimpleImputer
from sklearn.linear_model import Ridge
from sklearn.pipeline import Pipeline
from sklearn.preprocessing import StandardScaler

BASE_FEATURES = [
    "iv_1m", "iv_3m", "iv_1y", "term_slope_1m_1y", "fwd_vol_1m_1y",
    "skew_1m", "curv_1m", "skew_1y", "curv_1y", "rr25_1y", "bf25_1y",
    "rv_21", "rv_63", "rv_252", "ivrv_1y_21", "ivrv_1y_63",
    "ret_21d", "div_1y_21", "vol_of_vol_21", "iv_1y_rank_252",
    "entry_gamma", "entry_theta", "entry_delta_net",
]
DERIVED = ["gs_rv252_iv1y", "iv_ratio_1y_1m", "strangle_width"]


def load_panel(path: str) -> pd.DataFrame:
    df = pd.read_csv(path, sep="\t")
    df["key"] = pd.to_datetime(df["key"])
    df = df.sort_values(["key", "symbol"]).reset_index(drop=True)
    df["gs_rv252_iv1y"] = df["rv_252"] - df["iv_1y"]
    df["iv_ratio_1y_1m"] = df["iv_1y"] / df["iv_1m"]
    df["strangle_width"] = (df["strike_call"] - df["strike_put"]) / df["spot"]
    return df


def xsec_rank(df: pd.DataFrame, cols: list[str]) -> pd.DataFrame:
    """Per-date cross-sectional percentile rank in [-0.5, 0.5]; NaN-safe."""
    out = df.copy()
    g = df.groupby("key")
    for c in cols:
        out[c] = g[c].rank(pct=True) - 0.5
    return out


def winsorize_to(y: np.ndarray, q: float) -> np.ndarray:
    lo, hi = np.nanquantile(y, q), np.nanquantile(y, 1 - q)
    return np.clip(y, lo, hi)


def make_model(kind: str, seed: int, alpha: float):
    if kind == "ridge":
        return Pipeline([
            ("imp", SimpleImputer(strategy="median")),
            ("sc", StandardScaler()),
            ("m", Ridge(alpha=alpha)),
        ])
    if kind == "hgb":
        from sklearn.ensemble import HistGradientBoostingRegressor
        return HistGradientBoostingRegressor(
            max_iter=150, learning_rate=0.08, max_depth=4, random_state=seed)
    raise SystemExit(f"unknown model {kind}")


def decile_pick(scores: np.ndarray, frac: float) -> np.ndarray:
    n = len(scores)
    k = max(1, int(math.ceil(n * frac)))
    order = np.argsort(-scores)
    mask = np.zeros(n, dtype=bool)
    mask[order[:k]] = True
    return mask


def block_bootstrap_mean(series: np.ndarray, n_draws: int, block: int,
                         seed: int) -> tuple[float, float, float]:
    """P(mean>0), 5% and 95% quantiles of the mean under a circular block draw."""
    rng = np.random.default_rng(seed)
    n = len(series)
    if n < block * 2:
        return float("nan"), float("nan"), float("nan")
    means = np.empty(n_draws)
    n_blocks = int(math.ceil(n / block))
    for i in range(n_draws):
        starts = rng.integers(0, n, size=n_blocks)
        idx = (starts[:, None] + np.arange(block)[None, :]).ravel() % n
        means[i] = series[idx[:n]].mean()
    return float((means > 0).mean()), float(np.quantile(means, 0.05)), float(np.quantile(means, 0.95))


def walk_forward(df: pd.DataFrame, args) -> tuple[pd.DataFrame, pd.DataFrame]:
    feats = [c for c in BASE_FEATURES + DERIVED if c in df.columns]
    if args.drop_regex:
        import re
        feats = [c for c in feats if not re.search(args.drop_regex, c)]
    ranked = xsec_rank(df, feats)
    valid = ranked[ranked["label_valid"] == 1].copy()
    dates = sorted(valid["key"].unique())
    date_ix = {d: i for i, d in enumerate(dates)}
    purge = args.horizon + args.embargo

    rows = []
    daily = []
    model = None
    rng = np.random.default_rng(args.seed)
    for i, d in enumerate(dates):
        if i < args.min_train + purge:
            continue
        if model is None or (i - (args.min_train + purge)) % args.refit == 0:
            train = valid[valid["key"].isin(dates[: i - purge])]
            X = train[feats].to_numpy(dtype=float)
            y = winsorize_to(train["label_pnl_h"].to_numpy(dtype=float), args.winsor)
            model = make_model(args.model, args.seed, args.ridge_alpha)
            model.fit(X, y)
        day = valid[valid["key"] == d]
        if len(day) < args.min_names:
            continue
        X = day[feats].to_numpy(dtype=float)
        yhat = model.predict(X)
        y = day["label_pnl_h"].to_numpy(dtype=float)
        n = len(day)

        picks = {
            "model": decile_pick(yhat, args.top_frac),
            "oracle": decile_pick(y, args.top_frac),
            "random": decile_pick(rng.standard_normal(n), args.top_frac),
            "bottom": decile_pick(-yhat, args.top_frac),
            "gs": decile_pick(day["gs_rv252_iv1y"].fillna(-9e9).to_numpy(), args.top_frac),
            "slope": decile_pick(-day["term_slope_1m_1y"].fillna(9e9).to_numpy(), args.top_frac),
            "ivrv": decile_pick(-day["ivrv_1y_63"].fillna(9e9).to_numpy(), args.top_frac),
        }
        ic = sstats.spearmanr(yhat, y).statistic if n >= 5 else float("nan")
        rec = {
            "key": d, "n": n, "rank_ic": ic, "all_mean": float(np.mean(y)),
        }
        for name, m in picks.items():
            rec[f"{name}_mean"] = float(np.mean(y[m]))
        rec["picked"] = ",".join(sorted(day["symbol"].to_numpy()[picks["model"]]))
        daily.append(rec)
        m = picks["model"]
        for j, (s, p, yy) in enumerate(zip(day["symbol"], yhat, y)):
            rows.append({"key": d, "symbol": s, "pred": p, "label": yy,
                         "picked": bool(m[j])})
    return pd.DataFrame(daily), pd.DataFrame(rows)


def summarize(daily: pd.DataFrame, args) -> dict:
    out = {"n_days": int(len(daily)), "mean_rank_ic": float(daily["rank_ic"].mean()),
           "ic_tstat_nw": float("nan")}
    if len(daily) > 30:
        ic = daily["rank_ic"].dropna().to_numpy()
        # Newey-West t-stat with horizon lags on the daily IC series
        L = args.horizon
        g = ic - ic.mean()
        gamma0 = float((g * g).mean())
        var = gamma0
        for l in range(1, min(L, len(g) - 1) + 1):
            w = 1 - l / (L + 1)
            var += 2 * w * float((g[l:] * g[:-l]).mean())
        out["ic_tstat_nw"] = float(ic.mean() / math.sqrt(var / len(ic)))
    for name in ["model", "oracle", "random", "bottom", "gs", "slope", "ivrv", "all"]:
        col = f"{name}_mean"
        if col in daily.columns:
            s = daily[col].dropna().to_numpy()
            p_pos, lo, hi = block_bootstrap_mean(s, args.bootstrap, args.horizon, args.seed)
            out[name] = {
                "mean_pnl_per_1kvega": float(np.mean(s)),
                "total": float(np.sum(s)),
                "p_mean_pos": p_pos, "ci5": lo, "ci95": hi,
            }
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--panel", required=True)
    ap.add_argument("--out-dir", default=".")
    ap.add_argument("--model", default="ridge", choices=["ridge", "hgb"])
    ap.add_argument("--ridge-alpha", type=float, default=10.0)
    ap.add_argument("--top-frac", type=float, default=0.10)
    ap.add_argument("--horizon", type=int, default=21)
    ap.add_argument("--embargo", type=int, default=1)
    ap.add_argument("--refit", type=int, default=21)
    ap.add_argument("--min-train", type=int, default=60,
                    help="minimum training DATES before first OOS day")
    ap.add_argument("--min-names", type=int, default=20)
    ap.add_argument("--winsor", type=float, default=0.01)
    ap.add_argument("--bootstrap", type=int, default=2000)
    ap.add_argument("--drop-regex", default="")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--tag", default="run")
    args = ap.parse_args()

    df = load_panel(args.panel)
    n_sym = df["symbol"].nunique()
    print(f"panel rows={len(df)} symbols={n_sym} dates={df['key'].nunique()} "
          f"valid={int((df['label_valid'] == 1).sum())}", flush=True)
    daily, per_name = walk_forward(df, args)
    if daily.empty:
        print("no OOS days — not enough history", flush=True)
        sys.exit(1)
    os.makedirs(args.out_dir, exist_ok=True)
    daily.to_csv(os.path.join(args.out_dir, f"xsec_daily_{args.tag}.tsv"),
                 sep="\t", index=False)
    per_name.to_csv(os.path.join(args.out_dir, f"xsec_pername_{args.tag}.tsv"),
                    sep="\t", index=False)
    summary = summarize(daily, args)
    with open(os.path.join(args.out_dir, f"xsec_summary_{args.tag}.json"), "w") as f:
        json.dump(summary, f, indent=2, default=str)
    print(json.dumps(summary, indent=2, default=str), flush=True)


if __name__ == "__main__":
    main()
