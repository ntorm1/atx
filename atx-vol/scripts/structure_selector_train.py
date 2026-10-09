#!/usr/bin/env python
"""Walk-forward training/evaluation of the SPY option-structure selector.

Input: the TSV emitted by ``atx-vol-structure-panel`` (one row per entry day:
features + one-day-hold delta-neutral PnL of the vega-normalized front (1M)
and back (1Y) ATMF straddles).

Strategy menu (vega-matched, so calendars derive linearly):
    S1 long_gamma_1m   = +pnl_front
    S2 long_vega_1y    = +pnl_back
    S3 fwd_vol         = pnl_back - pnl_front   (short 1M / long 1Y)
    S4 rev_fwd_vol     = pnl_front - pnl_back   (long 1M / short 1Y)

Model: two regressors predict next-day pnl_front / pnl_back from entry-day
features; the selector picks argmax of the four implied strategy PnLs.
Walk-forward: expanding window, periodic refit, optional embargo. Baselines:
each always-on strategy, uniform-random, and the realized-argmax oracle.

Usage:
    python structure_selector_train.py --panel C:/atx-data/structure-panel/spy_panel.tsv \
        [--model hgb|ridge] [--min-train 252] [--retrain-every 21] [--embargo 1] \
        [--out-dir C:/atx-data/structure-panel]
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass

import numpy as np
import pandas as pd

LABEL_COLS = ("pnl_front", "pnl_back")
META_COLS = ("key", "pnl_valid")
STRATEGIES = ("long_gamma_1m", "long_vega_1y", "fwd_vol", "rev_fwd_vol")


def load_panel(path: str, keep_invalid: bool = False) -> pd.DataFrame:
    df = pd.read_csv(path, sep="\t")
    if not keep_invalid:
        df = df[df["pnl_valid"] == 1].reset_index(drop=True)
    df["key"] = df["key"].astype(str)
    return df


def feature_columns(df: pd.DataFrame) -> list[str]:
    return [c for c in df.columns if c not in LABEL_COLS and c not in META_COLS]


def add_derived_features(df: pd.DataFrame) -> pd.DataFrame:
    """Trailing-window derived features. Everything uses data at or before the
    row's entry day (rolling windows are right-aligned), so no lookahead."""
    out = df.copy()

    def rank252(s: pd.Series) -> pd.Series:
        return s.rolling(252, min_periods=252).rank(pct=True)

    out["iv_1m_rank"] = rank252(out["iv_1m"])
    out["slope_rank"] = rank252(out["term_slope"])
    out["ivrv_rank"] = rank252(out["ivrv_1m_21"])
    out["vov_rank"] = rank252(out["vol_of_vol_21"])
    out["skew_1m_rank"] = rank252(out["skew_1m"])
    out["rv_ratio_5_21"] = out["rv5"] / out["rv21"]
    out["iv_ratio_1m_1y"] = out["iv_1m"] / out["iv_1y"]
    # yesterday's move in units of the option-implied daily sigma (gap surprise)
    out["gap_z"] = out["ret_1d"].abs() * np.sqrt(252.0) / out["iv_1m"]
    out["gap_z_5"] = out["ret_5d"].abs() * np.sqrt(252.0 / 5.0) / out["iv_1m"]
    return out


def add_interaction_features(df: pd.DataFrame) -> pd.DataFrame:
    """Regime-interaction products a linear model cannot form on its own —
    the tail decomposition showed the front-leg payoff flips sign with the
    gap-risk state, so give ridge those hinge directions explicitly."""
    out = df.copy()
    pairs = (
        ("iv_1m_rank", "gap_z"),
        ("iv_1m_rank", "vov_rank"),
        ("ivrv_rank", "gap_z"),
        ("slope_rank", "iv_1m_rank"),
        ("vov_rank", "gap_z"),
        ("ivrv_rank", "vov_rank"),
    )
    for a, b in pairs:
        out[f"x_{a}_{b}"] = out[a] * out[b]
    return out


def strategy_pnls(front: np.ndarray, back: np.ndarray) -> np.ndarray:
    """(n, 4) realized/predicted PnL per strategy from the two base series."""
    return np.column_stack([front, back, back - front, front - back])


