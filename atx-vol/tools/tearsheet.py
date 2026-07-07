#!/usr/bin/env python3
"""Professional backtest tearsheet from a spy_strangle_backtest CSV.

Reads the CSV emitted by examples/spy_strangle_backtest.cpp (a `# key=value`
metadata header, then a pnl + greeks time series) and renders a single-page
tearsheet: the cumulative P&L track headline, a daily-P&L / attribution row, and a
row of book-greek panels. Title carries the run metadata (symbol, strategy, window,
wall-clock, headline stats).

    python tearsheet.py <run.csv> [out.png]

Pure matplotlib + pandas (no seaborn / external style). Saves a 150-dpi PNG.
"""

from __future__ import annotations

import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # headless
import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import pandas as pd

# ── palette ─────────────────────────────────────────────────────────────────
INK = "#1b1b2f"
MUTE = "#6b7280"
GRID = "#e6e6ea"
PAPER = "#ffffff"
ACCENT = "#0f766e"   # teal — the NAV track
POS = "#15803d"      # green — gains
NEG = "#b4232a"      # red — losses
GREEK_C = {"vega": "#7c3aed", "theta": "#0f766e", "gamma": "#c2410c", "delta": "#2563eb"}


def read_run(path: Path):
    """Return (meta: dict, df: DataFrame). Metadata is the `# k=v` header block."""
    meta: dict[str, str] = {}
    with path.open("r", encoding="utf-8") as fh:
        for line in fh:
            if not line.startswith("#"):
                break
            k, _, v = line[1:].strip().partition("=")
            if v:
                meta[k.strip()] = v.strip()
    df = pd.read_csv(path, comment="#", parse_dates=["date"])
    return meta, df


def _f(meta: dict, key: str, default: float = 0.0) -> float:
    try:
        return float(meta.get(key, default))
    except ValueError:
        return default


def _style_axis(ax) -> None:
    ax.set_facecolor(PAPER)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=MUTE, labelsize=8, length=0)
    ax.grid(True, color=GRID, linewidth=0.7, alpha=0.9)
    ax.set_axisbelow(True)


def _money(ax, axis: str = "y") -> None:
    fmt = mticker.FuncFormatter(lambda v, _: f"${v:,.0f}")
    (ax.yaxis if axis == "y" else ax.xaxis).set_major_formatter(fmt)


def _month_ticks(ax) -> None:
    ax.xaxis.set_major_locator(mdates.MonthLocator())
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%b"))


