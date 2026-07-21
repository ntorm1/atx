"""Compare two dispersion backtest routes in one self-contained HTML report.

Two routes price the *same* frozen trade schedule and are then set side by side:

* **listed** — `run-backtest` over the schedule against listed OPRA marks
  (``backtest.tsv``);
* **projected** — the same schedule projected onto historical fitted surfaces at
  ATM-forward strikes and re-marked. The canonical projected file is the *cold*
  run (``projected_cold_backtest.tsv``, ``--execution cold``); the fast tier
  (``projected_backtest.tsv``) is diagnostic only — see the methodology appendix.

The report answers one question — *do the two P&L tracks agree* — with a
tracking-stat hero, an overlaid NAV chart plus residual, a y=x agreement
scatter, a per-axis attribution comparison, the mark-divergence distribution
that motivates using cold marks, and a methodology section.

    from atxvol.report.parity import build_parity_report
    build_parity_report("backtest.tsv", "projected_cold_backtest.tsv",
                        "mark_divergence.tsv", "trade_schedule.tsv", "parity.html")

This module is deliberately free of any compiled-binding import: it reads the
engine's TSVs with a small local reader and folds the comparison stats in pure
Python, so a report renders without the ``_atxvol`` extension being built. (The
library's ``io.read_backtest_tsv`` is not reused here because it constructs a
binding ``BacktestResult``; see controller amendment 7.)
"""

from __future__ import annotations

import datetime as _dt
import math
import re
import statistics
from dataclasses import dataclass
from typing import Sequence

from . import charts, theme
from .charts import Series
from .components import (
    Column, Figure, Note, Prose, Report, Section, Stat, StatRow, Subhead, Table,
)

__all__ = ["ParityStats", "compute_parity_stats", "build_parity_report"]

# Colour follows route identity, not chart or rank: the listed route is always
# the first validated palette slot and the projected route the second, in every
# figure and in both light and dark rendering. Reusing these two constants is
# what keeps a route the same hue across the NAV overlay and the attribution
# bars. The residual and scatter are derived series (neither route), so they use
# a third, deliberately distinct slot / a neutral ink.
LISTED_COLOR = theme.SERIES[0]      # slate blue
PROJECTED_COLOR = theme.SERIES[1]   # ochre
DERIVED_COLOR = theme.SERIES[4]     # plum — the NAV residual, which is neither route

# The four attribution axes the comparison foregrounds. Display order is palette
# order; colour follows the *route*, so the two bars in a cluster are (listed,
# projected) regardless of which is larger.
ATTR_AXES = (("Gamma", "pnl_gamma"), ("Vega", "pnl_vega"),
             ("Theta", "pnl_theta"), ("Unexplained", "pnl_unexplained"))

_MAGIC_RE = re.compile(r"^ATX_[A-Z0-9_]+$")
_MINUS = "−"  # typographic minus, for "listed − projected"


# ── TSV reading ──────────────────────────────────────────────────────────────

def _read_tsv(path: str) -> tuple[list[str], list[dict[str, str]]]:
    """Read a TSV the C++ tools emit.

    Skips a leading magic sentinel line (``ATX_… <version>``, present on the
    schedule but not the backtest files) and any ``#`` comment lines; the first
    remaining line is the column header. Returns ``(header, rows)`` where each
    row is a ``{column: value}`` mapping.
    """
    header: list[str] | None = None
    rows: list[dict[str, str]] = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if header is None and _MAGIC_RE.match(fields[0]):
                continue  # magic sentinel line, e.g. ATX_LISTED_DISPERSION_SCHEDULE
            if header is None:
                header = fields
                continue
            rows.append(dict(zip(header, fields)))
    if header is None:
        raise ValueError(f"{path}: no column header row found")
    return header, rows


# ── Stats ────────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class ParityStats:
    """The three headline tracking numbers, plus the session count.

    ``tracking_error`` is the *population* standard deviation of the per-session
    P&L differences (listed − projected); ``corr`` is the Pearson correlation of
    the two daily P&L series; ``max_abs_nav_gap`` is the peak absolute gap
    between the two cumulative P&L (NAV) tracks over the window.
    """

    corr: float
    tracking_error: float
    max_abs_nav_gap: float
    n_sessions: int


