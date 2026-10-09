#!/usr/bin/env python3
"""PnL-track PNG for a spy_dispersion_pnl run (WS-D D5 acceptance renderer).

Reads the single self-describing TSV emitted by
examples/spy_dispersion_pnl.cpp (`atx::vol::write_backtest_pnl_tsv`): a
`# key=value` metadata header followed by the per-step PnL + greeks time
series, tab-separated. Renders ONE 150-dpi PNG: the cumulative P&L track
headline (with drawdown shading + a stats box), a daily-P&L / cumulative
attribution row, and a row of book-greek panels (gross vega / theta / gamma
and the post-hedge net delta). Title carries the run metadata (strategy,
names vs index, window, held-to-expiry / hedge config, wall-clock, headline
stats).

    python spy_dispersion_pnl_report.py <run.tsv> [out.png]

Default output: `<run-stem>_pnl_track.png` alongside the TSV. Reuses the
tools/tearsheet.py idiom (palette, axis styling, stats box). Pure matplotlib
(Agg backend) + pandas + stdlib. No new dependencies, no network, no external
assets.
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

# ── palette (matches tools/tearsheet.py) ────────────────────────────────────
INK = "#1b1b2f"
MUTE = "#6b7280"
GRID = "#e6e6ea"
PAPER = "#ffffff"
ACCENT = "#0f766e"   # teal — the NAV track
POS = "#15803d"      # green — gains
NEG = "#b4232a"      # red — losses
GREEK_C = {"vega": "#7c3aed", "theta": "#0f766e", "gamma": "#c2410c", "delta": "#2563eb"}


def read_run(path: Path):
    """Return (meta: dict, df: DataFrame). Metadata is the `# k=v` header block;
    the body is a tab-separated series with a `date` column."""
    meta: dict[str, str] = {}
    with path.open("r", encoding="utf-8") as fh:
        for line in fh:
            if not line.startswith("#"):
                break
            k, _, v = line[1:].strip().partition("=")
            if v:
                meta[k.strip()] = v.strip()
    df = pd.read_csv(path, sep="\t", comment="#", parse_dates=["date"])
    return meta, df


def _f(meta: dict, key: str, default: float = 0.0) -> float:
    try:
        return float(meta.get(key, default))
    except (TypeError, ValueError):
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


def _money(ax) -> None:
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"${v:,.0f}"))


def _date_ticks(ax, span_days: float) -> None:
    # Month ticks for a multi-month run; day-count ticks for a short fixture run.
    if span_days >= 60.0:
        ax.xaxis.set_major_locator(mdates.MonthLocator())
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%b"))
    else:
        ax.xaxis.set_major_locator(mdates.AutoDateLocator())
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%m-%d"))


def build(meta: dict, df: pd.DataFrame, out: Path) -> None:
    d = df["date"]
    nav = df["nav"]
    run_max = nav.cummax()
    span_days = float((d.iloc[-1] - d.iloc[0]).days) if len(d) > 1 else 0.0

    fig = plt.figure(figsize=(13.0, 9.6), dpi=150, facecolor=PAPER)
    gs = fig.add_gridspec(
        3, 4, height_ratios=[2.7, 1.15, 1.0], hspace=0.42, wspace=0.28,
        left=0.068, right=0.965, top=0.85, bottom=0.08,
    )

    # ── headline: cumulative P&L track ──────────────────────────────────────
    ax = fig.add_subplot(gs[0, :])
    _style_axis(ax)
    ax.plot(d, nav, color=ACCENT, linewidth=2.0, zorder=5)
    ax.fill_between(d, nav, 0.0, where=(nav >= 0), color=ACCENT, alpha=0.10, zorder=1)
    ax.fill_between(d, nav, 0.0, where=(nav < 0), color=NEG, alpha=0.09, zorder=1)
    ax.fill_between(d, nav, run_max, color=NEG, alpha=0.12, zorder=2, linewidth=0)
    ax.axhline(0.0, color=MUTE, linewidth=0.9, linestyle=(0, (4, 3)), alpha=0.7)
    ax.scatter([d.iloc[-1]], [nav.iloc[-1]], s=34, color=ACCENT, zorder=6)
    ax.annotate(
        f"${nav.iloc[-1]:,.0f}", (d.iloc[-1], nav.iloc[-1]),
        textcoords="offset points", xytext=(-6, 8), ha="right",
        fontsize=10, fontweight="bold", color=ACCENT,
    )
    _money(ax)
    _date_ticks(ax, span_days)
    ax.set_title("Cumulative P&L (vega-flat dispersion, held to expiry)",
                 fontsize=11, fontweight="bold", color=INK, loc="left", pad=8)

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
    _date_ticks(axd, span_days)
    axd.set_title("Daily P&L", fontsize=10, fontweight="bold", color=INK, loc="left", pad=6)

    # ── cumulative attribution ──────────────────────────────────────────────
    axa = fig.add_subplot(gs[1, 2:])
    _style_axis(axa)
    for key, col in (("pnl_theta", GREEK_C["theta"]), ("pnl_vega", GREEK_C["vega"]),
                     ("pnl_gamma", GREEK_C["gamma"]), ("pnl_unexplained", MUTE)):
        if key in df.columns:
            axa.plot(d, df[key].cumsum(), color=col, linewidth=1.6,
                     label=key.replace("pnl_", ""))
    axa.axhline(0.0, color=MUTE, linewidth=0.8, alpha=0.6)
    _money(axa)
    _date_ticks(axa, span_days)
    axa.legend(loc="upper left", frameon=False, fontsize=8, ncol=2, handlelength=1.3)
    axa.set_title("Cumulative attribution", fontsize=10, fontweight="bold", color=INK,
                  loc="left", pad=6)

    # ── book-greek panels ───────────────────────────────────────────────────
    panels = [
        ("gross_vega", "Gross Vega", GREEK_C["vega"], "$"),
        ("gross_theta", "Gross Theta", GREEK_C["theta"], "$"),
        ("gross_gamma", "Gross Gamma", GREEK_C["gamma"], ""),
        ("gross_delta", "Net Delta (post-hedge)", GREEK_C["delta"], ""),
    ]
    for i, (col, label, c, unit) in enumerate(panels):
        axg = fig.add_subplot(gs[2, i])
        _style_axis(axg)
        if col in df.columns:
            axg.plot(d, df[col], color=c, linewidth=1.5, zorder=4)
            axg.fill_between(d, df[col], df[col].iloc[0], color=c, alpha=0.10)
        if unit == "$":
            _money(axg)
        else:
            axg.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:,.2f}"))
        _date_ticks(axg, span_days)
        axg.set_title(label, fontsize=9.5, fontweight="bold", color=INK, loc="left", pad=5)

    # ── titles / metadata ───────────────────────────────────────────────────
    strat = meta.get("strategy", "spy_dispersion_vega_flat")
    names = meta.get("names", "?")
    index_symbol = meta.get("index_symbol", "SPY")
    fig.suptitle(
        f"{index_symbol} dispersion  ·  {strat}",
        x=0.068, y=0.965, ha="left", fontsize=18, fontweight="bold", color=INK,
    )
    n_names = meta.get("n_names", str(len(names.split(",")) if names != "?" else "?"))
    line1 = (
        f"{meta.get('window_start','?')} → {meta.get('window_end','?')}   |   "
        f"{meta.get('n_steps','?')} steps   |   "
        f"{n_names} names vs {index_symbol}   |   "
        f"tenor {_f(meta,'tenor_days'):.0f}d, {meta.get('delta_target','?')}Δ, "
        f"theta ${_f(meta,'theta_per_name_daily'):.0f}/name/day"
    )
    line2 = (
        f"held to expiry · hedge {meta.get('hedge','?')} · frictions {meta.get('frictions','?')} · "
        f"missing {meta.get('missing_policy','?')} (min {meta.get('min_names','?')})   |   "
        f"wall-clock {_f(meta,'wall_clock_ms'):.0f} ms ({_f(meta,'steps_per_s'):.0f} steps/s)"
    )
    fig.text(0.068, 0.912, line1, ha="left", fontsize=10.5, color=MUTE)
    fig.text(0.068, 0.884, line2, ha="left", fontsize=10.5, color=MUTE)
    src = meta.get("data_source", "surface_db")
    dropped = meta.get("dropped_alphabet_class", "none")
    tag = f"atx-vol backtest engine · {src}"
    if dropped not in ("none", ""):
        tag += f" · Alphabet dedup: dropped {dropped}"
    fig.text(0.965, 0.018, tag, ha="right", fontsize=8.5, color=MUTE, style="italic")

    # Calendar-gap annotation (I1): make silent holes / a narrowed window loud on
    # the artifact itself so the PNG can be audited against the request.
    try:
        missing = int(float(meta.get("missing_sessions", "0") or 0))
    except (TypeError, ValueError):
        missing = 0
    narrowed = meta.get("window_narrowed", "no") == "yes"
    if missing > 0 or narrowed:
        parts = []
        if missing > 0:
            parts.append(f"{missing} expected session(s) MISSING from the run")
        if narrowed:
            parts.append(
                f"window narrowed to {meta.get('window_start','?')}→{meta.get('window_end','?')} "
                f"(requested {meta.get('requested_start','?')}→{meta.get('requested_end','?')})"
            )
        fig.text(0.068, 0.018, "[!] CALENDAR GAP: " + "; ".join(parts),
                 ha="left", fontsize=9, fontweight="bold", color=NEG)

    fig.savefig(out, dpi=150, facecolor=PAPER)
    plt.close(fig)
    print(f"[spy_dispersion_pnl_report] wrote {out}")


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if not argv:
        print(__doc__)
        return 2
    tsv = Path(argv[0])
    if not tsv.exists():
        print(f"error: {tsv} not found", file=sys.stderr)
        return 1
    out = Path(argv[1]) if len(argv) > 1 else tsv.with_name(tsv.stem + "_pnl_track.png")
    meta, df = read_run(tsv)
    if df.empty:
        print(f"error: {tsv} has no data rows", file=sys.stderr)
        return 1
    build(meta, df, out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