def make_model(
    kind: str, seed: int, hgb_iters: int = 120, hgb_lr: float = 0.1, ridge_alpha: float = 10.0
):
    if kind == "hgb":
        from sklearn.ensemble import HistGradientBoostingRegressor

        return HistGradientBoostingRegressor(
            max_iter=hgb_iters,
            learning_rate=hgb_lr,
            max_depth=4,
            min_samples_leaf=20,
            early_stopping=False,
            random_state=seed,
        )
    if kind == "ridge":
        from sklearn.impute import SimpleImputer
        from sklearn.linear_model import Ridge
        from sklearn.pipeline import make_pipeline
        from sklearn.preprocessing import StandardScaler

        return make_pipeline(
            SimpleImputer(strategy="median"), StandardScaler(), Ridge(alpha=ridge_alpha)
        )
    if kind == "ens":
        return _Ensemble(
            [make_model("ridge", seed), make_model("hgb", seed, hgb_iters=60, hgb_lr=0.1)]
        )
    raise ValueError(f"unknown model kind: {kind}")


class _Ensemble:
    """Equal-weight average of member predictions (fit on the same window)."""

    def __init__(self, members):
        self.members = members

    def fit(self, x, y):
        for m in self.members:
            m.fit(x, y)
        return self

    def predict(self, x):
        return np.mean([m.predict(x) for m in self.members], axis=0)


@dataclass
class WalkForwardResult:
    keys: list[str]
    picks: np.ndarray  # (n_oos,) int strategy index
    pick_pnl: np.ndarray  # realized PnL of the pick
    pred: np.ndarray  # (n_oos, 4) predicted strategy PnLs
    realized: np.ndarray  # (n_oos, 4) realized strategy PnLs


def winsorize_to(train: np.ndarray, q: float) -> np.ndarray:
    """Clip TRAIN targets at the [q, 1-q] train quantiles (evaluation always
    uses raw PnL; this only tames the loss the model fits)."""
    if q <= 0.0:
        return train
    lo, hi = np.quantile(train, [q, 1.0 - q])
    return np.clip(train, lo, hi)