def compute_parity_stats(
    listed_pnl: Sequence[float],
    projected_pnl: Sequence[float],
    listed_nav: Sequence[float],
    projected_nav: Sequence[float],
) -> ParityStats:
    """Fold two aligned daily-P&L / NAV pairs into :class:`ParityStats`.

    Inputs are expected to be aligned session-for-session and equal length. A
    series with fewer than two points or no variance has no defined correlation,
    which is reported as ``nan`` rather than raising.
    """
    diffs = [a - b for a, b in zip(listed_pnl, projected_pnl)]
    tracking_error = statistics.pstdev(diffs) if diffs else float("nan")
    try:
        corr = statistics.correlation(list(listed_pnl), list(projected_pnl))
    except (statistics.StatisticsError, ValueError):
        corr = float("nan")
    gaps = [abs(a - b) for a, b in zip(listed_nav, projected_nav)]
    max_abs_nav_gap = max(gaps) if gaps else float("nan")
    return ParityStats(corr, tracking_error, max_abs_nav_gap, len(diffs))


def _ols(xs: Sequence[float], ys: Sequence[float]) -> tuple[float, float] | None:
    """OLS fit of ``ys`` on ``xs`` as ``(slope, intercept)``; ``None`` if undefined."""
    try:
        line = statistics.linear_regression(list(xs), list(ys))
        return (line.slope, line.intercept)
    except (statistics.StatisticsError, ValueError):
        return None


# ── Formatting ───────────────────────────────────────────────────────────────

def _money(v: float, dp: int = 0) -> str:
    if v is None or not math.isfinite(v):
        return "--"
    return f"{'-' if v < 0 else ''}${abs(v):,.{dp}f}"


def _short(dates: Sequence[str]) -> list[str]:
    return [d[5:] if len(d) == 10 else d for d in dates]


def _ns_to_date(ns: str) -> str:
    try:
        secs = int(float(ns)) / 1e9
        return _dt.datetime.fromtimestamp(secs, tz=_dt.timezone.utc).strftime("%Y-%m-%d")
    except (ValueError, OverflowError, OSError):
        return "?"


# ── Builder ──────────────────────────────────────────────────────────────────

def build_parity_report(
    listed_tsv: str,
    projected_tsv: str,
    mark_divergence_tsv: str,
    schedule_tsv: str,
    out_html: str,
    *,
    projected_label: str = "projected (cold)",
    listed_diagnostics_tsv: str | None = None,
    projected_diagnostics_tsv: str | None = None,
    schedule_diagnostics_tsv: str | None = None,
) -> ParityStats:
    """Render the two-route comparison to ``out_html`` and return its stats.

    ``projected_tsv`` may be either the canonical cold run or the diagnostic
    fast-tier run; ``projected_label`` names it in every legend and caption.

    The three optional ``*_diagnostics_tsv`` arguments are the per-subcommand
    ``diagnostics_<subcommand>.tsv`` phase-timing files the example emits
    (``run-backtest``, ``run-projected-backtest``, ``project-schedule``
    respectively). When any is supplied a "Runtime diagnostics" section is
    rendered between mark divergence and methodology; when all are ``None`` the
    document is byte-for-byte what it was before (existing callers unaffected).
    The returned :class:`ParityStats` never depends on the diagnostics inputs.
    """
    _, listed_rows = _read_tsv(listed_tsv)
    _, projected_rows = _read_tsv(projected_tsv)
    _, divergence_rows = _read_tsv(mark_divergence_tsv)
    _, schedule_rows = _read_tsv(schedule_tsv)

    diag_listed = _read_tsv(listed_diagnostics_tsv)[1] if listed_diagnostics_tsv else None
    diag_projected = _read_tsv(projected_diagnostics_tsv)[1] if projected_diagnostics_tsv else None
    diag_schedule = _read_tsv(schedule_diagnostics_tsv)[1] if schedule_diagnostics_tsv else None

    # Align session-for-session on date (listed order wins).
    proj_by_date = {r["date"]: r for r in projected_rows}
    aligned = [(l, proj_by_date[l["date"]]) for l in listed_rows if l["date"] in proj_by_date]
    dates = [l["date"] for l, _ in aligned]
    ticks = _short(dates)

    l_daily = [float(l["pnl_total"]) for l, _ in aligned]
    p_daily = [float(p["pnl_total"]) for _, p in aligned]
    l_nav = [float(l["nav"]) for l, _ in aligned]
    p_nav = [float(p["nav"]) for _, p in aligned]
    residual = [a - b for a, b in zip(l_nav, p_nav)]

    stats = compute_parity_stats(l_daily, p_daily, l_nav, p_nav)

    report = _assemble(
        projected_label, dates, ticks, l_daily, p_daily, l_nav, p_nav, residual,
        aligned, divergence_rows, schedule_rows, stats,
        diag_listed, diag_projected, diag_schedule,
    )
    report.write(out_html)
    return stats


