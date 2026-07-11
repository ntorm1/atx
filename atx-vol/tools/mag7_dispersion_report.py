#!/usr/bin/env python3
"""Self-contained HTML/SVG report for a mag7_dispersion_backtest run dir.

Reads the five-file run-output contract emitted by
examples/mag7_dispersion_backtest.cpp (Task 6): `series.csv`,
`strategy_metrics.csv`, `engine_metrics.csv`, `db_stats.csv`, and the
optional `populate_stats.csv` -- each a `# key=value` metadata header
followed by a CSV body (`tools/tearsheet.py`'s `read_run` parsing
precedent). Renders ONE self-contained HTML file: inline SVG chart(s)
(matplotlib Agg -> StringIO -> stripped of everything matplotlib normally
needs for a standalone XML document), one inline `<style>` block, no JS, no
external assets, no network references of any kind.

    python mag7_dispersion_report.py <run-dir> [out.html]

Default output: `<run-dir>/mag7_dispersion_report.html`.

Pure `pandas` + `matplotlib` (Agg backend) + stdlib. No new dependencies.
"""

from __future__ import annotations

import html
import io
import re
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # headless
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import pandas as pd

# ── palette (matches tools/tearsheet.py, for visual consistency) ───────────
INK = "#1b1b2f"
MUTE = "#6b7280"
GRID = "#e6e6ea"
PAPER = "#ffffff"
ACCENT = "#0f766e"   # teal — the NAV / equity track
NEG = "#b4232a"      # red — losses / drawdown
GREEK_C = {"theta": "#0f766e", "vega": "#7c3aed", "gamma": "#c2410c", "unexplained": "#6b7280"}

# ── pinned section headings (exact strings; the gate test greps for these) ─
H_TITLE = "MAG7 dispersion-strangle backtest report"
H_STRATEGY = "Strategy metrics"
H_ENGINE = "Engine metrics"
H_SURFACE = "Surface/db statistics"

# Required run-dir files (Task 6 contract); populate_stats.csv is optional.
REQUIRED_FILES = ("series.csv", "strategy_metrics.csv", "engine_metrics.csv", "db_stats.csv")

# Shared meta keys written verbatim into every T6-emitted file
# (examples/mag7_dispersion_backtest.cpp `MetaKv meta`), in pinned emission
# order, paired with a plain-language description. Rendering every one of
# these in the report header is an explicit acceptance requirement.
SHARED_META_FIELDS = [
    ("strategy", "Strategy identifier"),
    ("names", "Constituent names"),
    ("index_symbol", "Dispersion index / benchmark symbol"),
    ("data_source", "Market-data source"),
    ("db_root", "SurfaceDb root path"),
    ("db_generation", "SurfaceDb generation"),
    ("window_start", "Backtest window start date"),
    ("window_end", "Backtest window end date"),
    ("n_steps", "Number of backtest steps (trading days)"),
    ("delta_target", "Target |delta| per strangle leg"),
    ("tenor_days", "Target tenor at entry (days)"),
    ("close_dte_days", "Close-out DTE threshold (days)"),
    ("theta_per_name_daily", "Theta budget per name per day ($)"),
    ("entry_every_n_days", "Entry cadence (every N trading days)"),
    ("multiplier", "Contract multiplier"),
    ("frictions", "Friction model (on/off)"),
    ("missing_policy", "Missing-name handling policy"),
    ("min_names", "Minimum live names required"),
]

# strategy_metrics.csv rows that belong on the engine-metrics panel instead
# (unpriced-lot/greek counts are an engine-fidelity signal, not a strategy
# return metric).
UNPRICED_METRIC_KEYS = ("total_unpriced_lots", "total_unpriced_greeks")


# ── meta+CSV reader (tools/tearsheet.py's `read_run`, kept generic: no
#    parse_dates, since this helper is shared across all five run-dir CSVs
#    and only series.csv has a `date` column) ───────────────────────────────
def read_meta_csv(path) -> tuple[dict, pd.DataFrame]:
    """Return (meta: dict, df: DataFrame). Metadata is the `# k=v` header block."""
    path = Path(path)
    meta: dict[str, str] = {}
    with path.open("r", encoding="utf-8") as fh:
        for line in fh:
            if not line.startswith("#"):
                break
            k, _, v = line[1:].strip().partition("=")
            if v:
                meta[k.strip()] = v.strip()
    df = pd.read_csv(path, comment="#")
    return meta, df


