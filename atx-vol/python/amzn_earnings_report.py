#!/usr/bin/env python3
"""Render the VolaDynamics "AMZN around earnings" figure set from fitter CSVs.

Reads the CSV/JSON contract documented in ``amzn_report_schema.md`` (emitted by
the C++ ``amzn_earnings_report`` tool, or by ``make_synthetic_amzn_csvs.py`` for
development) and renders the nine VolaDynamics figures as PNGs plus a self-
contained ``index.html`` that embeds them with short captions.

    python amzn_earnings_report.py --in <csvdir> --out <pngdir>

The snapshot is real AMZN OPRA cbbo-1m marks at 2018-04-26 15:45 ET (spot
~ $1519, pre-split), 17 expiries (1 DTE -> LEAP). The near-term smile carries an
extreme negative ATF curvature "W-shape" (c2_eff ~ -1.1 on the front expiry).

Conventions (VolaDynamics; see the schema): the normalized smile is
``f(z) = 1 + s2*z + 1/2*c2*z^2 + ...`` with ``z = log(K/F)/(sigma0*sqrt(T))``;
the reported ``c2`` is ``c2_eff = f''(0)`` (curvature including the active modes).

Matplotlib ONLY (the one plotting dep in this repo). Pure numpy + pandas +
stdlib otherwise. No seaborn/plotly, no network. Saves 150-dpi PNGs.
"""

from __future__ import annotations

import argparse
import html
import json
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # headless
import matplotlib.colors as mcolors
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import numpy as np
import pandas as pd

# ── palette (matches tools/tearsheet.py + mag7_dispersion_report.py) ────────
INK = "#1b1b2f"
MUTE = "#6b7280"
GRID = "#e6e6ea"
PAPER = "#ffffff"
ACCENT = "#0f766e"     # teal
NEG = "#b4232a"        # red
POS = "#15803d"        # green
CMAP = plt.cm.viridis  # per-expiry colormap, keyed by T (days to expiry)

TITLE_BAND = "AMZN around earnings — 2018-04-26 15:45 ET (real OPRA cbbo-1m)"

# VolaDynamics' published AMZN example displays only the 10 near-term expiries
# through 2018-08-17 (not the 2019–2020 LEAPs). The near-term surface figures
# (1/4/5/8) restrict to this window; the term-structure (6/7) and earnings (9)
# figures stay full-range (all 17 expiries) since they are clean and richer there.
VOLA_WINDOW_DTE = 120

# Headline near-money band VolaDynamics uses for the calendar-arb proof.
NEAR_MONEY_K = 0.30

# Figures, in schema order: (filename, short caption for index.html).
FIGURES = [
    ("fig1_ns_surface.png",
     "NS-space surface: fitted IV vs z for the near-term expiries — the W-shape."),
    ("fig2_front_strike.png",
     "Front expiry in strike space: market marks + fit; the W and c2_eff annotated."),
    ("fig3_front_ns.png",
     "Front expiry in NS space (z): market + fit with the negative c2_eff annotated."),
    ("fig4_total_variance.png",
     "Total variance w vs log(K/F), the 10 near-term expiries over listed strikes — the calendar-arb proof."),
    ("fig5_tv_errorbars.png",
     "Total variance with market error bars vs the fit, per near expiry."),
    ("fig6_term_3param.png",
     "3-parameter term structure: sigma0(T), s2(T), c2_eff(T)."),
    ("fig7_term_8param.png",
     "8-parameter (C8) term structure: the base-5 + 3-mode parameters vs T."),
    ("fig8_multi_panels.png",
     "Per-expiry market-vs-fit panels across the near-term surface (over listed strikes)."),
    ("fig9_earnings.png",
     "Earnings decomposition: dirty vs censored ATM term curve, iEMove, event-variance share."),
]


# ── IO ──────────────────────────────────────────────────────────────────────
def load_inputs(indir: Path):
    """Load the schema files. Returns (meta, slices, tv, smiles, earn) where
    ``earn`` is None if the optional earnings files are absent."""
    meta_path = indir / "meta.json"
    meta = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.exists() else {}

    slices = pd.read_csv(indir / "slices.csv").sort_values("T").reset_index(drop=True)
    tv = pd.read_csv(indir / "total_variance.csv")
    smiles = pd.read_csv(indir / "smiles.csv")

    earn = None
    es = indir / "earnings_summary.csv"
    et = indir / "earnings_tenors.csv"
    if es.exists() and et.exists():
        earn = {
            "summary": pd.read_csv(es).iloc[0].to_dict(),
            "tenors": pd.read_csv(et).sort_values("T").reset_index(drop=True),
        }
    return meta, slices, tv, smiles, earn


# ── styling helpers ──────────────────────────────────────────────────────────
def _style_axis(ax) -> None:
    ax.set_facecolor(PAPER)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=MUTE, labelsize=8, length=0)
    ax.grid(True, color=GRID, linewidth=0.7, alpha=0.9)
    ax.set_axisbelow(True)


