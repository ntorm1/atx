"""Build the SPY listed-options dispersion backtest report.

Renders the output of `examples/spy_dispersion_backtest.cpp` as one
self-contained HTML file, preferring the shipped CLI's `run.atxrun` archive and
falling back to legacy loose TSV tracks. It also doubles as the worked example
for the component library.

The strategy in that run is the traditional dispersion proxy: a SHORT SPY ATM
straddle against LONG constituent ATM straddles, sized vega-flat at entry on the
served American vegas, delta-hedged daily at the close and rolled on a common
listed monthly expiry. Prices and Greeks come from atx-vol American fitted
surfaces reloaded from the archive, so the whole track is a replay over cached
fits rather than a re-fit.

    from atxvol.report.dispersion import build_report_from_run
    build_report_from_run("path/to/golden-run", "pnl_track.html")
"""

from __future__ import annotations

import math
import os
from typing import Mapping, Sequence

import atxvol as _av

from . import charts, theme
from .charts import Series
from .components import (
    Banner, Column, FacetGrid, Figure, Note, Prose, Report, Section, Stat, StatRow, Subhead,
    Table, esc,
)
from .io import read_backtest_archive_result, read_backtest_tsv, read_kv_tsv

# ── The friction regime is a first-class dimension, not a footnote ──────────
#
# On the pinned 82-session run the SAME strategy over the SAME surfaces returns
# +24740.62 frictionless, +1280.83 under retail frictions (cost 23459.79) and
# -6460.23 once square-root impact is added (cost 31200.85): ~95% friction-dominated, and
# the SIGN FLIPS under modest impact. A tearsheet showing only the frictionless
# number is not incomplete, it is actively misleading. So the engine leads both
# reporting artifacts with `friction_regime` / `friction_detail`
# (`dispersion_run.hpp`: "THE REGIME IS NOT OPTIONAL METADATA"), and this
# renderer honours the same contract:
#
#   * a full-width colour-coded BANNER carrying its own text label sits directly
#     under the masthead, before any number;
#   * every headline tile is captioned with the regime, so a cropped screenshot
#     of a single tile still says which assumptions produced it;
#   * the P&L chart title names it; and
#   * a track with no regime is REFUSED rather than silently rendered.
#
# Key -> (Banner tone, text badge, plain-language gloss). The tones come from
# `theme.STATUS`, a validated 3-state set.
REGIME_KEY = "friction_regime"
DETAIL_KEY = "friction_detail"
REGIMES: Mapping[str, tuple[str, str, str]] = {
    "frictionless": (
        "ok", "FRICTIONLESS",
        "mid fills, no commission — an upper bound, not a tradeable result",
    ),
    "frictioned": ("warn", "FRICTIONED", "spread + commission applied"),
    "frictioned+impact": (
        "alert", "FRICTIONED + IMPACT",
        "spread + commission + square-root market impact",
    ),
}

# Series files a run directory may carry, in preference order. The two
# `surface_*` names are `write_dispersion_tearsheet`'s; `pnl_track.tsv` is what
# the README's Python pipeline writes (`write_backtest_pnl_tsv`) and was missing
# here, so a user following the README then calling this renderer hit a bare
# FileNotFoundError; `backtest.tsv` is the legacy loose-SoA name.
TRACK_NAMES = (
    "surface_pnl_track.tsv",
    "pnl_track.tsv",
    "surface_backtest.tsv",
    "backtest.tsv",
)

# Columns the dispersion report economically folds. A loose TSV missing any of
# these is not renderable: BacktestResult must remain row-consistent and
# therefore has zero-filled placeholders, but omitted economics must never
# become real zeroes in a headline or attribution closure.
REQUIRED_REPORT_COLUMNS = frozenset({
    "date", "ts_ns", "pnl_total", "pnl_delta", "pnl_gamma", "pnl_vega",
    "pnl_vanna", "pnl_volga", "pnl_theta", "pnl_rho", "pnl_charm",
    "pnl_unexplained", "pnl_settlement", "pnl_shares", "financing", "cost",
    "nav", "gross_gamma", "gross_vega", "turnover_notional", "turnover_vega",
})

