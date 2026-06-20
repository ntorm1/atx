#!/usr/bin/env python
"""Plot the equity curve of the capacity-aware low-vol deliverable alpha.

Reads the daily-PnL CSV dumped by single_alpha_capacity_test.cpp
(ATX_PNL_CSV=...) and writes a PNG with the compounded equity curve and the
underwater (drawdown) plot.

Usage: python plot_equity_curve.py <pnl.csv> <out.png>
"""
import sys
import numpy as np
import matplotlib

matplotlib.use("Agg")  # headless
import matplotlib.pyplot as plt

ALPHA = "group_neutralize(signedpower(zscore(-1*ts_std(returns,20)),2),sector)"


def main() -> int:
    csv = sys.argv[1] if len(sys.argv) > 1 else "deliverable_pnl.csv"
    png = sys.argv[2] if len(sys.argv) > 2 else "equity_curve.png"

    data = np.genfromtxt(csv, delimiter=",", names=True)
    day = data["day"].astype(int)
    pnl = data["daily_pnl"].astype(float)
    equity = data["equity"].astype(float)

    n = len(day)
    # annualized Sharpe (sqrt(252) * mean / pop std), matching the engine convention
    sharpe = np.sqrt(252.0) * pnl.mean() / pnl.std() if pnl.std() > 0 else float("nan")
    ann_ret = pnl.mean() * 252.0
    ann_vol = pnl.std() * np.sqrt(252.0)
    total = equity[-1] - 1.0
    peak = np.maximum.accumulate(equity)
    dd = equity / peak - 1.0
    max_dd = dd.min()

    fig, (ax1, ax2) = plt.subplots(
        2, 1, figsize=(11, 7), sharex=True, gridspec_kw={"height_ratios": [3, 1]}
    )

    ax1.plot(day, equity, color="#1565c0", lw=1.6)
    ax1.axhline(1.0, color="grey", lw=0.8, ls="--")
    ax1.set_ylabel("growth of $1 (compounded)")
    ax1.set_title(
        "Capacity-aware low-volatility alpha — equity curve\n"
        + ALPHA
        + f"\nSharpe {sharpe:.2f}  |  ann.ret {ann_ret*100:.1f}%  |  ann.vol {ann_vol*100:.1f}%  "
        + f"|  maxDD {max_dd*100:.1f}%  |  total {total*100:.1f}% over {n} trading days",
        fontsize=10,
    )
    ax1.grid(alpha=0.25)

    # mark the first/second-half split (robustness reference)
    mid = day[len(day) // 2]
    ax1.axvline(mid, color="#c62828", lw=0.8, ls=":")
    ax1.text(mid, ax1.get_ylim()[1], " H1 | H2", color="#c62828", va="top", fontsize=8)

    ax2.fill_between(day, dd * 100.0, 0.0, color="#c62828", alpha=0.4)
    ax2.set_ylabel("drawdown (%)")
    ax2.set_xlabel("trading day (capacity universe, ~1,118 names/day, $50M+ ADV)")
    ax2.grid(alpha=0.25)

    fig.tight_layout()
    fig.savefig(png, dpi=130)
    print(f"wrote {png}  (Sharpe {sharpe:.3f}, maxDD {max_dd*100:.1f}%, total {total*100:.1f}%)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