def _pct_axis(ax, axis: str = "y") -> None:
    fmt = mticker.FuncFormatter(lambda v, _: f"{v*100:.0f}%")
    (ax.yaxis if axis == "y" else ax.xaxis).set_major_formatter(fmt)


def _color_norm(slices: pd.DataFrame):
    """viridis keyed by days-to-expiry on a log scale (T spans 1 DTE -> ~1.7 yr)."""
    dtes = slices["dte"].astype(float)
    lo = max(float(dtes.min()), 1.0)
    hi = max(float(dtes.max()), lo + 1.0)
    return mcolors.LogNorm(vmin=lo, vmax=hi)


def _expiry_style(row) -> dict:
    """Line style for one expiry; slices flagged reverted (or NaN) are
    visually de-emphasized (thin, dashed, translucent)."""
    reverted = bool(row.get("reverted", 0)) or not np.isfinite(row.get("c2_eff", np.nan))
    if reverted:
        return {"lw": 1.0, "ls": (0, (3, 2)), "alpha": 0.45, "zorder": 2}
    return {"lw": 1.7, "ls": "-", "alpha": 0.95, "zorder": 4}


def _add_colorbar(fig, ax, norm, label="days to expiry"):
    sm = plt.cm.ScalarMappable(norm=norm, cmap=CMAP)
    sm.set_array([])
    cb = fig.colorbar(sm, ax=ax, pad=0.015, fraction=0.045)
    cb.set_label(label, color=MUTE, fontsize=8)
    cb.ax.tick_params(colors=MUTE, labelsize=7)
    cb.outline.set_edgecolor(GRID)
    return cb


def _finish(fig, out: Path, figtitle: str) -> None:
    """Reserve a top band, stamp the figure + snapshot titles, save + close."""
    fig.tight_layout(rect=(0.0, 0.0, 1.0, 0.90))
    fig.text(0.5, 0.965, figtitle, ha="center", va="top",
             fontsize=13.5, fontweight="bold", color=INK)
    fig.text(0.5, 0.925, TITLE_BAND, ha="center", va="top",
             fontsize=9.0, color=MUTE, style="italic")
    fig.savefig(out, dpi=150, facecolor=PAPER)
    plt.close(fig)


def _window(slices: pd.DataFrame) -> pd.DataFrame:
    """VolaDynamics near-term display window: expiries with dte ≤ VOLA_WINDOW_DTE
    (the first ~10, through 2018-08-17). Falls back to all rows if none qualify."""
    win = slices[slices["dte"].astype(float) <= VOLA_WINDOW_DTE]
    return win if len(win) else slices


def _quoted_k_range(smiles: pd.DataFrame, expiry_date):
    """[k_min, k_max] over the in-fit market strikes of one expiry (VolaDynamics
    draws each smile only over its listed strikes). None if no usable quotes."""
    g = smiles[smiles["expiry_date"] == expiry_date]
    inf = g[(g["in_fit"] == 1) & np.isfinite(g["k"])]
    if inf.empty:
        inf = g[np.isfinite(g["k"])]
    if inf.empty:
        return None
    return float(inf["k"].min()), float(inf["k"].max())


def _clip_to_quotes(block: pd.DataFrame, krange):
    """Clip a dense fitted grid to a slice's quoted k-range (no extrapolation
    beyond listed strikes). Returns block unchanged if krange is None."""
    if krange is None or block.empty:
        return block
    lo, hi = krange
    return block[(block["k"] >= lo) & (block["k"] <= hi)]


def _calendar_scan(slices: pd.DataFrame, tv: pd.DataFrame, smiles: pd.DataFrame):
    """Windowed + quote-clipped calendar-arb scan.

    Counts total-variance crossings (a later-T curve dipping below an earlier-T
    one) separately in the near-money band |k| ≤ NEAR_MONEY_K (VolaDynamics'
    headline region) and in the sparse deep wings |k| > NEAR_MONEY_K, comparing
    only where two consecutive included slices both quote strikes.

    Returns (n_near, n_wing, window_df).
    """
    win = _window(slices)
    kn = NEAR_MONEY_K
    near_grid = np.linspace(-kn, kn, 121)
    wing_grid = np.concatenate([np.linspace(-0.6, -kn - 1e-3, 40),
                                np.linspace(kn + 1e-3, 0.6, 40)])
    prev_n = prev_w = None
    n_near = n_wing = 0
    for _, row in win.iterrows():
        block = tv[tv["expiry_date"] == row["expiry_date"]].sort_values("k")
        block = block[np.isfinite(block["w"])]
        block = _clip_to_quotes(block, _quoted_k_range(smiles, row["expiry_date"]))
        if len(block) < 3:
            continue
        kk, ww = block["k"].values, block["w"].values
        ni = np.interp(near_grid, kk, ww, left=np.nan, right=np.nan)
        wi = np.interp(wing_grid, kk, ww, left=np.nan, right=np.nan)
        if prev_n is not None:
            b = np.isfinite(ni) & np.isfinite(prev_n)
            if b.any() and np.nanmin((ni - prev_n)[b]) < -1e-9:
                n_near += 1
        if prev_w is not None:
            b = np.isfinite(wi) & np.isfinite(prev_w)
            if b.any() and np.nanmin((wi - prev_w)[b]) < -1e-9:
                n_wing += 1
        prev_n, prev_w = ni, wi
    return n_near, n_wing, win