# These fields enrich risk/invariant panels but do not enter the economic fold.
# A legacy TSV may omit them; the report labels them unavailable and omits the
# corresponding chart instead of presenting resize-created zeroes.
OPTIONAL_REPORT_COLUMNS = frozenset({
    "cash", "gross_delta", "gross_theta", "n_open_lots", "n_unpriced_lots",
    "n_unpriced_greeks",
})

# Attribution axes. Display order == palette slot order, which is the exact
# arrangement the palette was validated in: permuting the two only weakens the
# adjacent-pair CVD separation. Color follows the axis, never its rank, so
# reordering the legend must not repaint a series.
#
# `label_end` marks the two extremes (theta down, gamma up). They separate
# cleanly at the right edge; the middle four converge near zero, so labelling
# them too would collide — the legend and table carry those.
AXES = (
    ("Theta", "pnl_theta", 0, True),
    ("Gamma", "pnl_gamma", 1, True),
    ("Vega", "pnl_vega", 2, False),
    ("Delta", "pnl_delta", 3, False),
    ("Vanna", "pnl_vanna", 4, False),
    ("Unexplained", "pnl_unexplained", 5, False),
)


def _money(v: float, dp: int = 0) -> str:
    if v is None or not math.isfinite(v):
        return "--"
    return f"{'-' if v < 0 else ''}${abs(v):,.{dp}f}"


def _cum(values: Sequence[float]) -> list[float]:
    out, run = [], 0.0
    for v in values:
        run += v if math.isfinite(v) else 0.0
        out.append(run)
    return out


def _short(dates: Sequence[str]) -> list[str]:
    return [d[5:] if len(d) == 10 else d for d in dates]


def build_report_from_run(run_dir: str, path: str, *, label: str = "",
                          section: str = "backtest") -> str:
    """Render a `spy_dispersion_backtest` run directory into an HTML report.

    ``run.atxrun`` is authoritative when present; ``TRACK_NAMES`` are legacy
    fallbacks. Raises ``ValueError`` before folding a loose TSV that omits
    required economics, and ``atxvol.AtxError`` with
    ``code == ErrorCode.INVALID_ARGUMENT`` if the run carries no
    ``friction_regime`` — see ``REGIMES``.
    """
    archive = run_dir if os.path.isfile(run_dir) else os.path.join(run_dir, "run.atxrun")
    sidecar_dir = os.path.dirname(archive) if os.path.isfile(run_dir) else run_dir
    source = ""
    if os.path.isfile(archive):
        result, meta, extra = read_backtest_archive_result(archive, section)
        columns_present = extra.columns_present
        source = f"run.atxrun · {section} section"
    else:
        if section != "backtest":
            raise FileNotFoundError(
                f"{archive}: section {section!r} requires a RunArchive"
            )
        backtest = ""
        for name in TRACK_NAMES:
            candidate = os.path.join(sidecar_dir, name)
            if os.path.exists(candidate):
                backtest = candidate
                break
        if not backtest:
            raise FileNotFoundError(
                f"{run_dir}: no backtest series found (looked for run.atxrun, "
                + ", ".join(TRACK_NAMES) + ")"
            )
        result, meta, extra = read_backtest_tsv(backtest)
        columns_present = extra.columns_present
        source = f"{os.path.basename(backtest)} · legacy loose TSV"

    missing_required = sorted(REQUIRED_REPORT_COLUMNS - columns_present)
    if missing_required:
        raise ValueError(
            f"{source}: missing required dispersion-report columns: "
            f"{', '.join(missing_required)}. Refusing to treat omitted economics as zero."
        )

    # Precedence, weakest first: resolved spec, effective config, legacy
    # tearsheet metadata, then archive/track metadata — closest to the numbers
    # wins.
    spec: dict[str, str] = {}
    for name in ("run_spec.tsv", "run_config.tsv", "surface_tearsheet.tsv"):
        candidate = os.path.join(sidecar_dir, name)
        if os.path.exists(candidate):
            spec.update(read_kv_tsv(candidate))
    spec.update(meta)
    # The shipped CLI's effective config spells this key out; legacy report
    # metadata used the shorter name.
    if DETAIL_KEY not in spec and "friction_regime_detail" in spec:
        spec[DETAIL_KEY] = spec["friction_regime_detail"]

    counters_path = os.path.join(sidecar_dir, "backtest_counters.tsv")
    counters = read_kv_tsv(counters_path) if os.path.exists(counters_path) else {}

    # The fold is the library's, not a Python reimplementation.
    sheet = _av.tearsheet(result)
    return _render(
        result, sheet, spec, counters, path, label or spec.get("label", ""),
        columns_present=columns_present, source=source,
    )