def walk_forward(
    df: pd.DataFrame,
    model_kind: str,
    min_train: int,
    retrain_every: int,
    embargo: int,
    seed: int,
    train_window: int = 0,
    winsor: float = 0.0,
    risk_adjust: int = 0,
    risk_lambda: float = 0.0,
    anchor: int = -1,
    anchor_margin: float = 0.0,
    banned: tuple[int, ...] = (),
    crisis_gate: float = 0.0,
    vol_target: int = 0,
    ridge_alpha: float = 10.0,
    score_ema: float = 0.0,
    exclude: tuple[str, ...] = (),
) -> WalkForwardResult:
    feats = [c for c in feature_columns(df) if c not in exclude]
    x = df[feats].to_numpy(dtype=float)
    y_front = df["pnl_front"].to_numpy(dtype=float)
    y_back = df["pnl_back"].to_numpy(dtype=float)
    realized_all = strategy_pnls(y_front, y_back)

    # Per-strategy trailing PnL vol for the risk-adjusted pick. shift(1) keeps
    # it strictly past-only: the label of day t-1 is realized by day t's close.
    if risk_adjust > 0 or vol_target > 0:
        window = vol_target if vol_target > 0 else risk_adjust
        vol_scale = (
            pd.DataFrame(realized_all)
            .shift(1)
            .rolling(window, min_periods=window)
            .std()
            .to_numpy()
        )
    else:
        vol_scale = np.ones_like(realized_all)

    if vol_target > 0:
        # Vol-target the STRUCTURES: each strategy is sized daily to a constant
        # trailing PnL vol (median across the panel keeps dollar-ish units).
        # Same transform applies to realized PnL, predictions' training targets
        # stay raw — the model still predicts raw PnL; scores divide by vol.
        ref = np.nanmedian(vol_scale)
        unit = np.where(np.isfinite(vol_scale) & (vol_scale > 0.0), ref / vol_scale, np.nan)
        realized_all = realized_all * unit

    n = len(df)
    keys: list[str] = []
    picks: list[int] = []
    pick_pnl: list[float] = []
    preds: list[np.ndarray] = []
    realized: list[np.ndarray] = []

    if model_kind in ("decomp", "blend"):
        # Physics-informed assembly: pnl ≈ θ·dt + ½ΓS²·R² + V·Δσ_atm (validated
        # identity R² 0.92/0.85 with the skew-ride term, whose expectation ≈ 0).
        # Forecast the two stochastic pieces — next-day variance and ATM vol
        # change per tenor — and assemble expected PnL from KNOWN entry greeks.
        date = pd.to_datetime(df["key"])
        decomp_dt = ((date.shift(-1) - date).dt.days / 365.25).to_numpy()
        r_next = np.log(df["spot"].shift(-1) / df["spot"]).to_numpy()
        tgt_ret2 = r_next**2
        tgt_ds1m = (df["iv_1m"].shift(-1) - df["iv_1m"]).to_numpy()
        tgt_ds1y = (df["iv_1y"].shift(-1) - df["iv_1y"]).to_numpy()
        spot2 = df["spot"].to_numpy() ** 2
        g_front = df["front_gamma"].to_numpy()
        th_front = df["front_theta"].to_numpy()
        g_back = df["back_gamma"].to_numpy()
        th_back = df["back_theta"].to_numpy()
        vega_unit = 1000.0  # entry_vega == vega_target by construction

    ema_state: np.ndarray | None = None
    for fold_start in range(min_train, n, retrain_every):
        train_end = max(1, fold_start - embargo)
        train_lo = max(0, train_end - train_window) if train_window > 0 else 0
        test_end = min(n, fold_start + retrain_every)
        if model_kind in ("decomp", "blend"):

            def fit_predict(target: np.ndarray, s: int) -> np.ndarray:
                m = make_model("ridge", s, ridge_alpha=ridge_alpha)
                t = target[train_lo:train_end]
                mask = np.isfinite(t)
                m.fit(x[train_lo:train_end][mask], winsorize_to(t[mask], winsor))
                return m.predict(x[fold_start:test_end])

            e_ret2 = np.maximum(fit_predict(tgt_ret2, seed), 0.0)
            e_ds1m = fit_predict(tgt_ds1m, seed + 1)
            e_ds1y = fit_predict(tgt_ds1y, seed + 2)
            sl = slice(fold_start, test_end)
            pf = (
                th_front[sl] * decomp_dt[sl]
                + 0.5 * g_front[sl] * spot2[sl] * e_ret2
                + vega_unit * e_ds1m
            )
            pb = (
                th_back[sl] * decomp_dt[sl]
                + 0.5 * g_back[sl] * spot2[sl] * e_ret2
                + vega_unit * e_ds1y
            )
            if model_kind == "blend":
                mf = make_model("ridge", seed, ridge_alpha=ridge_alpha)
                mb = make_model("ridge", seed + 1, ridge_alpha=ridge_alpha)
                mf.fit(x[train_lo:train_end], winsorize_to(y_front[train_lo:train_end], winsor))
                mb.fit(x[train_lo:train_end], winsorize_to(y_back[train_lo:train_end], winsor))
                pf = 0.5 * (pf + mf.predict(x[fold_start:test_end]))
                pb = 0.5 * (pb + mb.predict(x[fold_start:test_end]))
        else:
            mf = make_model(model_kind, seed, ridge_alpha=ridge_alpha)
            mb = make_model(model_kind, seed + 1, ridge_alpha=ridge_alpha)
            mf.fit(x[train_lo:train_end], winsorize_to(y_front[train_lo:train_end], winsor))
            mb.fit(x[train_lo:train_end], winsorize_to(y_back[train_lo:train_end], winsor))
            pf = mf.predict(x[fold_start:test_end])
            pb = mb.predict(x[fold_start:test_end])
        pred = strategy_pnls(pf, pb)
        if score_ema > 0.0:
            # causal EWMA of predictions across sessions (fold-spanning state):
            # damps day-to-day forecast noise before the argmax
            sm = np.empty_like(pred)
            for j in range(pred.shape[0]):
                ema_state = (
                    pred[j]
                    if ema_state is None
                    else score_ema * pred[j] + (1.0 - score_ema) * ema_state
                )
                sm[j] = ema_state
            pred = sm
        scale = vol_scale[fold_start:test_end]
        ok = np.isfinite(scale) & (scale > 0.0)
        if risk_lambda > 0.0:
            # mean-variance style penalty: keeps PnL units, damps tail-chasing
            score = np.where(ok, pred - risk_lambda * scale, pred)
        elif risk_adjust > 0 or vol_target > 0:
            score = np.where(ok, pred / scale, pred)
        else:
            score = pred
        if banned:
            score = score.copy()
            score[:, list(banned)] = -np.inf
        if crisis_gate > 0.0 and "iv_1m_rank" in df.columns:
            # Stress gate: with front IV in its top tail, the short-front-gamma
            # calendar's loss distribution is dominated by gap days — take the
            # short-gamma structures off the menu rather than trust a point
            # forecast of a fat tail.
            rank = df["iv_1m_rank"].to_numpy(dtype=float)[fold_start:test_end]
            stressed = np.isfinite(rank) & (rank > crisis_gate)
            score = score.copy()
            score[stressed, 2] = -np.inf  # fwd_vol (short front straddle)
            score[stressed, 3] = -np.inf  # rev_fwd_vol (short back straddle)
        pick = np.argmax(score, axis=1)
        if anchor >= 0:
            # deviate from the anchor strategy only when the predicted edge
            # clears the margin — hysteresis against selection noise
            edge = score[np.arange(len(pick)), pick] - score[:, anchor]
            pick = np.where(edge > anchor_margin, pick, anchor)
        for j, i in enumerate(range(fold_start, test_end)):
            keys.append(df["key"].iloc[i])
            picks.append(int(pick[j]))
            pick_pnl.append(float(realized_all[i, pick[j]]))
            preds.append(pred[j])
            realized.append(realized_all[i])

    return WalkForwardResult(
        keys=keys,
        picks=np.asarray(picks),
        pick_pnl=np.asarray(pick_pnl),
        pred=np.vstack(preds),
        realized=np.vstack(realized),
    )