def _fmt_c2(v) -> str:
    return "n/a" if not np.isfinite(v) else f"{v:+.2f}"


# ── figure 1 : NS-space surface ──────────────────────────────────────────────
def fig1_ns_surface(slices, tv, smiles, out: Path):
    fig, ax = plt.subplots(figsize=(10.5, 6.6), facecolor=PAPER)
    _style_axis(ax)
    norm = _color_norm(slices)
    near = _window(slices)  # VolaDynamics near-term display (dte ≤ 120)
    drawn = 0
    for _, row in near.iterrows():
        block = tv[tv["expiry_date"] == row["expiry_date"]].sort_values("z")
        block = block[np.isfinite(block["fit_iv"])]
        block = _clip_to_quotes(block, _quoted_k_range(smiles, row["expiry_date"]))
        if len(block) < 3:
            continue
        st = _expiry_style(row)
        ax.plot(block["z"], block["fit_iv"], color=CMAP(norm(row["dte"])),
                label=f"{int(row['dte'])}d", **st)
        drawn += 1
    ax.axvline(0.0, color=MUTE, lw=0.8, ls=(0, (4, 3)), alpha=0.6)
    _pct_axis(ax)
    ax.set_xlabel("z  =  log(K/F) / (σ₀·√T)   (normalized log-moneyness)",
                  fontsize=9, color=INK)
    ax.set_ylabel("fitted implied volatility", fontsize=9, color=INK)
    if drawn:
        _add_colorbar(fig, ax, norm)
    ax.text(0.015, 0.03,
            "near-term smiles are W-shaped: an ATM event bump between two troughs,\n"
            "with rising wings — a signature of extreme negative ATF curvature.",
            transform=ax.transAxes, va="bottom", ha="left", fontsize=8.5, color=MUTE)
    _finish(fig, out, "NS-space surface — fitted IV vs z (near-term W-shape)")


# ── figures 2 & 3 : front expiry, strike- and NS-space ───────────────────────
def _front(slices, tv, smiles):
    row = slices.iloc[0]
    block = tv[tv["expiry_date"] == row["expiry_date"]].sort_values("z")
    mkt = smiles[smiles["expiry_date"] == row["expiry_date"]].copy()
    return row, block, mkt


def _plot_market(ax, mkt, xcol):
    """Market dots with bid/ask error bars; hollow markers for out-of-fit."""
    if mkt.empty:
        return
    for in_fit, sub in mkt.groupby("in_fit"):
        good = sub[np.isfinite(sub["mkt_iv"])]
        if good.empty:
            continue
        face = INK if in_fit else "none"
        ax.errorbar(good[xcol], good["mkt_iv"], yerr=good["iv_err"].fillna(0.0),
                    fmt="o", ms=4.0, mfc=face, mec=INK, ecolor=MUTE,
                    elinewidth=0.8, capsize=1.8, alpha=0.9, zorder=5,
                    label="market (in fit)" if in_fit else "market (excluded)")


def fig2_front_strike(slices, tv, smiles, out: Path):
    row, block, mkt = _front(slices, tv, smiles)
    fig, ax = plt.subplots(figsize=(9.6, 6.4), facecolor=PAPER)
    _style_axis(ax)
    block = block[np.isfinite(block["fit_iv"])]
    F = float(row["F"])
    if len(block) >= 3:
        # strike axis: reconstruct K = F·exp(k) from the fitted grid
        ax.plot(F * np.exp(block["k"]), block["fit_iv"], color=ACCENT, lw=2.2,
                zorder=4, label="CStar fit")
    _plot_market(ax, mkt, "K")
    ax.axvline(F, color=MUTE, lw=0.8, ls=(0, (4, 3)), alpha=0.7)
    ax.annotate(f"F = {F:,.0f}", (F, ax.get_ylim()[1]), textcoords="offset points",
                xytext=(4, -12), fontsize=8, color=MUTE)
    _pct_axis(ax)
    ax.set_xlabel("strike  K", fontsize=9, color=INK)
    ax.set_ylabel("implied volatility", fontsize=9, color=INK)
    txt = (f"front expiry:  {int(row['dte'])} DTE\n"
           f"σ₀ = {row['sigma0']*100:.0f}%\n"
           f"s2 = {row['s2']:+.2f}\n"
           f"c2_eff = {_fmt_c2(row['c2_eff'])}   ← W-shape")
    ax.text(0.98, 0.97, txt, transform=ax.transAxes, va="top", ha="right",
            family="monospace", fontsize=9.5, color=INK,
            bbox=dict(boxstyle="round,pad=0.6", fc="#f7f7f5", ec=GRID, lw=1.0))
    _annotate_w(ax, block, F, xspace="K")
    ax.legend(loc="lower left", frameon=False, fontsize=8)
    _finish(fig, out, "Front expiry — strike space (market vs fit)")


