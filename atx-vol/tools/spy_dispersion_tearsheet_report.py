#!/usr/bin/env python3
"""Tearsheet + benchmark-relative report for a surface-path dispersion run (WS-X-B / X5).

Reads the self-describing TSV emitted by `atx::vol::write_dispersion_tearsheet`
(`run_dir/surface_pnl_track.tsv`): a `# key=value` metadata header followed by the
per-step PnL + greeks series, tab-separated. Same file shape and the same
`read_run` idiom as `spy_dispersion_pnl_report.py`, which this extends — that
renderer draws the P&L TRACK; this one draws the TEARSHEET.

    python spy_dispersion_tearsheet_report.py <run_dir|track.tsv> [out.png]

Writes `<stem>_tearsheet.png` and, alongside it, a self-contained
`<stem>_tearsheet.html` with the figure base64-inlined (the
`build_standalone_report.py` idiom — one file, no external assets).

THE FRICTION REGIME IS A FIRST-CLASS DIMENSION, NOT A FOOTNOTE.
On the pinned 82-session run the same strategy over the same surfaces returns
+247.41 frictionless, +12.81 under retail frictions (cost 234.60), and -64.60
once square-root impact is added (cost 312.01) -- roughly 95% friction-dominated,
and the SIGN FLIPS under modest impact. A tearsheet that showed only the
frictionless number would be actively misleading, so:

  * the regime is a full-width colour-coded BANNER carrying its own text label
    (never colour alone) directly under the title, before any number;
  * every headline tile is captioned with the regime, so a cropped screenshot of
    a single tile still says which assumptions produced it;
  * a cost-decomposition panel shows gross -> cost -> net explicitly;
  * and the renderer HARD-REFUSES a track with no `friction_regime` key rather
    than silently drawing an unlabelled number.

Colour: single-hue accent for the (single-series) NAV track, a CVD-validated
diverging pair for signed quantities, and a validated 3-state status palette for
the regime badge. Pure matplotlib (Agg) + pandas + stdlib; no new dependencies.
"""

from __future__ import annotations

import base64
import html
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # headless
import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import pandas as pd

# ── palette ─────────────────────────────────────────────────────────────────
# Text/structure tokens are inherited verbatim from tools/spy_dispersion_pnl_report.py
# so the two artifacts read as one system.
INK = "#1b1b2f"
MUTE = "#6b7280"
GRID = "#e6e6ea"
PAPER = "#ffffff"
ACCENT = "#0f766e"  # teal — the single-series NAV track

# Diverging pair for signed quantities (gains/losses, attribution axes). Chosen
# over the conventional green/red, which is the classic red-green CVD trap:
# green/red scores deutan dE 3.1, this pair scores protan dE 24.1 / tritan 30.5,
# normal-vision 31.8 (validated, see the skill's validate_palette.js).
POS = "#1d6fbf"  # blue  — positive
NEG = "#d4600a"  # orange — negative

# Regime status palette. Validated as a 3-state set: CVD separation dE 12.5
# (protan) / 13.0 (tritan), normal-vision floor 21.2, all above the chroma floor
# and >= 3:1 against the surface. Each is ALWAYS rendered with its text label.
REGIME = {
    "frictionless": ("#0d9488", "FRICTIONLESS", "mid fills — an upper bound, not a tradeable result"),
    "frictioned": ("#d97706", "FRICTIONED", "spread + commission applied"),
    "frictioned+impact": ("#c2185b", "FRICTIONED + IMPACT", "spread + commission + square-root market impact"),
}


def read_run(path: Path):
    """Return (meta: dict, df: DataFrame) — the `# k=v` header then the series."""
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


def _has(meta: dict, key: str) -> bool:
    return key in meta and meta[key] != ""


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
    if span_days >= 60.0:
        ax.xaxis.set_major_locator(mdates.MonthLocator())
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%b"))
    else:
        ax.xaxis.set_major_locator(mdates.AutoDateLocator())
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%m-%d"))


def _tile(fig, x, y, w, h, label, value, note, value_color=INK):
    """One stat tile: a big number over a small label, with a regime/unit note."""
    ax = fig.add_axes([x, y, w, h])
    ax.set_axis_off()
    ax.add_patch(
        plt.Rectangle((0, 0), 1, 1, transform=ax.transAxes, facecolor="#f7f7f5",
                      edgecolor=GRID, linewidth=1.0, zorder=0)
    )
    ax.text(0.06, 0.72, label, transform=ax.transAxes, ha="left", va="center",
            fontsize=8.5, color=MUTE)
    ax.text(0.06, 0.42, value, transform=ax.transAxes, ha="left", va="center",
            fontsize=17, fontweight="bold", color=value_color)
    ax.text(0.06, 0.15, note, transform=ax.transAxes, ha="left", va="center",
            fontsize=7.2, color=MUTE, style="italic")
    return ax