def _binding_result(result) -> "_av.BacktestResult":
    """Materialize a RunArchive section into the binding's foldable result."""
    if isinstance(result, _av.BacktestResult):
        return result
    out = _av.BacktestResult()
    out.resize(len(result))
    out.date = list(result.date)
    out.ts_ns = list(result.ts_ns)
    for name, values in result.series.items():
        if hasattr(out, name):
            setattr(out, name, values)
    out.validate()
    return out


def _regime(spec: Mapping[str, str], source: str) -> tuple[str, str, str, str]:
    """Resolve `(key, tone, badge, gloss)` or refuse.

    The refusal is the point of this function. An unlabelled dispersion headline
    is the specific failure this report exists to prevent, so a track with no
    regime is not renderable — not rendered with a caveat, not rendered greyed
    out. An UNRECOGNISED regime string is still rendered, on the neutral
    "unknown" tone with its raw text as the badge: a new engine-side regime name
    must not black out a report, but it must not borrow another state's colour
    either.
    """
    key = str(spec.get(REGIME_KEY, "")).strip()
    if not key:
        # A CODED error, like the rest of this surface. The refusal is a contract
        # violation by the caller, and everywhere else in this layer that is an
        # `AtxError` carrying `ErrorCode.INVALID_ARGUMENT` — which is PY-1's
        # whole point. A caller wrapping the pipeline in `except av.AtxError`
        # used to miss this one because it was a bare `ValueError`.
        error = _av.AtxError(
            f"{source} carries no `{REGIME_KEY}`. Refusing to render: a dispersion "
            "headline is meaningless without the execution regime that produced "
            "it — on the pinned run the same strategy returns +24740.62 "
            "frictionless and -6460.23 once impact is added, so the sign itself is "
            "a function of the regime. Re-run with a build that emits "
            f"`{REGIME_KEY}` (write_dispersion_tearsheet), or pass it in the "
            "metadata mapping."
        )
        error.code = _av.ErrorCode.INVALID_ARGUMENT
        raise error
    tone, badge, gloss = REGIMES.get(key, ("unknown", key.upper(), ""))
    return key, tone, badge, gloss


# The masthead this renderer was written for: the SPY listed-options dispersion
# proxy. It stays the DEFAULT so every existing caller renders byte-identically,
# but it is no longer the only thing this file can say — see `build_report`.
DEFAULT_TITLE = "SPY Listed-Options Dispersion"
DEFAULT_EYEBROW = "atx-vol · surface replay · traditional dispersion proxy"
DEFAULT_STANDFIRST = (
    "A short SPY at-the-money straddle against long constituent ATM straddles, "
    "sized vega-flat at entry on served American vegas, delta-hedged daily at the "
    "close and rolled on a common listed monthly expiry. Prices and Greeks are "
    "replayed from cached atx-vol American fitted surfaces — no re-fit occurs "
    "inside the backtest."
)


def _attribution_caption(sheet) -> str:
    """Describe the attribution this run ACTUALLY produced, not a remembered one.

    The caption used to assert, unconditionally, that "long gamma and long vega
    are where the alpha sits, theta is the carry paid for it". That is the SPY
    proxy's signature and it is a claim about numbers — on a run whose gamma
    attribution is negative the same page's own table refutes its caption, which
    is precisely the read-a-figure-without-its-assumptions failure this renderer
    exists to prevent. So the sentence is now derived from the signs it is
    describing.
    """
    def side(value: float, name: str) -> str:
        if value > 0:
            return f"{name} contributes"
        if value < 0:
            return f"{name} costs"
        return f"{name} is flat"

    return (
        f"The dispersion signature for this run: {side(sheet.attr_gamma, 'gamma')}, "
        f"{side(sheet.attr_vega, 'vega')}, {side(sheet.attr_theta, 'theta')}, and delta is "
        "hedge slippage — small because the book is re-flattened at every close."
    )