def fig3_front_ns(slices, tv, smiles, out: Path):
    row, block, mkt = _front(slices, tv, smiles)
    fig, ax = plt.subplots(figsize=(9.6, 6.4), facecolor=PAPER)
    _style_axis(ax)
    block = block[np.isfinite(block["fit_iv"])]
    if len(block) >= 3:
        ax.plot(block["z"], block["fit_iv"], color=ACCENT, lw=2.2, zorder=4,
                label="CStar fit")
    _plot_market(ax, mkt, "z")
    ax.axvline(0.0, color=MUTE, lw=0.8, ls=(0, (4, 3)), alpha=0.7)
    _pct_axis(ax)
    ax.set_xlabel("z  =  log(K/F) / (σ₀·√T)", fontsize=9, color=INK)
    ax.set_ylabel("implied volatility", fontsize=9, color=INK)
    # annotate the negative ATF curvature at z=0
    if len(block) >= 3:
        i0 = int((block["z"].abs()).values.argmin())
        z0 = float(block["z"].iloc[i0]); v0 = float(block["fit_iv"].iloc[i0])
        ax.annotate(f"c2_eff = {_fmt_c2(row['c2_eff'])}\n(f''(0) < 0 — concave ATM)",
                    (z0, v0), textcoords="offset points", xytext=(18, 24),
                    fontsize=9.5, color=NEG, fontweight="bold",
                    arrowprops=dict(arrowstyle="->", color=NEG, lw=1.2))
    _annotate_w(ax, block, None, xspace="z")
    ax.legend(loc="upper right", frameon=False, fontsize=8)
    _finish(fig, out, "Front expiry — NS space (negative c2_eff)")


def _annotate_w(ax, block, F, xspace):
    """Label the two W troughs and the ATM bump, if the shape has them."""
    if len(block) < 5:
        return
    iv = block["fit_iv"].values
    z = block["z"].values
    x = (F * np.exp(block["k"].values)) if xspace == "K" else z
    # troughs: local minima on each side of z=0
    left = np.where(z < -0.2)[0]
    right = np.where(z > 0.2)[0]
    for side in (left, right):
        if len(side) >= 3:
            j = side[int(np.argmin(iv[side]))]
            ax.scatter([x[j]], [iv[j]], s=28, facecolor="none", edgecolor=NEG,
                       linewidths=1.3, zorder=6)
    ax.text(0.02, 0.97, "W-shape", transform=ax.transAxes, va="top", ha="left",
            fontsize=11, fontweight="bold", color=NEG)


# ── figure 4 : total variance vs k (calendar-arb proof) ──────────────────────
def fig4_total_variance(slices, tv, smiles, out: Path):
    fig, ax = plt.subplots(figsize=(10.5, 6.6), facecolor=PAPER)
    _style_axis(ax)
    norm = _color_norm(slices)
    # VolaDynamics window (dte ≤ 120) + draw each curve only over its listed
    # strikes (no deep-wing extrapolation). Verdict is driven by the shared scan.
    n_near, n_wing, win = _calendar_scan(slices, tv, smiles)
    for _, row in win.iterrows():
        block = tv[tv["expiry_date"] == row["expiry_date"]].sort_values("k")
        block = block[np.isfinite(block["w"])]
        block = _clip_to_quotes(block, _quoted_k_range(smiles, row["expiry_date"]))
        if len(block) < 3:
            continue
        st = _expiry_style(row)
        ax.plot(block["k"], block["w"], color=CMAP(norm(row["dte"])), **st)
    ax.axvspan(-NEAR_MONEY_K, NEAR_MONEY_K, color=POS, alpha=0.05, zorder=0)
    ax.set_xlabel("k  =  log(K/F)", fontsize=9, color=INK)
    ax.set_ylabel("total variance   w = T·σ²", fontsize=9, color=INK)
    _add_colorbar(fig, ax, norm)
    if n_near == 0:
        note = ("calendar-arbitrage-free: total-variance curves ordered by T,\n"
                "non-crossing over listed strikes (|k| ≤ 0.3).")
        col = POS
    else:
        note = (f"WARNING: {n_near} near-money calendar-arb crossing(s) on |k| ≤ 0.3\n"
                "(total variance not monotone in T at some near strike).")
        col = NEG
    ax.text(0.015, 0.97, note, transform=ax.transAxes, va="top", ha="left",
            fontsize=9.5, color=col, fontweight="bold",
            bbox=dict(boxstyle="round,pad=0.5", fc="#f7f7f5", ec=GRID, lw=1.0))
    # Deep-wing sparse-quote caveat: neutral grey footnote, NOT a red warning,
    # when the headline near-money band is clean.
    if n_wing and n_near == 0:
        ax.text(0.015, 0.02,
                f"note: {n_wing} sparse deep-wing (|k| > 0.3) crossing(s) among the\n"
                "widest-quoted expiries — a known thin-quote artifact, outside the "
                "headline band.",
                transform=ax.transAxes, va="bottom", ha="left", fontsize=8, color=MUTE)
    _finish(fig, out, "Total variance vs log(K/F) — calendar-arb proof")