def _assemble(projected_label, dates, ticks, l_daily, p_daily, l_nav, p_nav,
              residual, aligned, divergence_rows, schedule_rows, stats,
              diag_listed=None, diag_projected=None, diag_schedule=None) -> Report:
    n = len(dates)
    date_lo = dates[0] if dates else "?"
    date_hi = dates[-1] if dates else "?"

    # Schedule-derived coverage.
    names = sorted({r["symbol"] for r in schedule_rows if r.get("is_index") == "0"})
    index_sym = next((r["symbol"] for r in schedule_rows if r.get("is_index") == "1"), "?")
    n_names = schedule_rows[0].get("n_names", str(len(names))) if schedule_rows else "?"
    index_vega = schedule_rows[0].get("gross_index_vega_target", "?") if schedule_rows else "?"
    expiry = _ns_to_date(schedule_rows[0]["expiry_ts_ns"]) if schedule_rows else "?"

    report = Report(
        title="Dispersion: Listed vs Projected",
        eyebrow="atx-vol · dispersion two-route parity · listed OPRA vs surface projection",
        standfirst=(
            "One frozen trade schedule, priced two ways: against listed OPRA marks and "
            "against the same legs projected onto historical fitted surfaces at ATM-forward "
            "strikes with cold marks. This report sets the two P&amp;L tracks side by side — "
            "how tightly they agree, where they part, and why the projected route is marked "
            "cold rather than fast."
        ),
        meta=(
            ("Window", f"{date_lo} → {date_hi}"),
            ("Sessions", f"{n}"),
            ("Index", index_sym),
            ("Names", f"{n_names}"),
            ("Expiry", expiry),
            ("Projected route", projected_label),
        ),
        colophon=(
            "<b>Routes</b> listed = run-backtest over the schedule vs listed OPRA marks · "
            f"projected = project-schedule (ATM-forward) + re-mark, {projected_label}",
            "<b>Inputs</b> backtest.tsv · projected_*_backtest.tsv · mark_divergence.tsv · "
            "trade_schedule.tsv",
            "<b>Report</b> rendered by atxvol.report.parity — stats folded in pure Python "
            "from the engine's TSVs (no binding, no re-fit)",
        ),
    )

    _add_hero(report, projected_label, stats, l_nav, p_nav, n)
    _add_nav_tracks(report, projected_label, ticks, l_nav, p_nav, residual, dates, stats)
    _add_scatter(report, projected_label, ticks, l_daily, p_daily, dates)
    _add_attribution(report, projected_label, aligned)
    _add_divergence(report, divergence_rows)
    if diag_listed or diag_projected or diag_schedule:
        _add_diagnostics(report, diag_listed, diag_projected, diag_schedule)
    _add_methodology(report, projected_label, date_lo, date_hi, n, index_sym, names,
                     n_names, index_vega, expiry)
    return report


