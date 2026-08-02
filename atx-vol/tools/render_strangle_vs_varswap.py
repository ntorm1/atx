#!/usr/bin/env python3
"""Comparison report for an XOM strangle-vs-varswap run.

Reads the single self-describing track TSV
`atx-vol-strangle-varswap-driver` writes through
`atx::vol::write_backtest_pnl_tsv` — a `# key=value` metadata header, the 27
pinned series columns, then one dynamic column per signal — and renders ONE
figure that puts the two legs of the comparison side by side on every axis the
run measured:

    python render_strangle_vs_varswap.py <track.tsv> [out.html|out.png]

    panel 1  cumulative P&L: the strangle leg against the variance-swap leg
    panel 2  vega:   `gross_vega`  vs `swap_vega`  (+ `strangle_vega`)
    panel 3  delta:  `gross_delta` vs `swap_delta`
    panel 4  gamma:  `gross_gamma` vs `swap_gamma`
    panel 5  theta:  `gross_theta` vs `swap_theta`

Default output: `<track-stem>_comparison.png` alongside the TSV. `.html` writes
a self-contained page with the figure inlined as a data URI; `.png`/`.svg`/
`.pdf` write the figure directly. Reuses the tools/tearsheet.py idiom (palette,
axis styling, stats box). Pure matplotlib (Agg backend) + pandas + stdlib.

## Three things about this input that the code below is shaped by

1. `pnl_total` IS THE WHOLE STEP. The engine's row total is options
   `pnl_explain` + settlement + hedge shares + financing - cost + the swap's
   flow (`backtest.cpp`, `step_total`), so the OPTIONS-side leg is
   `pnl_total - swap_pnl` and the two legs sum back to `nav` by construction.
   Neither `swap_pv` nor `swap_pnl` is part of the frozen serialized column set,
   so the driver rides them in as signal columns; a track written by anything
   else may not carry them, and an options-only run legitimately does not.

2. SWAP LIVENESS IS KEYED OFF `swap_vega`, never off `swap_theta`.
   `deriv_greeks` declines the roll stencil within one bump width of expiry, so
   `swap_theta` is legitimately NaN on its own while the swap is live. NaN
   `swap_*` rows are ordinary data besides: the tail cycle of any corpus whose
   calendar runs out mid-tenor is one-legged by design.

3. `skipped_restrikes` / `skipped_swaps` ARE CUMULATIVE. The per-session event
   count is the consecutive-row difference; the last row carries the run total.
"""

from __future__ import annotations

import base64
import dataclasses
import html
import io
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # headless

import matplotlib.dates as mdates  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.ticker as mticker  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

# ── palette (matches tools/tearsheet.py) ────────────────────────────────────
INK = "#1b1b2f"
MUTE = "#6b7280"
GRID = "#e6e6ea"
PAPER = "#ffffff"
STRANGLE = "#b4232a"  # red — the options leg
SWAP = "#0f766e"      # teal — the variance-swap leg
TOTAL = "#1b1b2f"     # ink — the combined book
SKIP = "#c2410c"      # amber — a hole in the schedule

# The comparison panels, in render order: key -> (title, book column, swap
# column, y-unit). The {book, swap} pair is the renderer's contract with the
# signal names the strategy froze; `build_figure` returns it so a test can hold
# the wiring to it without scraping the figure.
_GREEK_PANELS = (
    ("vega", "Vega", "gross_vega", "swap_vega", "$"),
    ("delta", "Delta", "gross_delta", "swap_delta", ""),
    ("gamma", "Gamma", "gross_gamma", "swap_gamma", ""),
    ("theta", "Theta", "gross_theta", "swap_theta", "$"),
)

_RENDERABLE_SUFFIXES = (".png", ".svg", ".pdf")
_HTML_SUFFIXES = (".html", ".htm")


# ── input ───────────────────────────────────────────────────────────────────