# ── figure 5 : total variance with error bars, per near expiry ───────────────
def fig5_tv_errorbars(slices, tv, smiles, out: Path):
    near = _window(slices).head(6)  # the 6 nearest within the VolaDynamics window
    n = len(near)
    ncol = 3
    nrow = int(np.ceil(n / ncol)) if n else 1
    fig, axes = plt.subplots(nrow, ncol, figsize=(12.5, 3.4 * nrow),
                             facecolor=PAPER, squeeze=False)
    for ax in axes.flat:
        ax.set_visible(False)
    for idx, (_, row) in enumerate(near.iterrows()):
        ax = axes.flat[idx]
        ax.set_visible(True)
        _style_axis(ax)
        block = tv[tv["expiry_date"] == row["expiry_date"]].sort_values("z")
        block = block[np.isfinite(block["w"])]
        block = _clip_to_quotes(block, _quoted_k_range(smiles, row["expiry_date"]))
        if len(block) >= 3:
            ax.plot(block["k"], block["w"], color=ACCENT, lw=1.8, zorder=4,
                    label="fit")
        mkt = smiles[smiles["expiry_date"] == row["expiry_date"]].copy()
        mkt = mkt[np.isfinite(mkt["mkt_iv"]) & (mkt["in_fit"] == 1)]  # listed/fitted strikes
        if not mkt.empty:
            T = float(row["T"])
            w_mkt = T * mkt["mkt_iv"] ** 2
            # dw/dσ = 2σT  ⇒  w error ≈ 2·σ·T·iv_err
            w_err = 2.0 * mkt["mkt_iv"] * T * mkt["iv_err"].fillna(0.0)
            ax.errorbar(mkt["k"], w_mkt, yerr=w_err, fmt="o", ms=3.4, mfc=INK,
                        mec=INK, ecolor=MUTE, elinewidth=0.8, capsize=1.6,
                        alpha=0.9, zorder=5, label="market ±(2σT·iv_err)")
        ax.set_title(f"{int(row['dte'])} DTE   (T={row['T']:.3f})", fontsize=9.5,
                     fontweight="bold", color=INK, loc="left", pad=4)
        ax.tick_params(labelsize=7)
        ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:.4f}"))
        if idx == 0:
            ax.legend(loc="upper center", frameon=False, fontsize=7.5)
        if idx // ncol == nrow - 1:
            ax.set_xlabel("k = log(K/F)", fontsize=8, color=INK)
        if idx % ncol == 0:
            ax.set_ylabel("total variance w", fontsize=8, color=INK)
    _finish(fig, out, "Total variance with market error bars vs fit (near expiries)")


# ── figure 6 : 3-param term structure ────────────────────────────────────────
def fig6_term_3param(slices, out: Path):
    fig, axes = plt.subplots(3, 1, figsize=(10.0, 8.4), facecolor=PAPER, sharex=True)
    T = slices["T"].values
    specs = [
        ("sigma0", "σ₀(T)  —  ATF vol", ACCENT, True),
        ("s2", "s2(T)  —  ATF skew  f'(0)", "#7c3aed", False),
        ("c2_eff", "c2_eff(T)  —  ATF curvature  f''(0)", NEG, False),
    ]
    for ax, (col, title, c, is_pct) in zip(axes, specs):
        _style_axis(ax)
        y = slices[col].values
        m = np.isfinite(y)
        ax.plot(T[m], y[m], color=c, lw=1.8, zorder=4)
        ax.scatter(T[m], y[m], s=16, color=c, zorder=5)
        if is_pct:
            _pct_axis(ax)
        ax.set_title(title, fontsize=10.5, fontweight="bold", color=INK, loc="left", pad=5)
        ax.tick_params(labelsize=8)
    # c2_eff panel: zero line + front annotation + "flat after 3-4 mo" marker
    axc = axes[2]
    axc.axhline(0.0, color=MUTE, lw=0.9, ls=(0, (4, 3)), alpha=0.7)
    front = slices.iloc[0]
    axc.annotate(f"front  c2_eff = {_fmt_c2(front['c2_eff'])}\n(≈ VolaDynamics −1.1)",
                 (front["T"], front["c2_eff"]), textcoords="offset points",
                 xytext=(30, 6), fontsize=9.5, color=NEG, fontweight="bold",
                 arrowprops=dict(arrowstyle="->", color=NEG, lw=1.2))
    axc.axvspan(0.25, 0.34, color=POS, alpha=0.08)
    axc.text(0.295, axc.get_ylim()[1], "flat after ~3–4 mo", rotation=90,
             va="top", ha="left", fontsize=8, color=POS)
    axes[2].set_xlabel("T  (years to expiry)", fontsize=9, color=INK)
    _finish(fig, out, "3-parameter term structure:  σ₀(T), s2(T), c2_eff(T)")