# ── SVG embedding ────────────────────────────────────────────────────────
def _fig_to_inline_svg(fig, id_prefix: str) -> str:
    """Render `fig` to a self-contained inline `<svg>...</svg>` fragment.

    matplotlib's SVG backend emits a full standalone XML document (an XML
    declaration, a DOCTYPE with a w3.org DTD reference, and an RDF
    `<metadata>` block carrying more w3.org/purl.org/matplotlib.org links)
    plus `xmlns`/`xmlns:xlink` namespace declarations on the `<svg>` root.
    None of that is needed once the fragment is embedded inline in an HTML5
    document -- the HTML5 parser assigns the SVG/XLink namespaces to `<svg>`
    content automatically, per the "adjust foreign attributes" step of the
    tree-construction algorithm -- and the raw http(s):// URIs would
    otherwise trip this report's self-containment discipline. Strip all of
    it, and prefix every generated id (and every intra-document id
    reference: `id=`, `href="#..."`, `url(#...)`) with `id_prefix` so
    multiple charts embedded on the same page cannot collide on
    matplotlib's per-render id counters (e.g. "figure_1", "line2d_3" repeat
    across independent `savefig` calls in the same process).
    """
    buf = io.StringIO()
    fig.savefig(buf, format="svg")
    plt.close(fig)
    svg = buf.getvalue()
    svg = svg[svg.index("<svg"):]  # drops the XML decl + DOCTYPE in one shot
    svg = re.sub(r"<metadata>.*?</metadata>\s*", "", svg, flags=re.DOTALL)
    end_tag = svg.index(">")
    svg = re.sub(r'\s+xmlns(:xlink)?="[^"]*"', "", svg[:end_tag]) + svg[end_tag:]
    svg = re.sub(r'\bid="([^"]+)"', lambda m: f'id="{id_prefix}{m.group(1)}"', svg)
    svg = re.sub(
        r'(xlink:href|href)="#([^"]+)"',
        lambda m: f'{m.group(1)}="#{id_prefix}{m.group(2)}"',
        svg,
    )
    svg = re.sub(r"url\(#([^)]+)\)", lambda m: f"url(#{id_prefix}{m.group(1)})", svg)
    return svg


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


def _equity_drawdown_svg(series_df: pd.DataFrame) -> str:
    """Chart 1 (required): cumulative P&L/NAV equity curve with a drawdown
    panel beneath (shared x, `fill_between` shading)."""
    d = pd.to_datetime(series_df["date"])
    nav = series_df["nav"]
    run_max = nav.cummax()
    drawdown = nav - run_max

    fig, (ax1, ax2) = plt.subplots(
        2, 1, figsize=(11.0, 6.2), dpi=110, facecolor=PAPER, layout="constrained",
        gridspec_kw={"height_ratios": [2.2, 1.0], "hspace": 0.06}, sharex=True,
    )
    _style_axis(ax1)
    ax1.plot(d, nav, color=ACCENT, linewidth=1.8, zorder=5)
    ax1.fill_between(d, nav, 0.0, where=(nav >= 0), color=ACCENT, alpha=0.12, zorder=1)
    ax1.fill_between(d, nav, 0.0, where=(nav < 0), color=NEG, alpha=0.10, zorder=1)
    ax1.axhline(0.0, color=MUTE, linewidth=0.9, linestyle=(0, (4, 3)), alpha=0.7)
    _money(ax1)
    ax1.set_title("Cumulative P&L / NAV", fontsize=11, fontweight="bold", color=INK, loc="left")

    _style_axis(ax2)
    ax2.fill_between(d, drawdown, 0.0, color=NEG, alpha=0.25, zorder=2)
    ax2.plot(d, drawdown, color=NEG, linewidth=1.1, zorder=3)
    _money(ax2)
    ax2.set_title("Drawdown", fontsize=10, fontweight="bold", color=INK, loc="left")
    plt.setp(ax2.get_xticklabels(), rotation=30, ha="right")

    return _fig_to_inline_svg(fig, "eq-")


def _attribution_svg(series_df: pd.DataFrame) -> str:
    """Chart 2: cumulative attribution lines (theta/vega/gamma/unexplained
    cumsums)."""
    d = pd.to_datetime(series_df["date"])
    fig, ax = plt.subplots(figsize=(11.0, 4.4), dpi=110, facecolor=PAPER, layout="constrained")
    _style_axis(ax)
    for key, color in (
        ("pnl_theta", GREEK_C["theta"]),
        ("pnl_vega", GREEK_C["vega"]),
        ("pnl_gamma", GREEK_C["gamma"]),
        ("pnl_unexplained", GREEK_C["unexplained"]),
    ):
        if key in series_df.columns:
            ax.plot(d, series_df[key].cumsum(), color=color, linewidth=1.6,
                     label=key.replace("pnl_", ""))
    ax.axhline(0.0, color=MUTE, linewidth=0.8, alpha=0.6)
    _money(ax)
    ax.legend(loc="upper left", frameon=False, fontsize=8, ncol=4, handlelength=1.3)
    ax.set_title("Cumulative attribution", fontsize=11, fontweight="bold", color=INK, loc="left")
    plt.setp(ax.get_xticklabels(), rotation=30, ha="right")

    return _fig_to_inline_svg(fig, "attr-")