def _render(result, sheet, spec: Mapping[str, str], counters: Mapping[str, str],
            path: str, label: str, *, columns_present: frozenset[str] | None = None,
            source: str = "in-memory BacktestResult",
            title: str | None = None, eyebrow: str | None = None,
            standfirst: str | None = None) -> str:
    # Before anything is computed, let alone written: no regime, no report.
    regime, regime_tone, regime_badge, regime_gloss = _regime(spec, label or "this run")
    # The short tag repeated on every headline tile, so no number can be read
    # without the assumptions that produced it — including in a cropped
    # screenshot of one tile.
    tag = f"regime: {regime}"

    cols = result.to_dict()
    present = columns_present if columns_present is not None else frozenset(cols)
    missing_optional = sorted(OPTIONAL_REPORT_COLUMNS - present)
    dates = list(cols["date"])
    ticks = _short(dates)
    nav = [float(v) for v in cols["nav"]]
    daily = [float(v) for v in cols["pnl_total"]]
    n = len(dates)

    total = sheet.total_return
    tone = "pos" if total > 0 else "neg" if total < 0 else ""
    # Cost comes from the library's own fold, never re-derived here.
    cost = sheet.attr_cost
    gross = total + cost
    date_lo = spec.get("date_lo", dates[0] if dates else "?")
    date_hi = spec.get("date_hi", dates[-1] if dates else "?")

    def num(key: str, default: float = 0.0) -> float:
        try:
            return float(spec[key])
        except (KeyError, ValueError):
            return default
    delta_band = num("delta_band", 0)
    delta_band_text = f"{delta_band:.10g}"

    report = Report(
        # All three are TEXT (components.Report escapes them), so a
        # caller-supplied masthead cannot inject markup into the document.
        title=title if title is not None else DEFAULT_TITLE,
        eyebrow=eyebrow if eyebrow is not None else DEFAULT_EYEBROW,
        standfirst=standfirst if standfirst is not None else DEFAULT_STANDFIRST,
        meta=(
            ("Regime", regime),
            ("Window", f"{date_lo} → {date_hi}"),
            ("Sessions", f"{n}"),
            ("Target DTE", f"{num('target_dte_days', 30):.0f}d "
                           f"({num('min_dte_days', 21):.0f}–{num('max_dte_days', 60):.0f})"),
            ("Roll", f"{num('roll_dte_days', 7):.0f}d to expiry"),
            # FIX-5/M6, mirroring FIX-E M-11 on tools/spy_dispersion_tearsheet_report.py:176:
            # `gross_index_vega` is dollars per ONE VOL POINT after E1/AN-P1-1 rescaled it
            # by 100x. Printing it bare invites reading it as dollars per unit vol — which
            # is exactly what it meant on this route BEFORE E1, so the ambiguity is a live
            # 100x. E1 landed after this renderer was written and could not reach it.
            ("Index vega", f"{_money(num('gross_index_vega', 10000))}/vol pt"),
            ("Delta band", delta_band_text),
        ),
        colophon=(
            # The colophon is a raw-markup channel by design (components.Report
            # joins the lines verbatim), so every value read out of a run artifact
            # has to be escaped on the way in: a '<' in a path, label or spec value
            # otherwise corrupts — or injects into — the document.
            f"<b>Run</b> {esc(label)}" if label else "<b>Run</b> spy_dispersion_backtest",
            f"<b>Source</b> {esc(source)}",
            # The weight-coverage clause is OMITTED when the run has no such
            # floor, rather than printed as "≥ ?" — an unfilled placeholder reads
            # as a broken template, and a sizing rule that is not weight-based
            # (a per-name theta budget, say) has no value to put there.
            f"<b>Data</b> {esc(spec.get('opra_root', 'OPRA'))} · flat rate "
            f"{num('flat_rate', 0.043):.3f} · min names {esc(spec.get('min_names', '?'))}"
            + (f" · weight coverage ≥ {esc(spec['min_weight_coverage'])}"
               if spec.get("min_weight_coverage") else ""),
            "<b>Report</b> rendered by atxvol.report from the engine's persisted series — "
            "metrics folded by atx-vol's own tearsheet",
        ),
    )

    # ── The regime banner — full width, before any number ───────────────────
    share = f"{abs(cost) / abs(gross) * 100.0:.0f}% of gross" if abs(gross) > 1e-12 else ""
    report.add(Banner(
        badge=regime_badge,
        detail=" · ".join(p for p in (spec.get(DETAIL_KEY, ""), regime_gloss) if p),
        aside=f"cost {_money(cost, 2)}" + (f"  =  {share}" if share else ""),
        tone=regime_tone,
    ))
    if missing_optional:
        report.add(Banner(
            badge="PARTIAL LOOSE TSV",
            detail=(
                "Unavailable optional columns: " + ", ".join(missing_optional)
                + ". Related panels and invariant values are omitted, not treated as zero."
            ),
            tone="warn",
        ))

    # ── 01 Headline ─────────────────────────────────────────────────────────
    up = sum(1 for v in daily[1:] if v > 0)
    down = sum(1 for v in daily[1:] if v < 0)
    report.add(Section(
        "Headline",
        lede=(
            "Every figure below is folded by the atx-vol tearsheet from the engine's own "
            "persisted series; the report layer formats, it does not compute. Every tile "
            "is captioned "
            "with the execution regime, because on this strategy the headline is "
            "friction-dominated and its sign is a function of the regime."
        ),
        body=[
            StatRow([
                Stat("Net return (after cost)", _money(total), tag, tone),
                Stat("Gross return (pre-cost)", _money(gross), "frictionless equivalent"),
                Stat("Cost drag", _money(-abs(cost)), tag, "neg" if cost > 0 else ""),
                Stat("Sharpe", f"{sheet.sharpe:.2f}", tag),
                Stat("Max drawdown", _money(sheet.max_drawdown), tag),
                Stat("Hit rate", f"{sheet.hit_rate * 100:.0f}%",
                     f"{up} up / {down} down · {tag}"),
                Stat("Return on vega", f"{sheet.return_on_gross_vega:.4f}", tag),
                Stat("Annualized vol", _money(sheet.ann_vol), f"of the $ P&L series · {tag}"),
            ]),
        ],
    ))

    # ── 02 The track ────────────────────────────────────────────────────────
    nav_rows = [
        (dates[i], _money(daily[i], 2), _money(nav[i], 2),
         _money(float(cols["pnl_gamma"][i]), 2), _money(float(cols["pnl_vega"][i]), 2),
         _money(float(cols["pnl_theta"][i]), 2), f"{float(cols['gross_vega'][i]):,.0f}")
        for i in range(n)
    ]
    report.add(Section(
        "The track",
        body=[
            Figure(
                charts.line_chart(
                    [Series("Cumulative P&L", nav, color=theme.SERIES[0], area=True,
                            label_end=True)],
                    ticks, height=340, chart_id="nav",
                ),
                title=f"Cumulative P&L — {regime}",
                subtitle="Net asset value from inception, in dollars, under the regime "
                         "named in the title. One series — the title names it, so no "
                         "legend box.",
                caption=(
                    f"The book finishes at <b>{_money(total)}</b> over {n} sessions, "
                    f"peak-to-trough drawdown <b>{_money(sheet.max_drawdown)}</b>. "
                    "Hover for per-session values, or open the table below."
                ),
                table=Table(
                    [Column("Session", mono=True), Column("Daily", tone="sign"),
                     Column("Cumulative", tone="sign"), Column("Gamma", tone="sign"),
                     Column("Vega", tone="sign"), Column("Theta", tone="sign"),
                     Column("Gross vega")],
                    nav_rows, numbered=False,
                ),
                table_label=f"Show per-session values ({n} rows)",
            ),
            Figure(
                charts.bar_chart(daily, ticks, height=250, chart_id="daily"),
                title="Daily P&L",
                subtitle="Per-session change. Gains and losses are a diverging pair about "
                         "zero, not two categorical series.",
                legend=[("Gain", theme.POSITIVE), ("Loss", theme.NEGATIVE)],
                caption=(
                    f"{up} up sessions against {down} down. A dispersion book carries "
                    "negative theta every day and earns it back in bursts, so the daily "
                    "series is asymmetric by construction."
                ),
            ),
        ],
    ))

    # ── 03 Attribution ──────────────────────────────────────────────────────
    axis_series, legend, attr_rows = [], [], []
    for name, key, slot, label_end in AXES:
        if key not in cols:
            continue
        color = theme.series_color(slot)
        cumulative = _cum([float(v) for v in cols[key]])
        axis_series.append(Series(name, cumulative, color=color, label_end=label_end))
        legend.append((name, color))
        attr_rows.append((name, _money(cumulative[-1], 2),
                          f"{(cumulative[-1] / total * 100) if total else 0:+.0f}%"))

    closure = (sheet.attr_delta + sheet.attr_gamma + sheet.attr_vega + sheet.attr_vanna
               + sheet.attr_volga + sheet.attr_theta + sheet.attr_rho + sheet.attr_charm
               + sheet.attr_unexplained + sheet.attr_settlement + sheet.attr_shares
               + sheet.attr_financing - sheet.attr_cost)
    residual = abs(total - closure)

    report.add(Section(
        "Attribution",
        lede=(
            "Each session's P&L is decomposed onto the Greek axes plus settlement, share "
            "and financing terms. The decomposition is exact by construction: the axes "
            "must sum to the total return."
        ),
        body=[
            Figure(
                charts.line_chart(axis_series, ticks, height=340, chart_id="attr"),
                title="Cumulative attribution by axis",
                subtitle="Running sum of each axis, in dollars, on one shared scale.",
                legend=legend,
                caption=_attribution_caption(sheet),
                table=Table(
                    [Column("Axis"), Column("Cumulative $", tone="sign"),
                     Column("Share of total")],
                    attr_rows, numbered=False,
                ),
            ),
            Table(
                [Column("Component"), Column("Total $", tone="sign")],
                [("Delta", _money(sheet.attr_delta, 2)),
                 ("Gamma", _money(sheet.attr_gamma, 2)),
                 ("Vega", _money(sheet.attr_vega, 2)),
                 ("Vanna", _money(sheet.attr_vanna, 2)),
                 ("Volga", _money(sheet.attr_volga, 2)),
                 ("Theta", _money(sheet.attr_theta, 2)),
                 ("Rho", _money(sheet.attr_rho, 2)),
                 ("Charm", _money(sheet.attr_charm, 2)),
                 ("Unexplained", _money(sheet.attr_unexplained, 2)),
                 ("Settlement", _money(sheet.attr_settlement, 2)),
                 ("Shares", _money(sheet.attr_shares, 2)),
                 ("Financing", _money(sheet.attr_financing, 2)),
                 ("Cost", _money(-sheet.attr_cost, 2))],
                caption="Attribution totals over the whole run, summed across all rows.",
                footer=("Sum of axes", _money(closure, 2)),
            ),
            Note(
                "<b>Closure identity.</b> The axes sum to "
                f"<span class='mono'>{_money(closure, 6)}</span> against a total return of "
                f"<span class='mono'>{_money(total, 6)}</span> — residual "
                f"<span class='mono'>{residual:.3e}</span>. This is the same gate the C++ "
                "tearsheet test asserts, re-evaluated after loading the persisted series."
            ),
        ],
    ))

    # ── 04 Book risk ────────────────────────────────────────────────────────
    panels = []
    for name, key, slot, note in (
        ("Gross vega", "gross_vega", 2, "Post-constraint book vega"),
        ("Gross gamma", "gross_gamma", 1, "Book convexity"),
        ("Gross theta", "gross_theta", 0, "Daily carry, dollars"),
        ("Net delta", "gross_delta", 3, "After the daily close hedge"),
    ):
        if key in present:
            panels.append((
                name,
                charts.small_multiple([float(v) for v in cols[key]], ticks,
                                      color=theme.series_color(slot), chart_id=key),
                note,
            ))

    max_delta = (
        max((abs(float(v)) for v in cols["gross_delta"]), default=0.0)
        if "gross_delta" in present else None
    )
    peak_lots = (
        max((float(v) for v in cols["n_open_lots"]), default=0.0)
        if "n_open_lots" in present else None
    )
    unpriced = (
        sum(float(v) for v in cols["n_unpriced_lots"])
        if "n_unpriced_lots" in present else None
    )

    if max_delta is None:
        delta_caption = (
            f"Net delta is unavailable in this loose TSV. The configured per-underlying "
            f"close-hedge band is <b>{delta_band_text}</b>; breaches of that band trigger "
            "a trade back to zero."
        )
    else:
        delta_caption = (
            f"Peak recorded |net delta| is <b>{max_delta:.1e}</b> under the configured "
            f"per-underlying close-hedge band of <b>{delta_band_text}</b>; breaches of "
            "that band trigger a trade back to zero."
        )

    report.add(Section(
        "Book risk",
        lede=(
            "Four measures on four scales, faceted rather than overlaid — two y-scales on "
            "one plot would imply a relationship the data does not contain."
        ),
        body=[
            FacetGrid(
                panels, columns=2,
                title="Book greeks through the run",
                caption=(
                    delta_caption + " Gross vega is the constrained quantity — the "
                    "index leg is scaled so index and basket vega offset at entry."
                ),
            ),
            Subhead("Invariants observed"),
            Table(
                [Column("Invariant"), Column("Observed", mono=True), Column("Source")],
                [("Attribution closure", f"{residual:.3e}", "tearsheet vs sum of axes"),
                 ("Peak |net delta|", f"{max_delta:.3e}" if max_delta is not None else "--",
                  "gross_delta" if max_delta is not None else "unavailable"),
                 ("Peak open lots", f"{peak_lots:.0f}" if peak_lots is not None else "--",
                  "n_open_lots" if peak_lots is not None else "unavailable"),
                 ("Unpriced lots", f"{unpriced:.0f}" if unpriced is not None else "--",
                  "n_unpriced_lots" if unpriced is not None else "unavailable"),
                 ("Sessions", f"{n}", "recorded rows")],
                caption="Engine invariants, read back from the persisted series.",
            ),
        ],
    ))

    # ── 05 Configuration ────────────────────────────────────────────────────
    order = ("label", "date_lo", "date_hi", "snapshot_suffix", "opra_root", "path_template",
             "flat_rate", "min_names", "min_weight_coverage", "target_dte_days",
             "min_dte_days", "max_dte_days", "roll_dte_days", "gross_index_vega",
             "delta_band", "fit_workers", "core_mode")
    rows = [(k, spec[k]) for k in order if k in spec]
    rows += [(k, v) for k, v in spec.items() if k not in order]
    body = [Table([Column("Key", mono=True), Column("Value", mono=True)], rows,
                  caption="Run specification, as recorded by the backtest driver.")]
    if counters:
        body.append(Table(
            [Column("Counter", mono=True), Column("Value", mono=True)],
            list(counters.items())[:40],
            caption="Engine counters for the run.",
        ))
    report.add(Section(
        "Configuration",
        lede="The run's own specification file, reproduced verbatim.",
        body=body,
    ))

    return report.write(path)