def _add_hero(report, projected_label, stats, l_nav, p_nav, n) -> None:
    corr_txt = f"{stats.corr:.3f}" if math.isfinite(stats.corr) else "--"
    report.add(Section(
        "Tracking summary",
        lede=(
            "Three numbers say whether the two routes tell the same story: how correlated "
            "their daily P&amp;L is, how far the per-session difference typically strays, and "
            "the widest the cumulative tracks ever drift apart."
        ),
        body=[
            StatRow([
                Stat("Correlation", corr_txt,
                     "Pearson r of the two routes' daily P&L",
                     "pos" if math.isfinite(stats.corr) and stats.corr > 0 else ""),
                Stat("Tracking error", _money(stats.tracking_error),
                     "std dev of daily P&L differences (listed − projected)"),
                Stat("Max |NAV gap|", _money(stats.max_abs_nav_gap),
                     "peak |cumulative P&L gap| over the window"),
                Stat("Sessions", f"{n}", "aligned session-for-session on date"),
                Stat("Listed final", _money(l_nav[-1] if l_nav else float("nan")),
                     "cumulative P&L, listed route",
                     "pos" if l_nav and l_nav[-1] > 0 else "neg" if l_nav and l_nav[-1] < 0 else ""),
                Stat(f"{projected_label.capitalize()} final",
                     _money(p_nav[-1] if p_nav else float("nan")),
                     "cumulative P&L, projected route",
                     "pos" if p_nav and p_nav[-1] > 0 else "neg" if p_nav and p_nav[-1] < 0 else ""),
            ]),
        ],
    ))


def _add_nav_tracks(report, projected_label, ticks, l_nav, p_nav, residual,
                    dates, stats) -> None:
    overlay = charts.line_chart(
        [Series("listed", l_nav, color=LISTED_COLOR, label_end=True),
         Series(projected_label, p_nav, color=PROJECTED_COLOR, label_end=True)],
        ticks, height=340, chart_id="nav",
    )
    resid = charts.line_chart(
        [Series(f"NAV gap (listed {_MINUS} projected)", residual, color=DERIVED_COLOR,
                area=True, label_end=True)],
        ticks, height=230, chart_id="resid",
    )
    nav_rows = [
        (dates[i], _money(l_nav[i], 2), _money(p_nav[i], 2), _money(residual[i], 2))
        for i in range(len(dates))
    ]
    report.add(Section(
        "NAV tracks",
        lede=(
            "Both cumulative P&amp;L curves on one shared dollar scale, with the residual — "
            "their per-session gap — broken out below so a drift that is invisible against "
            "the level is legible against zero."
        ),
        body=[
            Figure(
                overlay,
                title="Cumulative P&L — listed vs projected",
                subtitle="Net asset value from inception, in dollars, both routes on one scale.",
                legend=[("listed", LISTED_COLOR), (projected_label, PROJECTED_COLOR)],
                caption=(
                    f"The tracks part by at most <b>{_money(stats.max_abs_nav_gap)}</b> "
                    "over the window. Hover for per-session values, or open the table."
                ),
                table=Table(
                    [Column("Session", mono=True), Column("Listed", tone="sign"),
                     Column("Projected", tone="sign"), Column("Gap", tone="sign")],
                    nav_rows, numbered=False,
                ),
                table_label=f"Show per-session NAV ({len(dates)} rows)",
            ),
            Figure(
                resid,
                title="NAV residual",
                subtitle=f"Listed {_MINUS} projected cumulative P&L, per session. A flat line "
                         "at zero would be perfect agreement.",
                caption=(
                    "The residual is the cumulative disagreement between the routes; its "
                    "peak magnitude is the Max |NAV gap| headline stat."
                ),
            ),
        ],
    ))