# ── figure 7 : 8-param (C8) term structure ───────────────────────────────────
def fig7_term_8param(slices, out: Path):
    params = [
        ("beta0", "β₀  level (σ₀)"),
        ("beta1", "β₁  skew (s2)"),
        ("beta2", "β₂  base curv (c2_base)"),
        ("beta3", "β₃  left wing (C_left)"),
        ("beta4", "β₄  right wing (C_right)"),
        ("beta5", "β₅  mode 1 (event)"),
        ("beta6", "β₆  mode 2 (width)"),
        ("beta7", "β₇  mode 3 (curv radius)"),
    ]
    params = [(c, lbl) for c, lbl in params if c in slices.columns]
    fig, axes = plt.subplots(2, 4, figsize=(13.5, 6.6), facecolor=PAPER, sharex=True)
    T = slices["T"].values
    norm = _color_norm(slices)
    for ax, (col, lbl) in zip(axes.flat, params):
        _style_axis(ax)
        y = slices[col].values
        m = np.isfinite(y)
        ax.plot(T[m], y[m], color=INK, lw=1.4, zorder=3, alpha=0.8)
        ax.scatter(T[m], y[m], s=18, c=[CMAP(norm(d)) for d in slices["dte"].values[m]],
                   zorder=4)
        ax.set_title(lbl, fontsize=9.5, fontweight="bold", color=INK, loc="left", pad=4)
        ax.tick_params(labelsize=7.5)
    for ax in axes.flat[len(params):]:
        ax.set_visible(False)
    for ax in axes[-1]:
        if ax.get_visible():
            ax.set_xlabel("T (yr)", fontsize=8, color=INK)
    _finish(fig, out, "8-parameter (C8) term structure — base 5 + 3 modes vs T")


# ── figure 8 : multi-expiry market-vs-fit panels ─────────────────────────────
def fig8_multi_panels(slices, tv, smiles, out: Path):
    win = _window(slices).reset_index(drop=True)  # near-term set (dte ≤ 120)
    n = len(win)
    # representative spread across the near-term surface (schema: i=0,1,3,4,5,6,9-style)
    want = [0, 1, 3, 4, 5, 6, 9, n - 1]
    idxs = sorted({i for i in want if 0 <= i < n})
    ncol = 4
    nrow = int(np.ceil(len(idxs) / ncol)) if idxs else 1
    fig, axes = plt.subplots(nrow, ncol, figsize=(14.0, 3.5 * nrow),
                             facecolor=PAPER, squeeze=False)
    for ax in axes.flat:
        ax.set_visible(False)
    norm = _color_norm(slices)
    for pos, i in enumerate(idxs):
        row = win.iloc[i]
        ax = axes.flat[pos]
        ax.set_visible(True)
        _style_axis(ax)
        block = tv[tv["expiry_date"] == row["expiry_date"]].sort_values("z")
        block = block[np.isfinite(block["fit_iv"])]
        block = _clip_to_quotes(block, _quoted_k_range(smiles, row["expiry_date"]))
        if len(block) >= 3:
            ax.plot(block["z"], block["fit_iv"], color=CMAP(norm(row["dte"])),
                    lw=2.0, zorder=4)
        mkt = smiles[smiles["expiry_date"] == row["expiry_date"]].copy()
        mkt = mkt[np.isfinite(mkt["mkt_iv"]) & (mkt["in_fit"] == 1)]  # listed/fitted strikes
        if not mkt.empty:
            ax.errorbar(mkt["z"], mkt["mkt_iv"], yerr=mkt["iv_err"].fillna(0.0),
                        fmt="o", ms=3.0, mfc=INK, mec=INK, ecolor=MUTE,
                        elinewidth=0.7, capsize=1.4, alpha=0.85, zorder=5)
        _pct_axis(ax)
        rev = " · reverted" if bool(row.get("reverted", 0)) else ""
        ax.set_title(f"{int(row['dte'])} DTE   c2_eff={_fmt_c2(row['c2_eff'])}{rev}",
                     fontsize=9.0, fontweight="bold", color=INK, loc="left", pad=4)
        ax.tick_params(labelsize=7.5)
        if pos // ncol == nrow - 1:
            ax.set_xlabel("z", fontsize=8, color=INK)
        if pos % ncol == 0:
            ax.set_ylabel("IV", fontsize=8, color=INK)
    _finish(fig, out, "Per-expiry market-vs-fit panels (NS space)")