def block_bootstrap_stats(
    model: np.ndarray, base: np.ndarray, n_boot: int, block: int, seed: int
) -> tuple[float, float, tuple[float, float]]:
    """Circular block bootstrap of the daily PnL series. Returns
    (P[model total > 0], P[model total > base total], model Sharpe 5-95% CI)."""
    rng = np.random.default_rng(seed)
    n = len(model)
    n_blocks = int(np.ceil(n / block))
    pos = beat = 0
    sharpes = np.empty(n_boot)
    for b in range(n_boot):
        starts = rng.integers(0, n, size=n_blocks)
        idx = (starts[:, None] + np.arange(block)[None, :]).ravel()[:n] % n
        m = model[idx]
        pos += m.sum() > 0.0
        beat += m.sum() > base[idx].sum()
        sharpes[b] = ann_sharpe(m)
    lo, hi = np.quantile(sharpes, [0.05, 0.95])
    return pos / n_boot, beat / n_boot, (float(lo), float(hi))


def predict_latest(
    df: pd.DataFrame,
    model_kind: str,
    train_window: int,
    winsor: float,
    seed: int,
    ridge_alpha: float,
    crisis_gate: float,
    exclude: tuple[str, ...],
) -> None:
    """Operational mode: fit on everything available, print the structure to
    hold from the LAST panel session to the next one. The last row's labels are
    for yesterday->today; its FEATURES are today's state, which is exactly the
    decision input."""
    feats = [c for c in feature_columns(df) if c not in exclude]
    x = df[feats].to_numpy(dtype=float)
    n = len(df)
    lo = max(0, n - train_window) if train_window > 0 else 0
    # train on all completed rows; the last row is the decision row
    mf = make_model(model_kind, seed, ridge_alpha=ridge_alpha)
    mb = make_model(model_kind, seed + 1, ridge_alpha=ridge_alpha)
    valid = (df["pnl_valid"].to_numpy() == 1)[lo : n - 1]
    xt = x[lo : n - 1][valid]
    mf.fit(xt, winsorize_to(df["pnl_front"].to_numpy()[lo : n - 1][valid], winsor))
    mb.fit(xt, winsorize_to(df["pnl_back"].to_numpy()[lo : n - 1][valid], winsor))
    pf = float(mf.predict(x[n - 1 : n])[0])
    pb = float(mb.predict(x[n - 1 : n])[0])
    score = strategy_pnls(np.array([pf]), np.array([pb]))[0]
    gated = ""
    if crisis_gate > 0.0 and "iv_1m_rank" in df.columns:
        rank = float(df["iv_1m_rank"].iloc[-1])
        if np.isfinite(rank) and rank > crisis_gate:
            score = score.copy()
            score[2] = score[3] = -np.inf
            gated = f"  [crisis gate ACTIVE: iv_1m_rank {rank:.2f} > {crisis_gate}]"
    pick = int(np.argmax(score))
    print(f"decision date: {df['key'].iloc[-1]}{gated}")
    for s, name in enumerate(STRATEGIES):
        marker = " <== HOLD" if s == pick else ""
        val = score[s] if np.isfinite(score[s]) else float("-inf")
        print(f"  {name:>14}: predicted 1-day PnL {val:>8.2f}{marker}")