def _add_scatter(report, projected_label, ticks, l_daily, p_daily, dates) -> None:
    fit = _ols(l_daily, p_daily)
    scatter = charts.scatter_chart(
        l_daily, p_daily,
        x_title="Listed daily P&L", y_title=f"{projected_label} daily P&L",
        point_labels=ticks, color=theme.INK_2, fit=fit, chart_id="agree",
        width=560, height=460,
    )
    day_rows = [
        (dates[i], _money(l_daily[i], 2), _money(p_daily[i], 2),
         _money(l_daily[i] - p_daily[i], 2))
        for i in range(len(dates))
    ]
    slope_txt = f"{fit[0]:.3f}" if fit else "--"
    report.add(Section(
        "Daily P&L agreement",
        lede=(
            "Each session is one point: listed P&amp;L on the x-axis, projected on the y. On "
            "a shared scale with the y=x diagonal drawn, agreement is proximity to the "
            "dashed line — not a tight fit on independent axes, which can hide a bias."
        ),
        body=[
            Figure(
                scatter,
                title="Daily P&L — projected vs listed",
                subtitle="One point per session, shared scale, y=x reference dashed; the solid "
                         "line is the OLS fit of projected on listed.",
                caption=(
                    f"OLS slope is <b>{slope_txt}</b> (1.0 would be unbiased agreement in "
                    "scale). Points above the diagonal are sessions the projected route "
                    "marked richer than listed, below are the reverse."
                ),
                table=Table(
                    [Column("Session", mono=True), Column("Listed", tone="sign"),
                     Column("Projected", tone="sign"),
                     Column(f"Listed {_MINUS} proj.", tone="sign")],
                    day_rows, numbered=False,
                ),
                table_label=f"Show per-session daily P&L ({len(dates)} rows)",
            ),
        ],
    ))


def _add_attribution(report, projected_label, aligned) -> None:
    listed_rows = [l for l, _ in aligned]
    proj_rows = [p for _, p in aligned]
    categories = [name for name, _ in ATTR_AXES]
    listed_cum = [sum(float(r[key]) for r in listed_rows) for _, key in ATTR_AXES]
    proj_cum = [sum(float(r[key]) for r in proj_rows) for _, key in ATTR_AXES]

    bars = charts.paired_bar_chart(
        categories,
        [Series("listed", listed_cum, color=LISTED_COLOR),
         Series(projected_label, proj_cum, color=PROJECTED_COLOR)],
        height=320, chart_id="attr",
    )
    attr_rows = [
        (categories[i], _money(listed_cum[i], 2), _money(proj_cum[i], 2),
         _money(listed_cum[i] - proj_cum[i], 2))
        for i in range(len(categories))
    ]
    report.add(Section(
        "Attribution parity",
        lede=(
            "Where the two routes agree or diverge, axis by axis: each cluster is one Greek "
            "axis summed over the window, with the listed and projected bars side by side. "
            "Colour follows the route, so a taller projected bar never repaints as listed."
        ),
        body=[
            Figure(
                bars,
                title="Cumulative attribution by axis — listed vs projected",
                subtitle="Running-total P&L on each axis over the window, in dollars.",
                legend=[("listed", LISTED_COLOR), (projected_label, PROJECTED_COLOR)],
                caption=(
                    "Gamma and vega are where the dispersion alpha sits and theta is the "
                    "carry; the two routes should agree closely on all four once the "
                    "projected route is marked cold."
                ),
                table=Table(
                    [Column("Axis"), Column("Listed $", tone="sign"),
                     Column("Projected $", tone="sign"), Column("Difference $", tone="sign")],
                    attr_rows, numbered=False,
                ),
            ),
            Note(
                "The <b>unexplained</b> axis is the residual of the Greek decomposition, not "
                "a distinct risk. A route that agrees on gamma/vega/theta but not on "
                "unexplained is repricing second-order terms the first-order axes miss."
            ),
        ],
    ))