# ── figure 9 : earnings decomposition (optional) ─────────────────────────────
def fig9_earnings(slices, earn, out: Path):
    ten = earn["tenors"]
    summ = earn["summary"]
    fig, (axl, axr) = plt.subplots(1, 2, figsize=(13.0, 5.6), facecolor=PAPER)

    # left: dirty vs censored ATM term curve + fitted censored curve
    _style_axis(axl)
    T = ten["T"].values
    axl.plot(T, ten["atm_dirty"].values, "o-", color=NEG, lw=1.8, ms=5,
             label="ATM (dirty — with events)", zorder=5)
    axl.plot(T, ten["atm_cen"].values, "s-", color=ACCENT, lw=1.8, ms=4.5,
             label="ATM (earnings-censored)", zorder=4)
    lt, st, decay = float(summ.get("lt", np.nan)), float(summ.get("st", np.nan)), float(summ.get("decay", np.nan))
    if np.isfinite([lt, st, decay]).all():
        tt = np.linspace(max(T.min(), 1e-4), T.max(), 200)
        axl.plot(tt, lt + (st - lt) * np.exp(-decay * tt), color=ACCENT, lw=1.0,
                 ls=(0, (4, 3)), alpha=0.8, label="σ_C(T)=lt+(st−lt)e^{−decay·T}")
    _pct_axis(axl)
    axl.set_xlabel("T (years to expiry)", fontsize=9, color=INK)
    axl.set_ylabel("ATM implied volatility", fontsize=9, color=INK)
    axl.set_title("Dirty vs earnings-censored ATM term curve", fontsize=10.5,
                  fontweight="bold", color=INK, loc="left", pad=5)
    iemove = float(summ.get("iEMove", np.nan))
    axl.text(0.97, 0.95, f"iEMove = {iemove*100:.1f}% of spot\n(implied per-event move)",
             transform=axl.transAxes, va="top", ha="right", family="monospace",
             fontsize=9.5, color=INK,
             bbox=dict(boxstyle="round,pad=0.5", fc="#f7f7f5", ec=GRID, lw=1.0))
    axl.legend(loc="upper center", frameon=False, fontsize=8)

    # right: event-variance share vs T
    _style_axis(axr)
    axr.plot(T, ten["event_var_share"].values, "o-", color="#7c3aed", lw=1.8, ms=5,
             zorder=5)
    axr.fill_between(T, ten["event_var_share"].values, 0.0, color="#7c3aed", alpha=0.10)
    axr.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v*100:.0f}%"))
    axr.set_ylim(0, 1)
    axr.set_xlabel("T (years to expiry)", fontsize=9, color=INK)
    axr.set_ylabel("event-variance share   n·eMove² / w_atm", fontsize=9, color=INK)
    axr.set_title("Share of ATM variance from scheduled events", fontsize=10.5,
                  fontweight="bold", color=INK, loc="left", pad=5)
    axr.text(0.5, 0.5, "the front expiry's variance is almost entirely the\n"
             "earnings event; it decays as diffusion accumulates.",
             transform=axr.transAxes, va="center", ha="center", fontsize=8.5, color=MUTE)
    _finish(fig, out, "Earnings decomposition — dirty vs censored ATM, iEMove, event share")