def read_track(path: Path) -> tuple[dict[str, str], pd.DataFrame]:
    """Return (meta, df) for a `write_backtest_pnl_tsv` track.

    `meta` is the leading `# k=v` header block; `df` is the tab-separated body
    with `date` parsed. Raises the underlying OSError/ValueError — a track that
    cannot be read is a caller error, not a rendering fallback.
    """
    meta: dict[str, str] = {}
    with Path(path).open("r", encoding="utf-8") as fh:
        for line in fh:
            if not line.startswith("#"):
                break
            k, _, v = line[1:].strip().partition("=")
            if v:
                meta[k.strip()] = v.strip()
    df = pd.read_csv(path, sep="\t", comment="#", parse_dates=["date"])
    if df.empty:
        raise ValueError(f"{path}: track has a header but no rows")
    return meta, df


def series(df: pd.DataFrame, name: str, default: float = float("nan")) -> pd.Series:
    """`df[name]` as float, or a constant series when the column is absent.

    Absence is legal input, not corruption: `swap_*` columns exist only on a run
    that had a swap lane, and the pinned column set carries neither `swap_pv`
    nor `swap_pnl`. The caller picks the stand-in — NaN for a MEASUREMENT that
    was never taken (so it draws as a gap), 0.0 for a FLOW that provably did not
    happen.
    """
    if name in df.columns:
        return pd.to_numeric(df[name], errors="coerce").astype(float)
    return pd.Series(np.full(len(df), default, dtype=float), index=df.index, name=name)


def swap_live_mask(df: pd.DataFrame) -> pd.Series:
    """Rows on which a variance swap was live — keyed off `swap_vega` ONLY.

    See module note (2): `swap_theta` is legitimately NaN on its own while the
    swap is live, so keying off it would report a live swap as dead.
    """
    return np.isfinite(series(df, "swap_vega"))


def step_events(df: pd.DataFrame, name: str) -> pd.Series:
    """A CUMULATIVE counter column differenced into per-recorded-row events.

    Row 0 carries its own value: the counter starts at 0 before the run, so
    whatever it already reads on the first recorded row happened on it. A
    missing column is no events at all.
    """
    cumulative = series(df, name, default=0.0).fillna(0.0)
    events = cumulative.diff()
    if len(events):
        events.iloc[0] = cumulative.iloc[0]
    return events


@dataclasses.dataclass(frozen=True)
class Legs:
    """The two legs of the comparison, split out of one track.

    `strangle_* + swap_* == nav` on every row, by construction — see module
    note (1).
    """

    date: pd.Series
    strangle_step: pd.Series
    strangle_cum: pd.Series
    swap_step: pd.Series
    swap_cum: pd.Series
    total_cum: pd.Series
    swap_pv: pd.Series
    swap_live: pd.Series
    restrike_events: pd.Series
    swap_skip_events: pd.Series

    @property
    def n_swap_live_rows(self) -> int:
        return int(self.swap_live.sum())

    @property
    def total_restrike_skips(self) -> float:
        return float(self.restrike_events.sum())

    @property
    def total_swap_skips(self) -> float:
        return float(self.swap_skip_events.sum())


def split_legs(df: pd.DataFrame) -> Legs:
    """Split a track into its options leg and its variance-swap leg."""
    pnl_total = series(df, "pnl_total", default=0.0).fillna(0.0)
    # A FLOW that is absent provably did not happen (the engine writes 0.0 on
    # every row of a book with no swap lots), so 0.0 — not NaN — is the honest
    # stand-in, and it keeps the legs summing to `nav`.
    swap_step = series(df, "swap_pnl", default=0.0).fillna(0.0)
    strangle_step = pnl_total - swap_step
    return Legs(
        date=df["date"],
        strangle_step=strangle_step,
        strangle_cum=strangle_step.cumsum(),
        swap_step=swap_step,
        swap_cum=swap_step.cumsum(),
        total_cum=pnl_total.cumsum(),
        swap_pv=series(df, "swap_pv", default=0.0).fillna(0.0),
        swap_live=swap_live_mask(df),
        restrike_events=step_events(df, "skipped_restrikes"),
        swap_skip_events=step_events(df, "skipped_swaps"),
    )