def _add_divergence(report, divergence_rows) -> None:
    def bps(r):
        try:
            return abs(float(r["abs_diff_bps_of_mark"]))
        except (KeyError, ValueError):
            return float("nan")

    ranked = sorted(
        (r for r in divergence_rows if math.isfinite(bps(r))),
        key=bps, reverse=True,
    )
    top = ranked[:12]
    dist = charts.bar_chart(
        [bps(r) for r in top],
        [f"{r['symbol']} {r['side'][:1]}" for r in top],
        height=280, unit="", positive=theme.ACCENT, negative=theme.ACCENT,
        chart_id="div",
    )
    offenders = ranked[:10]
    off_rows = [
        (r["symbol"], r["side"], r["strike"],
         f"{float(r['schedule_mark']):,.4f}", f"{float(r['live_mark']):,.4f}",
         _money(float(r["diff"]), 4), f"{bps(r):,.2f}")
        for r in offenders
    ]
    worst = ranked[0] if ranked else None
    worst_line = ""
    if worst is not None:
        worst_line = (
            f"The worst offender is <b>{worst['symbol']} {worst['side']}</b> at "
            f"<b>{bps(worst):,.0f} bps</b> — schedule mark "
            f"{float(worst['schedule_mark']):,.4f} vs live {float(worst['live_mark']):,.4f}. "
        )
    report.add(Section(
        "Mark divergence",
        lede=(
            "Per-contract gap between the schedule's frozen mark and a live re-mark, in "
            "basis points of the mark. This is the diagnostic that decides which projected "
            "marks are trustworthy — and it is why the canonical projected route is cold."
        ),
        body=[
            Figure(
                dist,
                title="Mark divergence by contract",
                subtitle="Absolute gap, basis points of the schedule mark, worst contracts "
                         "first. The scale is dominated by a single outlier by design.",
                caption=(
                    worst_line
                    + "That single spike is the RepresentativeFast early-exercise defect "
                    "(see methodology); every other contract is orders of magnitude tighter."
                ),
                table=Table(
                    [Column("Symbol", mono=True), Column("Side"), Column("Strike"),
                     Column("Schedule"), Column("Live"), Column("Diff", tone="sign"),
                     Column("|Diff| bps")],
                    off_rows, numbered=False,
                ),
                table_label=f"Show worst offenders ({len(offenders)} rows)",
            ),
        ],
    ))


def _phase_count(row) -> int:
    try:
        return int(row["count"])
    except (KeyError, ValueError):
        return 0


def _diag_table(rows) -> Table:
    """A per-route phase table: phase, wall ms, unit count, ms per unit.

    ``ms / unit`` is the phase's wall time divided by its unit count; it is left
    blank (an em dash) for phases whose count is 0 (n/a), so a rate is only ever
    shown where dividing is meaningful.
    """
    body = []
    for row in rows:
        wall = _num(row.get("wall_ms", ""))
        count = _phase_count(row)
        per = wall / count if count > 0 else None
        body.append((
            row.get("phase", "?"),
            f"{wall:,.3f}",
            f"{count:,}" if count > 0 else "—",
            f"{per:,.3f}" if per is not None else "—",
        ))
    return Table(
        [Column("Phase", mono=True), Column("Wall (ms)"), Column("Count"),
         Column("ms / unit")],
        body, numbered=False,
    )


def _phase(rows, name):
    return next((r for r in rows if r.get("phase") == name), None)


def _add_diagnostics(report, diag_listed, diag_projected, diag_schedule) -> None:
    body: list = []

    # Hero: the projected route's throughput, so the ms/session claim is
    # self-documenting from the artifact rather than shell `time` folklore.
    total = _phase(diag_projected, "total") if diag_projected else None
    if total is not None:
        total_ms = _num(total.get("wall_ms", ""))
        sessions = _phase_count(total)
        per = total_ms / sessions if sessions > 0 else float("nan")
        body.append(StatRow([
            Stat("Projected route wall", f"{total_ms:,.1f} ms",
                 "run-projected-backtest, end to end"),
            Stat("Priced sessions", f"{sessions:,}", "one re-mark per session"),
            Stat("Per session", f"{per:,.2f} ms" if math.isfinite(per) else "--",
                 "wall time ÷ sessions"),
        ]))

    # One phase table per supplied route, in execution order (schedule builder,
    # then the two priced routes).
    for label, rows in (
        ("Route P builder — project-schedule", diag_schedule),
        ("Listed route — run-backtest", diag_listed),
        ("Projected route — run-projected-backtest", diag_projected),
    ):
        if not rows:
            continue
        body.append(Subhead(label))
        body.append(_diag_table(rows))

    # Surface the reconciliation-dominates claim when the listed route is present.
    recon = _phase(diag_listed, "reconciliation") if diag_listed else None
    listed_total = _phase(diag_listed, "total") if diag_listed else None
    if recon is not None and listed_total is not None:
        denom = _num(listed_total.get("wall_ms", ""))
        share = (_num(recon.get("wall_ms", "")) / denom * 100.0) if denom > 0 else float("nan")
        if math.isfinite(share):
            body.append(Note(
                "The listed route's wall time is dominated by <b>reconciliation</b> — the "
                "OPRA parquet join that re-marks every session against the exchange tape — "
                f"at <b>{share:.0f}%</b> of the subcommand. That join is the cost the two-route "
                "design isolates behind the frozen schedule; the projected route replaces it "
                "with surface re-marks."
            ))

    report.add(Section(
        "Runtime diagnostics",
        lede=(
            "Phase-level wall time for each route, taken on a monotonic clock and written next "
            "to the run artifacts, so the performance claims are provable from the run itself "
            "rather than shell timing. Each phase carries its own unit count (sessions, rolls, "
            "legs, or archive loads); <span class='mono'>ms / unit</span> is that phase's wall "
            "time divided by its count, shown only where a count applies."
        ),
        body=body,
    ))