# Backwards-compatible entry point for an in-memory run.
def build_report(result, sheet, meta: Mapping[str, str], path: str, *,
                 title: str | None = None, eyebrow: str | None = None,
                 standfirst: str | None = None) -> str:
    """Render a `BacktestResult` + `TearSheet` + metadata mapping.

    `meta` must carry `friction_regime`; this entry point is held to exactly the
    same contract as `build_report_from_run` (both go through `_render`, which
    refuses first). An in-memory caller is not a licence to publish an
    unqualified headline — it is the same number in the same document. The
    refusal is an `atxvol.AtxError` with `code == ErrorCode.INVALID_ARGUMENT`.

    THE MASTHEAD IS THE CALLER'S. `title` / `eyebrow` / `standfirst` default to
    the SPY listed-options proxy this file was written for, so every existing
    caller is byte-unchanged — but a caller running a DIFFERENT strategy must
    override them. This whole renderer is built on the rule that a reader must
    never meet a figure without the assumptions that produced it: it hard-refuses
    a track with no `friction_regime`, captions every tile with the regime and
    names it in the chart title. A masthead that describes someone else's
    strategy breaks that rule at the one line a reader is guaranteed to read, and
    a `<title>` is what a circulated HTML file is filed under.

    All three are escaped by `components.Report`, so they are text, not markup.
    """
    return _render(result, sheet, dict(meta), {}, path, meta.get("strategy", ""),
                   title=title, eyebrow=eyebrow, standfirst=standfirst)