# ── styling ─────────────────────────────────────────────────────────────────

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


def _plain(ax) -> None:
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:,.4g}"))


def _date_ticks(ax, n_rows: int) -> None:
    # A 5-row fixture and a 250-row year want different locators; AutoDateLocator
    # picks one rather than drawing an empty axis on the short input.
    locator = mdates.AutoDateLocator(minticks=2, maxticks=7)
    ax.xaxis.set_major_locator(locator)
    ax.xaxis.set_major_formatter(mdates.ConciseDateFormatter(locator))
    if n_rows == 1:
        ax.margins(x=0.5)


def _meta_num(meta: dict[str, str], key: str, default: float = 0.0) -> float:
    try:
        return float(meta.get(key, default))
    except (TypeError, ValueError):
        return default


# ── figure ──────────────────────────────────────────────────────────────────

def build_figure(meta: dict[str, str], df: pd.DataFrame):
    """Render the comparison figure.

    @return (figure, panels) where `panels` maps each comparison panel's key to
            the pair of series it overlays. The caller owns the figure and must
            close it (`close_figure`).
    """
    legs = split_legs(df)
    d = legs.date
    n = len(df)
    panels: dict[str, tuple[str, str]] = {}

    fig = plt.figure(figsize=(13.0, 10.4), dpi=150, facecolor=PAPER)
    gs = fig.add_gridspec(
        3, 4, height_ratios=[2.4, 1.0, 1.0], hspace=0.42, wspace=0.30,
        left=0.070, right=0.965, top=0.845, bottom=0.06,
    )

    # ── panel 1: cumulative P&L, one line per leg ───────────────────────────
    ax = fig.add_subplot(gs[0, :])
    _style_axis(ax)
    ax.plot(d, legs.strangle_cum, color=STRANGLE, linewidth=2.0, zorder=5,
            label="strangle leg (pnl_total - swap_pnl)")
    ax.plot(d, legs.swap_cum, color=SWAP, linewidth=2.0, zorder=5,
            label="variance-swap leg (swap_pnl)")
    ax.plot(d, legs.total_cum, color=TOTAL, linewidth=1.1, alpha=0.55,
            linestyle=(0, (5, 3)), zorder=4, label="combined (nav)")
    ax.axhline(0.0, color=MUTE, linewidth=0.9, linestyle=(0, (4, 3)), alpha=0.7)
    _mark_schedule_holes(ax, legs)
    _money(ax)
    _date_ticks(ax, n)
    ax.legend(loc="best", frameon=False, fontsize=8.5, ncol=2, handlelength=1.6)
    ax.set_title("Cumulative P&L by leg", fontsize=11, fontweight="bold", color=INK,
                 loc="left", pad=8)
    _stats_box(ax, legs)
    panels["pnl"] = ("strangle_cum", "swap_cum")

    # ── panels 2-5: one greek each, book against swap ───────────────────────
    slots = (gs[1, :2], gs[1, 2:], gs[2, :2], gs[2, 2:])
    for slot, (key, title, book_col, swap_col, unit) in zip(slots, _GREEK_PANELS):
        axg = fig.add_subplot(slot)
        _style_axis(axg)
        axg.plot(d, series(df, book_col), color=STRANGLE, linewidth=1.6, zorder=5,
                 label=book_col)
        axg.plot(d, series(df, swap_col), color=SWAP, linewidth=1.6, zorder=5,
                 label=swap_col)
        if key == "vega":
            # The strangle's OWN dollar vega — the quantity the cycle's swap was
            # sized against, and equal to `swap_vega` on a cycle-open row. It is
            # not `gross_vega`, which is the whole book net of the hedge, so it
            # rides here as a reference rather than as the panel's pair. Drawn
            # ON TOP because it normally coincides with `gross_vega`: the
            # informative reading is where it does NOT (a step that could not
            # resolve its wings reports NaN here while `gross_vega` carries on).
            axg.plot(d, series(df, "strangle_vega"), color=MUTE, linewidth=1.1,
                     linestyle=(0, (4, 2)), zorder=6, label="strangle_vega")
        axg.axhline(0.0, color=MUTE, linewidth=0.8, alpha=0.6)
        _money(axg) if unit == "$" else _plain(axg)
        _date_ticks(axg, n)
        axg.legend(loc="best", frameon=False, fontsize=7.5, handlelength=1.4)
        axg.set_title(title, fontsize=9.5, fontweight="bold", color=INK, loc="left", pad=5)
        panels[key] = (book_col, swap_col)

    _titles(fig, meta, legs, n)
    return fig, panels