def _add_methodology(report, projected_label, date_lo, date_hi, n, index_sym,
                     names, n_names, index_vega, expiry) -> None:
    report.add(Section(
        "Methodology",
        lede="Routes, policies, the unit conventions the columns carry, and the fast-tier "
             "defect that motivates cold marks.",
        body=[
            Subhead("The two routes"),
            Prose([
                "<b>Listed.</b> The frozen <span class='mono'>trade_schedule.tsv</span> is "
                "run through <span class='mono'>run-backtest</span> against listed OPRA "
                "marks — the same instruments, priced from the exchange tape.",
                f"<b>Projected ({projected_label}).</b> The identical schedule is projected "
                "onto historical fitted surfaces at ATM-forward strikes and re-marked. The "
                "canonical projected route uses <span class='mono'>--execution cold</span> "
                "(cold marks); the fast tier is diagnostic only.",
            ]),
            Subhead("Policies & coverage"),
            Table(
                [Column("Property"), Column("Value", mono=True)],
                [("Window", f"{date_lo} → {date_hi}"),
                 ("Sessions", f"{n}"),
                 ("Index leg", index_sym),
                 ("Constituent names", f"{n_names} ({', '.join(names)})" if names else n_names),
                 ("Common expiry", expiry),
                 ("Gross index vega target", _money(_num(index_vega))),
                 ("Sizing", "vega-flat: index leg scaled so basket and index vega offset at entry"),
                 ("Hedge", "delta-to-zero at the daily close (zero band)")],
                caption="Both routes share this schedule and these policies; only the marking source differs.",
            ),
            Note(
                "<b>Units — the gross_* misnomer (D7).</b> The TSV columns named "
                "<span class='mono'>gross_gamma</span> / <span class='mono'>gross_vega</span> / "
                "<span class='mono'>gross_theta</span> are <b>SIGNED NET</b> portfolio greeks "
                "from the short-index / long-names book, <b>not gross magnitudes</b>. "
                "<span class='mono'>gross_theta</span> is annualized dollars-per-year — the "
                "daily accrual is <span class='mono'>value / 365</span>."
            ),
            Subhead("Appendix: the RepresentativeFast tier defect"),
            Prose(
                "The projected route is marked <b>cold</b> because the fast query tier "
                "misprices exactly the leg this book is shortest. The "
                "<b>RepresentativeFast</b> tier uses a single Chebyshev American-correction "
                "cache spanning the whole surface domain, which overstates the "
                f"{index_sym} ATM / short-T put early-exercise correction by <b>~800 bps</b> "
                "(fast <span class='mono'>13.7974396166</span> vs cold "
                "<span class='mono'>12.7750175640</span>). That single mark is the outlier "
                "dominating the divergence chart above."
            ),
            Note(
                "Scope, stated precisely so the defect is not overstated: <b>calls are "
                "unaffected</b> (~1e-7), and the single-name puts diverge by only <b>6–47 "
                "bps</b>. The damage is confined to the index short put — which is why the "
                "fast tier is usable as a diagnostic but the cold run is canonical for parity.",
                flat=True,
            ),
        ],
    ))


def _num(value: str) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return float("nan")