def ann_sharpe(daily: np.ndarray) -> float:
    sd = daily.std(ddof=1)
    if not np.isfinite(sd) or sd == 0.0:
        return float("nan")
    return float(np.sqrt(252.0) * daily.mean() / sd)


def describe(name: str, daily: np.ndarray) -> str:
    return (
        f"{name:>18}: total {daily.sum():>10.1f}  mean {daily.mean():>7.3f}  "
        f"sharpe {ann_sharpe(daily):>6.2f}  worst {daily.min():>8.1f}"
    )


def report(res: WalkForwardResult, seed: int) -> str:
    lines: list[str] = []
    n = len(res.pick_pnl)
    lines.append(f"OOS days: {n}  ({res.keys[0]} .. {res.keys[-1]})")
    lines.append("")
    lines.append(describe("model", res.pick_pnl))
    for s, name in enumerate(STRATEGIES):
        lines.append(describe(f"always {name}", res.realized[:, s]))
    rng = np.random.default_rng(seed)
    random_pick = res.realized[np.arange(n), rng.integers(0, 4, size=n)]
    lines.append(describe("random", random_pick))
    oracle = res.realized.max(axis=1)
    lines.append(describe("oracle", oracle))
    lines.append("")
    hit = float((res.picks == np.argmax(res.realized, axis=1)).mean())
    capture = float(res.pick_pnl.sum() / oracle.sum()) if oracle.sum() != 0 else float("nan")
    lines.append(f"hit rate vs oracle: {hit:.3f}   oracle capture: {capture:.3f}")
    counts = np.bincount(res.picks, minlength=4)
    lines.append(
        "selection counts: "
        + "  ".join(f"{name}={counts[s]}" for s, name in enumerate(STRATEGIES))
    )
    switches = int((np.diff(res.picks) != 0).sum())
    lines.append(f"switches: {switches} ({switches / max(1, n - 1):.2%} of days)")
    lines.append("")
    lines.append("per-year:")
    years = pd.Series([k[:4] for k in res.keys])
    daily = pd.Series(res.pick_pnl)
    base = pd.DataFrame(res.realized, columns=list(STRATEGIES))
    for year, idx in years.groupby(years).groups.items():
        d = daily.iloc[idx].to_numpy()
        best_base = base.iloc[idx].sum().max()
        lines.append(
            f"  {year}: model {d.sum():>9.1f} (sharpe {ann_sharpe(d):>5.2f})  "
            f"best-always {best_base:>9.1f}"
        )
    return "\n".join(lines)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--panel", required=True)
    ap.add_argument(
        "--model", default="hgb", choices=("hgb", "ridge", "ens", "decomp", "blend")
    )
    ap.add_argument("--interactions", action="store_true", help="add regime product features")
    ap.add_argument("--min-train", type=int, default=252)
    ap.add_argument("--retrain-every", type=int, default=21)
    ap.add_argument("--embargo", type=int, default=1)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--out-dir", default=None)
    ap.add_argument("--train-window", type=int, default=0, help="rolling train days (0=expanding)")
    ap.add_argument("--winsor", type=float, default=0.0, help="train-target clip quantile (e.g. 0.01)")
    ap.add_argument("--no-derived", action="store_true", help="skip derived feature block")
    ap.add_argument(
        "--risk-adjust",
        type=int,
        default=0,
        help="pick argmax(pred / trailing N-day strategy PnL vol) instead of raw argmax",
    )
    ap.add_argument(
        "--risk-lambda",
        type=float,
        default=0.0,
        help="mean-variance pick: argmax(pred - lambda * trailing vol); needs --risk-adjust window",
    )
    ap.add_argument(
        "--anchor",
        default=None,
        choices=STRATEGIES,
        help="default strategy; deviate only when predicted edge > --anchor-margin",
    )
    ap.add_argument("--anchor-margin", type=float, default=0.0)
    ap.add_argument(
        "--ban", action="append", default=[], choices=STRATEGIES, help="exclude from selection"
    )
    ap.add_argument(
        "--crisis-gate",
        type=float,
        default=0.0,
        help="iv_1m 252d percentile above which short-straddle strategies are off-menu",
    )
    ap.add_argument(
        "--vol-target",
        type=int,
        default=0,
        help="size every strategy to constant trailing N-day PnL vol (rescales the whole game)",
    )
    ap.add_argument("--tag", default=None, help="suffix for the eval TSV name")
    ap.add_argument("--bootstrap", type=int, default=0, help="block-bootstrap draws (0=off)")
    ap.add_argument("--drop-regex", default=None, help="drop feature columns matching this regex")
    ap.add_argument("--ridge-alpha", type=float, default=10.0)
    ap.add_argument("--score-ema", type=float, default=0.0, help="EWMA alpha for prediction smoothing")
    ap.add_argument(
        "--predict-latest",
        action="store_true",
        help="fit on all data, print the recommended structure for the next session",
    )
    args = ap.parse_args()

    df = load_panel(args.panel, keep_invalid=args.predict_latest)
    if not args.no_derived:
        df = add_derived_features(df)
    if args.interactions:
        df = add_interaction_features(df)
    exclude: tuple[str, ...] = ()
    if args.drop_regex:
        import re

        rx = re.compile(args.drop_regex)
        exclude = tuple(c for c in feature_columns(df) if rx.search(c))
        print(f"excluded {len(exclude)} features: {', '.join(exclude)}")
    print(f"panel: {len(df)} valid rows, {len(feature_columns(df))} features")

    if args.predict_latest:
        predict_latest(
            df,
            model_kind=args.model,
            train_window=args.train_window,
            winsor=args.winsor,
            seed=args.seed,
            ridge_alpha=args.ridge_alpha,
            crisis_gate=args.crisis_gate,
            exclude=exclude,
        )
        return

    res = walk_forward(
        df,
        model_kind=args.model,
        min_train=args.min_train,
        retrain_every=args.retrain_every,
        embargo=args.embargo,
        seed=args.seed,
        train_window=args.train_window,
        winsor=args.winsor,
        risk_adjust=args.risk_adjust,
        risk_lambda=args.risk_lambda,
        anchor=STRATEGIES.index(args.anchor) if args.anchor else -1,
        anchor_margin=args.anchor_margin,
        banned=tuple(STRATEGIES.index(b) for b in args.ban),
        crisis_gate=args.crisis_gate,
        vol_target=args.vol_target,
        ridge_alpha=args.ridge_alpha,
        score_ema=args.score_ema,
        exclude=exclude,
    )
    print(report(res, seed=args.seed))

    if args.bootstrap > 0:
        base = res.realized[:, STRATEGIES.index("long_vega_1y")]
        p_pos, p_beat, (s_lo, s_hi) = block_bootstrap_stats(
            res.pick_pnl, base, n_boot=args.bootstrap, block=21, seed=args.seed
        )
        print(
            f"bootstrap({args.bootstrap}, block=21): P(total>0)={p_pos:.3f}  "
            f"P(beats always-long-vega)={p_beat:.3f}  sharpe 5-95% [{s_lo:.2f}, {s_hi:.2f}]"
        )

    if args.out_dir:
        out = pd.DataFrame(
            {
                "key": res.keys,
                "pick": [STRATEGIES[p] for p in res.picks],
                "pick_pnl": res.pick_pnl,
                **{f"pred_{name}": res.pred[:, s] for s, name in enumerate(STRATEGIES)},
                **{f"real_{name}": res.realized[:, s] for s, name in enumerate(STRATEGIES)},
            }
        )
        tag = args.tag if args.tag else args.model
        path = f"{args.out_dir}/selector_eval_{tag}.tsv"
        out.to_csv(path, sep="\t", index=False)
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