def build(meta: dict, df: pd.DataFrame, out: Path) -> None:
    regime_key = meta.get("friction_regime", "")
    colour, badge, gloss = REGIME.get(regime_key, ("#6b7280", regime_key.upper(), ""))
    detail = meta.get("friction_detail", "")
    # Short regime tag repeated on every tile, so no number can be read without it.
    tag = f"regime: {regime_key}"

    d = df["date"]
    nav = df["nav"]
    run_max = nav.cummax()
    span_days = float((d.iloc[-1] - d.iloc[0]).days) if len(d) > 1 else 0.0

    net = _f(meta, "total_return")
    cost = _f(meta, "total_cost")
    gross = _f(meta, "gross_return", net + cost)
    financing = _f(meta, "total_financing")

    fig = plt.figure(figsize=(13.0, 11.4), dpi=150, facecolor=PAPER)

    # ── title ───────────────────────────────────────────────────────────────
    fig.suptitle(
        f"{meta.get('index_symbol', 'SPY')} dispersion — tearsheet",
        x=0.055, y=0.975, ha="left", fontsize=19, fontweight="bold", color=INK,
    )
    fig.text(
        0.055, 0.951,
        f"{meta.get('label', 'surface-path dispersion')}   |   "
        f"{meta.get('date_lo', '?')} → {meta.get('date_hi', '?')}   |   "
        f"{meta.get('n_sessions', '?')} sessions   |   "
        f"{meta.get('weighting', '?')} / {meta.get('strike_rule', '?')}   |   "
        f"gross index vega ${_f(meta, 'gross_index_vega'):,.0f}",
        ha="left", fontsize=9.5, color=MUTE,
    )

    # ── THE REGIME BANNER — full width, before any number ───────────────────
    band = fig.add_axes([0.055, 0.878, 0.90, 0.052])
    band.set_axis_off()
    band.add_patch(
        plt.Rectangle((0, 0), 1, 1, transform=band.transAxes, facecolor=colour,
                      edgecolor="none", zorder=0)
    )
    band.text(0.012, 0.66, badge, transform=band.transAxes, ha="left", va="center",
              fontsize=13, fontweight="bold", color="#ffffff")
    band.text(0.012, 0.26, f"{detail}   ·   {gloss}", transform=band.transAxes,
              ha="left", va="center", fontsize=9, color="#ffffff", alpha=0.95)
    band.text(0.988, 0.5,
              f"cost {cost:,.2f}  =  {abs(cost) / abs(gross) * 100.0:.0f}% of gross"
              if abs(gross) > 1e-12 else f"cost {cost:,.2f}",
              transform=band.transAxes, ha="right", va="center",
              fontsize=11, fontweight="bold", color="#ffffff")

    # ── headline tiles (every one captioned with the regime) ────────────────
    tiles = [
        ("Net return (after cost)", f"${net:,.2f}", tag, POS if net >= 0 else NEG),
        ("Gross return (pre-cost)", f"${gross:,.2f}", "frictionless equivalent", INK),
        ("Cost drag", f"-${abs(cost):,.2f}", tag, NEG if cost > 0 else INK),
        ("Sharpe", f"{_f(meta, 'sharpe'):.2f}", tag, INK),
        ("Max drawdown", f"${_f(meta, 'max_drawdown'):,.0f}", tag, INK),
        ("Return on gross vega", f"{_f(meta, 'return_on_gross_vega'):.4f}", tag, INK),
    ]
    for i, (label, value, note, col) in enumerate(tiles):
        _tile(fig, 0.055 + i * 0.1517, 0.775, 0.1417, 0.083, label, value, note, col)

    # ── NAV track (single series => no legend; the title names it) ──────────
    ax = fig.add_axes([0.055, 0.485, 0.90, 0.255])
    _style_axis(ax)
    ax.plot(d, nav, color=ACCENT, linewidth=2.0, zorder=5)
    ax.fill_between(d, nav, 0.0, where=(nav >= 0), color=ACCENT, alpha=0.10, zorder=1)
    ax.fill_between(d, nav, 0.0, where=(nav < 0), color=NEG, alpha=0.09, zorder=1)
    ax.fill_between(d, nav, run_max, color=NEG, alpha=0.12, zorder=2, linewidth=0)
    ax.axhline(0.0, color=MUTE, linewidth=0.9, linestyle=(0, (4, 3)), alpha=0.7)
    ax.scatter([d.iloc[-1]], [nav.iloc[-1]], s=34, color=ACCENT, zorder=6)
    ax.annotate(f"${nav.iloc[-1]:,.0f}", (d.iloc[-1], nav.iloc[-1]),
                textcoords="offset points", xytext=(-6, 9), ha="right",
                fontsize=10.5, fontweight="bold", color=ACCENT)
    _money(ax)
    _date_ticks(ax, span_days)
    ax.set_title(f"Cumulative P&L — {regime_key}  (drawdown shaded)",
                 fontsize=11, fontweight="bold", color=INK, loc="left", pad=8)

    # ── cost decomposition: gross -> cost -> financing -> net ───────────────
    axc = fig.add_axes([0.055, 0.285, 0.40, 0.145])
    _style_axis(axc)
    bars = [("Gross\n(pre-cost)", gross), ("Cost", -abs(cost)),
            ("Financing", financing), ("Net", net)]
    labels = [b[0] for b in bars]
    values = [b[1] for b in bars]
    axc.bar(labels, values, color=[POS if v >= 0 else NEG for v in values],
            width=0.62, linewidth=0)
    axc.axhline(0.0, color=MUTE, linewidth=0.9, alpha=0.7)
    for i, v in enumerate(values):  # direct labels — no legend needed
        axc.annotate(f"{v:,.0f}", (i, v), textcoords="offset points",
                     xytext=(0, 5 if v >= 0 else -12), ha="center",
                     fontsize=8.5, fontweight="bold", color=INK)
    _money(axc)
    axc.set_title("Where the P&L went", fontsize=10, fontweight="bold",
                  color=INK, loc="left", pad=6)

    # ── benchmark-relative block ────────────────────────────────────────────
    axb = fig.add_axes([0.525, 0.285, 0.43, 0.145])
    axb.set_axis_off()
    axb.set_title("Benchmark-relative", fontsize=10, fontweight="bold",
                  color=INK, loc="left", pad=6)
    if _has(meta, "benchmark_beta"):
        rows = [
            ("Information ratio", f"{_f(meta, 'benchmark_information_ratio'):.3f}"),
            ("Alpha (annualized)", f"${_f(meta, 'benchmark_alpha'):,.2f}"),
            ("Beta", f"{_f(meta, 'benchmark_beta'):.3f}"),
            ("Tracking error (ann.)", f"${_f(meta, 'benchmark_tracking_error'):,.2f}"),
            ("Active return (ann.)", f"${_f(meta, 'benchmark_active_return'):,.2f}"),
            ("Correlation", f"{_f(meta, 'benchmark_correlation'):.3f}"),
            ("Paired observations", meta.get("benchmark_n_obs", "?")),
        ]
        body = "\n".join(f"{k:<24}{v:>14}" for k, v in rows)
        axb.text(0.0, 0.94, body, transform=axb.transAxes, va="top", ha="left",
                 family="monospace", fontsize=8.6, color=INK)
    else:
        # An absent benchmark is stated, never implied as a zero alpha/beta.
        axb.text(0.0, 0.72,
                 "No benchmark series supplied.\n\n"
                 "IR / alpha / beta / tracking error are UNDEFINED for this run\n"
                 "and are deliberately not shown — an absent benchmark is not a\n"
                 "zero alpha. Set `benchmark_series` in the run spec (a date<TAB>pnl\n"
                 "TSV in the same $ units as the track) to populate this block.",
                 transform=axb.transAxes, va="top", ha="left",
                 fontsize=8.6, color=MUTE, style="italic")

    # ── cumulative greek attribution (diverging, direct-labeled ends) ───────
    axa = fig.add_axes([0.055, 0.055, 0.90, 0.155])
    _style_axis(axa)
    axes_cols = [("pnl_theta", "theta"), ("pnl_vega", "vega"), ("pnl_gamma", "gamma"),
                 ("pnl_delta", "delta"), ("pnl_unexplained", "unexplained")]
    totals = [(name, float(df[col].sum())) for col, name in axes_cols if col in df.columns]
    if "cost" in df.columns:
        totals.append(("cost", -float(df["cost"].sum())))
    totals.sort(key=lambda kv: kv[1])
    names = [t[0] for t in totals]
    vals = [t[1] for t in totals]
    axa.barh(names, vals, color=[POS if v >= 0 else NEG for v in vals],
             height=0.6, linewidth=0)
    axa.axvline(0.0, color=MUTE, linewidth=0.9, alpha=0.7)
    for i, v in enumerate(vals):  # position AND colour encode sign; labels are direct
        axa.annotate(f"{v:,.0f}", (v, i), textcoords="offset points",
                     xytext=(6 if v >= 0 else -6, 0),
                     ha="left" if v >= 0 else "right", va="center",
                     fontsize=8.5, color=INK)
    axa.xaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"${v:,.0f}"))
    axa.set_title(f"Cumulative attribution — {regime_key}",
                  fontsize=10, fontweight="bold", color=INK, loc="left", pad=6)

    fig.text(0.955, 0.016,
             f"atx-vol surface-path dispersion · {regime_key} · {detail}",
             ha="right", fontsize=8, color=MUTE, style="italic")

    fig.savefig(out, dpi=150, facecolor=PAPER)
    plt.close(fig)
    print(f"[spy_dispersion_tearsheet_report] wrote {out}")