# ── index.html ───────────────────────────────────────────────────────────────
def write_index(outdir: Path, meta: dict, slices: pd.DataFrame, produced, crossings_note):
    front = slices.iloc[0] if len(slices) else None
    cards = []
    for fname, caption in produced:
        cards.append(
            f'<figure class="card">\n'
            f'  <a href="{html.escape(fname)}"><img src="{html.escape(fname)}" '
            f'alt="{html.escape(caption)}"></a>\n'
            f'  <figcaption>{html.escape(caption)}</figcaption>\n'
            f"</figure>"
        )
    n_exp = int(meta.get("n_expiries", len(slices)))
    spot = meta.get("spot", "?")
    fit_ms = meta.get("fit_ms", "?")
    curve = meta.get("curve", "?")
    front_c2 = _fmt_c2(front["c2_eff"]) if front is not None else "n/a"
    front_dte = int(front["dte"]) if front is not None else 0
    synth = " · <strong>SYNTHETIC harness data</strong>" if meta.get("synthetic") else ""
    facts = (
        f"underlying <strong>{html.escape(str(meta.get('underlying','AMZN')))}</strong> · "
        f"spot ≈ <strong>${spot}</strong> · {n_exp} expiries · curve {html.escape(str(curve))} · "
        f"fit {fit_ms} ms · front {front_dte} DTE c2_eff <strong>{front_c2}</strong>{synth}"
    )
    style = f"""<style>
:root {{ color-scheme: light dark; }}
* {{ box-sizing: border-box; }}
body {{ font-family: -apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
  margin: 0 auto; padding: 2rem; max-width: 1180px; background: {PAPER}; color: {INK}; }}
h1 {{ font-size: 1.55rem; margin: 0 0 .2rem; }}
.band {{ color: {MUTE}; font-size: .95rem; margin: 0 0 .4rem; }}
.facts {{ color: {INK}; font-size: .82rem; margin: 0 0 1rem;
  padding: .55rem .8rem; background: #f7f7f5; border: 1px solid {GRID}; border-radius: 8px; }}
.arb {{ font-size: .82rem; margin: 0 0 1.4rem; padding: .5rem .8rem;
  border-radius: 8px; border: 1px solid {GRID}; }}
.grid {{ display: grid; grid-template-columns: repeat(auto-fit,minmax(460px,1fr)); gap: 1.4rem; }}
figure.card {{ margin: 0; border: 1px solid {GRID}; border-radius: 10px; overflow: hidden;
  background: {PAPER}; }}
figure.card img {{ width: 100%; height: auto; display: block; }}
figcaption {{ font-size: .82rem; color: {MUTE}; padding: .55rem .75rem; }}
footer {{ color: {MUTE}; font-size: .78rem; margin-top: 2rem; }}
</style>"""
    arb_cls_col = POS if "free" in crossings_note else NEG
    body = "\n".join([
        f"<h1>{html.escape(meta.get('underlying','AMZN'))} around earnings — VolaDynamics figure set</h1>",
        f'<p class="band">{html.escape(TITLE_BAND)}</p>',
        f'<p class="facts">{facts}</p>',
        f'<p class="arb" style="color:{arb_cls_col}">{html.escape(crossings_note)}</p>',
        '<div class="grid">',
        *cards,
        "</div>",
        "<footer>atx-vol · amzn_earnings_report.py · matplotlib-only, self-contained</footer>",
    ])
    doc = ("<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
           f"<title>{html.escape(meta.get('underlying','AMZN'))} around earnings — report</title>\n"
           f"{style}\n</head>\n<body>\n{body}\n</body>\n</html>\n")
    (outdir / "index.html").write_text(doc, encoding="utf-8")


# ── driver ───────────────────────────────────────────────────────────────────
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Render the AMZN-earnings VolaDynamics figure set.")
    ap.add_argument("--in", dest="indir", required=True, help="input CSV/JSON directory")
    ap.add_argument("--out", dest="outdir", required=True, help="output PNG/HTML directory")
    args = ap.parse_args(argv)

    indir = Path(args.indir)
    outdir = Path(args.outdir)
    if not indir.is_dir():
        print(f"error: input dir {indir} not found", file=sys.stderr)
        return 1
    for req in ("slices.csv", "total_variance.csv", "smiles.csv"):
        if not (indir / req).exists():
            print(f"error: missing required input {indir / req}", file=sys.stderr)
            return 1
    outdir.mkdir(parents=True, exist_ok=True)

    meta, slices, tv, smiles, earn = load_inputs(indir)
    if slices.empty:
        print("error: slices.csv is empty", file=sys.stderr)
        return 1

    # calendar-arb verdict for the index header — same windowed + quote-clipped
    # near-money scan that drives fig 4, so the two agree.
    n_near, n_wing, win = _calendar_scan(slices, tv, smiles)
    crossings_note = (
        "Calendar-arbitrage-free: total-variance curves ordered by T, non-crossing "
        "over listed strikes (|k| ≤ 0.3, near-term window through 2018-08-17)."
        if n_near == 0 else
        f"WARNING: {n_near} near-money calendar-arb crossing(s) on |k| ≤ 0.3.")
    win_dtes = ", ".join(str(int(d)) for d in win["dte"].values)
    print(f"[report] calendar scan: near-money(|k|<=0.3) crossings={n_near}, "
          f"deep-wing(|k|>0.3) crossings={n_wing}")
    print(f"[report] near-term window (dte <= {VOLA_WINDOW_DTE}): {len(win)} expiries "
          f"[dte {win_dtes}]")

    fig1_ns_surface(slices, tv, smiles, outdir / FIGURES[0][0])
    fig2_front_strike(slices, tv, smiles, outdir / FIGURES[1][0])
    fig3_front_ns(slices, tv, smiles, outdir / FIGURES[2][0])
    fig4_total_variance(slices, tv, smiles, outdir / FIGURES[3][0])
    fig5_tv_errorbars(slices, tv, smiles, outdir / FIGURES[4][0])
    fig6_term_3param(slices, outdir / FIGURES[5][0])
    fig7_term_8param(slices, outdir / FIGURES[6][0])
    fig8_multi_panels(slices, tv, smiles, outdir / FIGURES[7][0])

    produced = list(FIGURES[:8])
    if earn is not None:
        fig9_earnings(slices, earn, outdir / FIGURES[8][0])
        produced.append(FIGURES[8])
    else:
        print("[report] earnings files absent - skipping figure 9")

    write_index(outdir, meta, slices, produced, crossings_note)
    print(f"[report] wrote {len(produced)} figures + index.html to {outdir}")
    for fname, _ in produced:
        print(f"   {fname}")
    print("   index.html")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
