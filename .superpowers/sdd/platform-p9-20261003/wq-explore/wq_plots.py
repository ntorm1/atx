"""WQ-explore plots (exploratory, TRAIN 2020-2023, in-sample). PNGs under docs/plans/2026-10-03-wq-explore-*.png."""
import csv, sys
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "atx-impl/tools"))
import nav_summ as NS
X = ROOT / "build-equity/wq-explore"
OUT = ROOT / "docs/plans"
SURF, INK, MUTE = "#fcfcfb", "#0b0b0b", "#52514e"
C = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]
SCEN = "modeled-1bn-stale5-v1+swap-fin-v1"
plt.rcParams.update({"font.size": 9, "axes.edgecolor": "#c9c8c2", "axes.labelcolor": MUTE, "xtick.color": MUTE,
                     "ytick.color": MUTE, "figure.facecolor": SURF, "axes.facecolor": SURF, "savefig.facecolor": SURF})


def style(ax):
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.grid(axis="y", color="#e6e5e0", lw=0.6)
    ax.set_axisbelow(True)


def nets(d):
    n = NS.net_series(NS.load_daily(d, SCEN))
    k = sorted(n)
    return np.array(k, dtype="int64").astype("datetime64[ns]"), np.array([n[i] for i in k])


def equity():
    runs = [("primary (Y-F0, 12 themes)", X / "primary-rel/nav"), ("+ WQ theme, slow subset (theme-erc, 13%)", X / "slow/nav-gm"),
            ("+ WQ slow at a quarter share (3%)", X / "slowmix25/nav-gm"), ("+ WQ top-10 by TRAIN Sharpe (in-sample pick)", X / "top10/nav-gm")]
    fig, ax = plt.subplots(2, 1, figsize=(9, 7.2), height_ratios=[3, 2], sharex=True)
    t0, r0 = nets(runs[0][1])
    for i, (lab, d) in enumerate(runs):
        t, r = nets(d)
        ax[0].plot(t, 100 * (np.cumprod(1 + r) - 1), color=C[i], lw=2 if i == 0 else 1.5, label=f"{lab}: net Sharpe {NS.sharpe(r):.2f}")
        if i:
            ax[1].plot(t, 100 * np.cumsum(r - r0), color=C[i], lw=1.5, label=lab)
    ax[0].set_title("Net cumulative return, primary cost scenario (modeled $1bn + swap financing), matched gross\n"
                    "TRAIN 2020-2023, in-sample, exploratory, off-ledger", loc="left", color=INK, fontsize=10)
    ax[0].set_ylabel("cumulative net return, %")
    ax[1].set_title("Cumulative daily net return minus the primary's (percentage points)", loc="left", color=INK, fontsize=10)
    ax[1].set_ylabel("pp")
    ax[1].axhline(0, color=MUTE, lw=0.8)
    for a in ax:
        style(a)
        a.legend(frameon=False, fontsize=8, labelcolor=INK, loc="upper left")
    ax[1].xaxis.set_major_formatter(mdates.DateFormatter("%Y-%m"))
    fig.tight_layout()
    fig.savefig(OUT / "2026-10-03-wq-explore-equity-curve.png", dpi=150)


def scatter():
    rows = list(csv.DictReader(open(X / "stats/wq_candidates.csv")))
    fig, ax = plt.subplots(figsize=(9, 5))
    for i, (form, lab) in enumerate((("raw", "as printed (101)"), ("house", "house form rank(decay_linear(x, 5 | 21)) (101)"))):
        R = [r for r in rows if r["form"] == form and r["sharpe"] not in ("", "nan") and float(r["coverage"]) >= 0.5]
        ax.scatter([float(r["tau"]) for r in R], [float(r["sharpe"]) for r in R], s=22, color=C[i], alpha=0.8,
                   edgecolor=SURF, linewidth=0.6, label=lab)
    ax.axvline(0.7, color=MUTE, lw=0.8, ls="--")
    ax.text(0.71, -0.75, "house turnover limit 0.70", color=MUTE, fontsize=8)
    ax.axhline(0, color=MUTE, lw=0.8)
    ax.set_xlabel("standalone daily turnover tau (admission definition)")
    ax.set_ylabel("annualised factor Sharpe, gross of costs")
    ax.set_title("The 101 formulaic alphas on TRAIN 2020-2023: standalone factor Sharpe against turnover\n"
                 "admission factor definition, prior sign as printed; in-sample, exploratory", loc="left", color=INK, fontsize=10)
    style(ax)
    ax.legend(frameon=False, fontsize=8, labelcolor=INK)
    fig.tight_layout()
    fig.savefig(OUT / "2026-10-03-wq-explore-sharpe-vs-turnover.png", dpi=150)


equity()
scatter()