def build(meta: dict, df: pd.DataFrame, out: Path) -> None:
    d = df["date"]
    nav = df["nav"]
    run_max = nav.cummax()

    fig = plt.figure(figsize=(13.0, 9.6), dpi=150, facecolor=PAPER)
    gs = fig.add_gridspec(
        3, 4, height_ratios=[2.7, 1.15, 1.0], hspace=0.42, wspace=0.28,
        left=0.068, right=0.965, top=0.85, bottom=0.07,
    )

    # ── headline: cumulative P&L track ──────────────────────────────────────
    ax = fig.add_subplot(gs[0, :])
    _style_axis(ax)
    ax.plot(d, nav, color=ACCENT, linewidth=2.0, zorder=5)
    ax.fill_between(d, nav, 0.0, where=(nav >= 0), color=ACCENT, alpha=0.10, zorder=1)
    ax.fill_between(d, nav, 0.0, where=(nav < 0), color=NEG, alpha=0.09, zorder=1)
    # drawdown shading (peak-to-current)
    ax.fill_between(d, nav, run_max, color=NEG, alpha=0.12, zorder=2, linewidth=0)
    ax.axhline(0.0, color=MUTE, linewidth=0.9, linestyle=(0, (4, 3)), alpha=0.7)
    # final value marker + label
    ax.scatter([d.iloc[-1]], [nav.iloc[-1]], s=34, color=ACCENT, zorder=6)
    ax.annotate(
        f"${nav.iloc[-1]:,.0f}",
        (d.iloc[-1], nav.iloc[-1]),
        textcoords="offset points", xytext=(-6, 8), ha="right",
        fontsize=10, fontweight="bold", color=ACCENT,
    )
    _money(ax)
    _month_ticks(ax)
    ax.set_title("Cumulative P&L", fontsize=11, fontweight="bold", color=INK, loc="left", pad=8)

    # stats box (top-left inside the track)
    stats = [
        ("Total return", f"${_f(meta,'total_return'):,.0f}"),
        ("Sharpe", f"{_f(meta,'sharpe'):.2f}"),
        ("Ann. vol", f"${_f(meta,'ann_vol'):,.0f}"),
        ("Max drawdown", f"${_f(meta,'max_drawdown'):,.0f}"),
        ("Hit rate", f"{_f(meta,'hit_rate')*100:.1f}%"),
        ("Avg gross vega", f"${_f(meta,'avg_gross_vega'):,.0f}"),
    ]
    txt = "\n".join(f"{k:<15}{v:>12}" for k, v in stats)
    ax.text(
        0.012, 0.97, txt, transform=ax.transAxes, va="top", ha="left",
        family="monospace", fontsize=9, color=INK,
        bbox=dict(boxstyle="round,pad=0.6", fc="#f7f7f5", ec=GRID, lw=1.0),
    )

    # ── daily P&L bars ──────────────────────────────────────────────────────
    axd = fig.add_subplot(gs[1, :2])
    _style_axis(axd)
    colors = [POS if v >= 0 else NEG for v in df["pnl_total"]]
    axd.bar(d, df["pnl_total"], color=colors, width=1.0, linewidth=0)
    axd.axhline(0.0, color=MUTE, linewidth=0.8, alpha=0.6)
    _money(axd)
    _month_ticks(axd)
    axd.set_title("Daily P&L", fontsize=10, fontweight="bold", color=INK, loc="left", pad=6)

    # ── cumulative attribution ──────────────────────────────────────────────
    axa = fig.add_subplot(gs[1, 2:])
    _style_axis(axa)
    for key, col in (("pnl_theta", GREEK_C["theta"]), ("pnl_vega", GREEK_C["vega"]),
                     ("pnl_gamma", GREEK_C["gamma"]), ("pnl_unexplained", MUTE)):
        axa.plot(d, df[key].cumsum(), color=col, linewidth=1.6,
                 label=key.replace("pnl_", ""))
    axa.axhline(0.0, color=MUTE, linewidth=0.8, alpha=0.6)
    _money(axa)
    _month_ticks(axa)
    axa.legend(loc="upper left", frameon=False, fontsize=8, ncol=2, handlelength=1.3)
    axa.set_title("Cumulative attribution", fontsize=10, fontweight="bold", color=INK,
                  loc="left", pad=6)

    # ── book-greek panels ───────────────────────────────────────────────────
    panels = [
        ("gross_vega", "Gross Vega", GREEK_C["vega"], "$"),
        ("gross_theta", "Gross Theta", GREEK_C["theta"], "$"),
        ("gross_gamma", "Gross Gamma", GREEK_C["gamma"], ""),
        ("gross_delta", "Gross Delta", GREEK_C["delta"], ""),
    ]
    for i, (col, label, c, unit) in enumerate(panels):
        axg = fig.add_subplot(gs[2, i])
        _style_axis(axg)
        axg.plot(d, df[col], color=c, linewidth=1.5)
        axg.fill_between(d, df[col], df[col].iloc[0], color=c, alpha=0.10)
        if unit == "$":
            _money(axg)
        else:
            axg.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:,.2f}"))
        axg.xaxis.set_major_locator(mdates.MonthLocator(interval=2))
        axg.xaxis.set_major_formatter(mdates.DateFormatter("%b"))
        axg.set_title(label, fontsize=9.5, fontweight="bold", color=INK, loc="left", pad=5)

    # ── titles / metadata ───────────────────────────────────────────────────
    symbol = meta.get("symbol", "?")
    strat = meta.get("strategy", "")
    fig.suptitle(
        f"{symbol}  ·  {strat}",
        x=0.068, y=0.965, ha="left", fontsize=18, fontweight="bold", color=INK,
    )
    sub = (
        f"{meta.get('window_start','?')} → {meta.get('window_end','?')}   |   "
        f"{meta.get('business_days','?')} business days, {meta.get('priced_steps','?')} priced steps   |   "
        f"tenor {_f(meta,'tenor_years'):.2f}y, {meta.get('delta_target','?')}Δ, "
        f"{meta.get('n_strangles','1')}× (mult {meta.get('multiplier','100')})   |   "
        f"wall-clock {_f(meta,'wall_clock_ms'):.0f} ms  ({_f(meta,'steps_per_s'):.0f} steps/s)"
    )
    fig.text(0.068, 0.905, sub, ha="left", fontsize=10.5, color=MUTE)
    fig.text(0.965, 0.018, "atx-vol backtest engine · synthetic SPY corpus",
             ha="right", fontsize=8.5, color=MUTE, style="italic")

    fig.savefig(out, dpi=150, facecolor=PAPER)
    print(f"[tearsheet] wrote {out}")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    csv = Path(sys.argv[1])
    if not csv.exists():
        print(f"error: {csv} not found")
        return 1
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else csv.with_name(csv.stem + "_tearsheet.png")
    meta, df = read_run(csv)
    build(meta, df, out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