def _mark_schedule_holes(ax, legs: Legs) -> None:
    """Tick the sessions on which the schedule lost a restrike or a swap.

    Both counters are cumulative, so this draws `step_events`, not the columns.
    """
    marks = [
        (legs.restrike_events, "o", "restrike skipped"),
        (legs.swap_skip_events, "x", "cycle opened without a swap"),
    ]
    floor = float(np.nanmin([legs.strangle_cum.min(), legs.swap_cum.min(), 0.0]))
    for events, marker, label in marks:
        hit = events.to_numpy() > 0.0
        if not hit.any():
            continue
        ax.scatter(legs.date[hit], np.full(int(hit.sum()), floor), marker=marker, s=26,
                   color=SKIP, zorder=6, label=label)


def _stats_box(ax, legs: Legs) -> None:
    rows = [
        ("Strangle P&L", f"${float(legs.strangle_cum.iloc[-1]):,.0f}"),
        ("Var-swap P&L", f"${float(legs.swap_cum.iloc[-1]):,.0f}"),
        ("Combined P&L", f"${float(legs.total_cum.iloc[-1]):,.0f}"),
        ("Swap-live rows", f"{legs.n_swap_live_rows} / {len(legs.date)}"),
        ("Restrike skips", f"{legs.total_restrike_skips:,.0f}"),
        ("Swapless cycles", f"{legs.total_swap_skips:,.0f}"),
    ]
    txt = "\n".join(f"{k:<16}{v:>13}" for k, v in rows)
    ax.text(
        0.012, 0.97, txt, transform=ax.transAxes, va="top", ha="left",
        family="monospace", fontsize=9, color=INK,
        bbox=dict(boxstyle="round,pad=0.6", fc="#f7f7f5", ec=GRID, lw=1.0),
    )


def _titles(fig, meta: dict[str, str], legs: Legs, n_rows: int) -> None:
    symbol = meta.get("symbol", "?")
    strat = meta.get("strategy", "strangle vs variance swap")
    fig.suptitle(f"{symbol}  ·  {strat}", x=0.070, y=0.965, ha="left", fontsize=18,
                 fontweight="bold", color=INK)
    line1 = (
        f"{meta.get('window_start', '?')} → {meta.get('window_end', '?')}   |   "
        f"{n_rows} recorded sessions   |   "
        f"{_meta_num(meta, 'delta_target'):.2f}Δ strangle, "
        f"{_meta_num(meta, 'tenor_days'):.0f}d fixed-expiry cycles, "
        f"{_meta_num(meta, 'contracts'):.0f} contracts/wing"
    )
    line2 = (
        f"hedge: {meta.get('hedge', 'off')}   |   "
        f"swap: uncapped variance, struck fair, sized to the strangle's entry vega   |   "
        f"data: {meta.get('data_source', '?')}"
    )
    fig.text(0.070, 0.912, line1, ha="left", fontsize=10.5, color=MUTE)
    fig.text(0.070, 0.886, line2, ha="left", fontsize=10.5, color=MUTE)
    note = (
        "NaN swap rows are a one-legged cycle, not an error; swap liveness is "
        "read off swap_vega (swap_theta is NaN near expiry while live)."
    )
    fig.text(0.965, 0.014, f"atx-vol · {note}", ha="right", fontsize=8.0, color=MUTE,
             style="italic")


def close_figure(fig) -> None:
    """Release a figure returned by `build_figure`."""
    plt.close(fig)


# ── output ──────────────────────────────────────────────────────────────────