def write_html(meta: dict, png: Path, out: Path) -> None:
    """One self-contained HTML page with the figure base64-inlined."""
    regime_key = meta.get("friction_regime", "")
    colour, badge, gloss = REGIME.get(regime_key, ("#6b7280", regime_key.upper(), ""))
    b64 = base64.b64encode(png.read_bytes()).decode("ascii")
    esc = html.escape
    doc = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{esc(meta.get('index_symbol', 'SPY'))} dispersion tearsheet — {esc(regime_key)}</title>
<style>
  :root {{ color-scheme: light dark; }}
  body {{ margin:0; padding:2rem 1.25rem; font:15px/1.55 -apple-system,BlinkMacSystemFont,
         "Segoe UI",Roboto,sans-serif; color:#1b1b2f; background:#fff; }}
  @media (prefers-color-scheme: dark) {{ body {{ color:#e8e8ef; background:#14141c; }} }}
  main {{ max-width:1180px; margin:0 auto; }}
  h1 {{ font-size:1.5rem; margin:0 0 .35rem; }}
  .sub {{ color:#6b7280; margin:0 0 1.25rem; font-size:.92rem; }}
  .regime {{ background:{colour}; color:#fff; padding:.85rem 1.1rem; border-radius:8px;
             margin:0 0 1.25rem; }}
  .regime b {{ display:block; font-size:1.05rem; letter-spacing:.03em; }}
  .regime span {{ font-size:.88rem; opacity:.95; }}
  figure {{ margin:0; }}
  img {{ width:100%; max-width:100%; height:auto; display:block;
         border:1px solid #e6e6ea; border-radius:8px; }}
  .note {{ color:#6b7280; font-size:.85rem; margin-top:1rem; }}
</style></head><body><main>
<h1>{esc(meta.get('index_symbol', 'SPY'))} dispersion — tearsheet</h1>
<p class="sub">{esc(meta.get('label', ''))} &middot; {esc(meta.get('date_lo', '?'))} &rarr;
   {esc(meta.get('date_hi', '?'))} &middot; {esc(meta.get('n_sessions', '?'))} sessions
   &middot; {esc(meta.get('weighting', '?'))} / {esc(meta.get('strike_rule', '?'))}</p>
<div class="regime"><b>{esc(badge)}</b>
  <span>{esc(meta.get('friction_detail', ''))} &mdash; {esc(gloss)}</span></div>
<figure><img alt="Dispersion tearsheet ({esc(regime_key)})"
     src="data:image/png;base64,{b64}"></figure>
<p class="note">Every figure on this page was produced under the
   <b>{esc(regime_key)}</b> execution regime. Results from different regimes are
   not comparable: on the pinned run the same strategy returns +247.41
   frictionless, +12.81 frictioned, and &minus;64.60 with square-root impact.</p>
</main></body></html>
"""
    out.write_text(doc, encoding="utf-8")
    print(f"[spy_dispersion_tearsheet_report] wrote {out}")


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if not argv:
        print(__doc__)
        return 2
    target = Path(argv[0])
    tsv = target / "surface_pnl_track.tsv" if target.is_dir() else target
    if not tsv.exists():
        print(f"error: {tsv} not found", file=sys.stderr)
        return 1

    meta, df = read_run(tsv)
    if df.empty:
        print(f"error: {tsv} has no data rows", file=sys.stderr)
        return 1

    # HARD REFUSAL. An unlabelled headline number is the specific failure this
    # report exists to prevent, so a track with no regime is not renderable.
    if "friction_regime" not in meta:
        print(
            f"error: {tsv} carries no `friction_regime` metadata key.\n"
            "       Refusing to render: a dispersion headline number is meaningless\n"
            "       without the execution regime that produced it. Re-run\n"
            "       `run-surface-backtest` with a build that emits it.",
            file=sys.stderr,
        )
        return 1

    out = Path(argv[1]) if len(argv) > 1 else tsv.with_name(tsv.stem + "_tearsheet.png")
    build(meta, df, out)
    write_html(meta, out, out.with_suffix(".html"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