# ── value formatting + table helpers ────────────────────────────────────
def _fmt_value(v) -> str:
    """Format a table scalar sensibly: near-integers render without a
    fractional part, other numerics get 4 decimals + thousands separators,
    non-numeric values (symbols, policy strings) pass through unchanged."""
    try:
        f = float(v)
    except (TypeError, ValueError):
        return str(v)
    if f != f:  # NaN
        return "nan"
    if f == int(f) and abs(f) < 1e15:
        return f"{int(f):,}"
    return f"{f:,.4f}"


def _kv_table_html(heading: str, rows) -> str:
    """`rows`: iterable of (label, value) pairs; `value` is str-formatted."""
    body = "".join(
        f"<tr><td>{html.escape(str(k))}</td><td>{html.escape(str(v))}</td></tr>"
        for k, v in rows
    )
    return (
        f'<section class="panel">\n<h2>{html.escape(heading)}</h2>\n'
        f"<table><thead><tr><th>Metric</th><th>Value</th></tr></thead>\n"
        f"<tbody>\n{body}\n</tbody></table>\n</section>\n"
    )


def _df_table_html(heading: str, df: pd.DataFrame) -> str:
    header_cells = "".join(f"<th>{html.escape(str(c))}</th>" for c in df.columns)
    body_rows = "".join(
        "<tr>" + "".join(f"<td>{html.escape(_fmt_value(v))}</td>" for v in row) + "</tr>"
        for row in df.itertuples(index=False)
    )
    return (
        f'<div class="panel">\n<h3>{html.escape(heading)}</h3>\n'
        f"<table><thead><tr>{header_cells}</tr></thead>\n"
        f"<tbody>{body_rows}</tbody></table>\n</div>\n"
    )


def _human_bytes(n) -> str:
    n = float(n)
    for unit in ("B", "KB", "MB", "GB"):
        if abs(n) < 1024.0:
            return f"{n:,.1f} {unit}"
        n /= 1024.0
    return f"{n:,.1f} TB"


# ── section builders ────────────────────────────────────────────────────
def _header_html(meta: dict) -> str:
    rows = "".join(
        f'<tr><td class="key"><code>{html.escape(key)}</code></td>'
        f"<td>{html.escape(desc)}</td>"
        f'<td>{html.escape(meta.get(key, "?"))}</td></tr>'
        for key, desc in SHARED_META_FIELDS
    )
    subtitle = (
        f'{html.escape(meta.get("strategy", "?"))} &middot; '
        f'{html.escape(meta.get("names", "?"))} vs {html.escape(meta.get("index_symbol", "?"))} '
        f'&middot; {html.escape(meta.get("window_start", "?"))} '
        f'&rarr; {html.escape(meta.get("window_end", "?"))} '
        f'({html.escape(meta.get("n_steps", "?"))} steps)'
    )
    return (
        "<header>\n"
        f"<h1>{html.escape(H_TITLE)}</h1>\n"
        f'<p class="subtitle">{subtitle}</p>\n'
        "<table><thead><tr><th>Key</th><th>Description</th><th>Value</th></tr></thead>\n"
        f"<tbody>{rows}</tbody></table>\n"
        "</header>\n"
    )


def _strategy_section(strat_df: pd.DataFrame) -> str:
    rows = [(r.metric, _fmt_value(r.value)) for r in strat_df.itertuples(index=False)]
    return _kv_table_html(H_STRATEGY, rows)


def _engine_section(engine_df: pd.DataFrame, strat_df: pd.DataFrame) -> str:
    rows = [(r.metric, _fmt_value(r.value)) for r in engine_df.itertuples(index=False)]
    rows += [
        (r.metric, _fmt_value(r.value))
        for r in strat_df.itertuples(index=False)
        if r.metric in UNPRICED_METRIC_KEYS
    ]
    return _kv_table_html(H_ENGINE, rows)