def _write_html(fig, out: Path, meta: dict[str, str], legs: Legs) -> None:
    """A self-contained page: the figure inlined as a data URI, no sidecars."""
    buf = io.BytesIO()
    fig.savefig(buf, format="png", dpi=150, facecolor=PAPER)
    encoded = base64.b64encode(buf.getvalue()).decode("ascii")
    symbol = html.escape(meta.get("symbol", "?"))
    strategy = html.escape(meta.get("strategy", "strangle vs variance swap"))
    window = html.escape(
        f"{meta.get('window_start', '?')} .. {meta.get('window_end', '?')}"
    )
    out.write_text(
        "<!doctype html>\n"
        '<html lang="en"><head><meta charset="utf-8">\n'
        f"<title>{symbol} — strangle vs variance swap</title>\n"
        "<style>body{margin:0;padding:24px;background:#ffffff;color:#1b1b2f;"
        "font-family:system-ui,-apple-system,Segoe UI,sans-serif}"
        "img{max-width:100%;height:auto;display:block}"
        "p{color:#6b7280;font-size:14px}</style>\n"
        "</head><body>\n"
        f"<h1>{symbol} — {strategy}</h1>\n"
        f"<p>{window} · strangle ${float(legs.strangle_cum.iloc[-1]):,.0f} · "
        f"variance swap ${float(legs.swap_cum.iloc[-1]):,.0f} · "
        f"{legs.n_swap_live_rows}/{len(legs.date)} swap-live sessions</p>\n"
        f'<img alt="{symbol} strangle vs variance swap comparison panels" '
        f'src="data:image/png;base64,{encoded}">\n'
        "</body></html>\n",
        encoding="utf-8",
    )


def render(track: Path, out: Path) -> dict[str, float]:
    """Render `track` to `out`; return the headline comparison numbers.

    `out`'s suffix selects the format: `.html`/`.htm` inlines the figure into a
    self-contained page, `.png`/`.svg`/`.pdf` writes the figure directly.
    """
    track = Path(track)
    out = Path(out)
    suffix = out.suffix.lower()
    if suffix not in _RENDERABLE_SUFFIXES + _HTML_SUFFIXES:
        raise ValueError(
            f"unsupported output '{out.name}': expected one of "
            + ", ".join(_RENDERABLE_SUFFIXES + _HTML_SUFFIXES)
        )
    meta, df = read_track(track)
    legs = split_legs(df)
    fig, _panels = build_figure(meta, df)
    try:
        out.parent.mkdir(parents=True, exist_ok=True)
        if suffix in _HTML_SUFFIXES:
            _write_html(fig, out, meta, legs)
        else:
            fig.savefig(out, dpi=150, facecolor=PAPER)
    finally:
        close_figure(fig)
    return {
        "n_rows": len(df),
        "n_swap_live_rows": legs.n_swap_live_rows,
        "strangle_total": float(legs.strangle_cum.iloc[-1]),
        "swap_total": float(legs.swap_cum.iloc[-1]),
        "combined_total": float(legs.total_cum.iloc[-1]),
        "skipped_restrikes": legs.total_restrike_skips,
        "skipped_swaps": legs.total_swap_skips,
    }


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if not args:
        print(__doc__)
        return 2
    track = Path(args[0])
    if not track.exists():
        print(f"error: {track} not found", file=sys.stderr)
        return 1
    out = Path(args[1]) if len(args) > 1 else track.with_name(track.stem + "_comparison.png")
    summary = render(track, out)
    print(
        f"[strangle-vs-varswap] wrote {out}\n"
        f"  strangle ${summary['strangle_total']:,.0f} | "
        f"var-swap ${summary['swap_total']:,.0f} | "
        f"combined ${summary['combined_total']:,.0f}\n"
        f"  {summary['n_swap_live_rows']:.0f}/{summary['n_rows']:.0f} swap-live sessions | "
        f"{summary['skipped_restrikes']:.0f} restrike skips | "
        f"{summary['skipped_swaps']:.0f} swapless cycles"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