def _surface_section(db_meta: dict, db_df: pd.DataFrame,
                      populate_df: pd.DataFrame | None) -> str:
    parts = db_df.sort_values("key")
    n_partitions = len(parts)
    if n_partitions:
        first_key, last_key = parts.iloc[0]["key"], parts.iloc[-1]["key"]
        sizes = parts["file_size"]
        total_size, avg_size = sizes.sum(), sizes.mean()
        min_size, max_size = sizes.min(), sizes.max()
    else:
        first_key = last_key = "?"
        total_size = avg_size = min_size = max_size = 0

    rows = [
        ("db_root", db_meta.get("db_root", "?")),
        ("generation", db_meta.get("generation", "?")),
        ("n_symbols", db_meta.get("n_symbols", "?")),
        ("n_partitions", db_meta.get("n_partitions", str(n_partitions))),
        ("dates_covered", f"{first_key} → {last_key} ({n_partitions} partitions)"),
        ("total_partition_size", _human_bytes(db_meta.get("total_file_size", total_size))),
        ("avg_partition_size", _human_bytes(avg_size)),
        ("min_partition_size", _human_bytes(min_size)),
        ("max_partition_size", _human_bytes(max_size)),
    ]
    out = _kv_table_html(H_SURFACE, rows)
    if populate_df is not None and not populate_df.empty:
        out += _df_table_html("Per-symbol fit success", populate_df)
    return out


def _style_block() -> str:
    return f"""<style>
:root {{ color-scheme: light dark; }}
* {{ box-sizing: border-box; }}
body {{
  font-family: -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  margin: 0 auto; padding: 2rem; max-width: 1080px;
  background: {PAPER}; color: {INK};
}}
h1 {{ font-size: 1.6rem; margin: 0 0 0.3rem; }}
h2 {{ font-size: 1.15rem; margin: 1.8rem 0 0.6rem; }}
h3 {{ font-size: 0.98rem; margin: 1rem 0 0.5rem; color: {MUTE}; }}
.subtitle {{ color: {MUTE}; margin: 0 0 1.2rem; }}
table {{ border-collapse: collapse; width: 100%; margin-bottom: 0.6rem; font-size: 0.86rem; }}
th, td {{ text-align: left; padding: 0.32rem 0.7rem; border-bottom: 1px solid {GRID}; }}
th {{ color: {MUTE}; font-weight: 600; }}
td.key code, td code {{ font-family: ui-monospace, SFMono-Regular, Consolas, monospace; }}
.panel {{ margin-bottom: 1.4rem; }}
.chart {{ margin: 0.8rem 0 1.6rem; overflow-x: auto; }}
.chart svg {{ max-width: 100%; height: auto; display: block; }}
footer {{ color: {MUTE}; font-size: 0.78rem; margin-top: 2rem; }}
</style>"""


def build_html(run_dir: Path) -> str:
    series_meta, series_df = read_meta_csv(run_dir / "series.csv")
    _strat_meta, strat_df = read_meta_csv(run_dir / "strategy_metrics.csv")
    _engine_meta, engine_df = read_meta_csv(run_dir / "engine_metrics.csv")
    db_meta, db_df = read_meta_csv(run_dir / "db_stats.csv")

    populate_path = run_dir / "populate_stats.csv"
    populate_df = read_meta_csv(populate_path)[1] if populate_path.exists() else None

    chart1 = _equity_drawdown_svg(series_df)
    chart2 = _attribution_svg(series_df)

    body = "\n".join([
        _header_html(series_meta),
        '<section class="panel">',
        "<h2>Equity curve &amp; drawdown</h2>",
        f'<div class="chart">{chart1}</div>',
        "</section>",
        '<section class="panel">',
        "<h2>Cumulative attribution</h2>",
        f'<div class="chart">{chart2}</div>',
        "</section>",
        _strategy_section(strat_df),
        _engine_section(engine_df, strat_df),
        _surface_section(db_meta, db_df, populate_df),
        "<footer>atx-vol &middot; mag7_dispersion_report.py</footer>",
    ])

    return (
        "<!doctype html>\n"
        '<html lang="en">\n<head>\n<meta charset="utf-8">\n'
        f"<title>{html.escape(H_TITLE)}</title>\n"
        f"{_style_block()}\n</head>\n<body>\n{body}\n</body>\n</html>\n"
    )


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if not argv:
        print(__doc__)
        return 2

    run_dir = Path(argv[0])
    if not run_dir.is_dir():
        print(f"error: {run_dir} is not a directory", file=sys.stderr)
        return 1

    missing = [f for f in REQUIRED_FILES if not (run_dir / f).exists()]
    if missing:
        print(f"error: {run_dir} is missing required file(s): {', '.join(missing)}",
              file=sys.stderr)
        return 1

    out = Path(argv[1]) if len(argv) > 1 else run_dir / "mag7_dispersion_report.html"
    doc = build_html(run_dir)
    out.write_text(doc, encoding="utf-8")
    print(f"[mag7_dispersion_report] wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
