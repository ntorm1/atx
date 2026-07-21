"""Gates for the dispersion two-route comparative report builder.

The builder compares a listed-options dispersion backtest against the same
schedule projected onto historical surfaces (cold marks). These tests hold the
two things that regress silently: the tracking-stat arithmetic (pinned on a toy
frame with hand-computed numbers) and the produced document (parsed, not merely
non-empty — self-contained, both routes coloured, every required section
present).

Deliberately binding-free: the report module must import without the compiled
_atxvol extension, so nothing here imports `atxvol` (controller amendment 7).
"""

from __future__ import annotations

import math
import re
import statistics
from pathlib import Path

import pytest

from atxvol.report import parity
from atxvol.report.parity import ParityStats, build_parity_report, compute_parity_stats

DATA = Path(__file__).parent / "data" / "dispersion_parity"
LISTED = DATA / "backtest.tsv"
COLD = DATA / "projected_cold_backtest.tsv"
FAST = DATA / "projected_backtest.tsv"
DIVERGENCE = DATA / "mark_divergence.tsv"
SCHEDULE = DATA / "trade_schedule.tsv"


# ── Pure stats on a toy frame ────────────────────────────────────────────────

def test_compute_parity_stats_on_toy_frame():
    # Hand-computed against a three-session toy. tracking_error is the population
    # std of the per-session P&L differences (listed - projected); corr is the
    # Pearson r of the two daily P&L series; max_abs_nav_gap is the peak absolute
    # cumulative divergence.
    listed_pnl = [0.0, 10.0, -4.0]
    projected_pnl = [0.0, 6.0, -2.0]
    listed_nav = [0.0, 10.0, 6.0]
    projected_nav = [0.0, 6.0, 4.0]

    stats = compute_parity_stats(listed_pnl, projected_pnl, listed_nav, projected_nav)

    # diffs = [0, 4, -2]; pstdev = 2.494438257849294
    assert stats.tracking_error == pytest.approx(2.494438257849294)
    assert stats.tracking_error == pytest.approx(statistics.pstdev([0.0, 4.0, -2.0]))
    # Pearson r of [0,10,-4] vs [0,6,-2]
    assert stats.corr == pytest.approx(0.9992600812897369)
    assert stats.corr == pytest.approx(statistics.correlation(listed_pnl, projected_pnl))
    # |nav diff| = [0, 4, 2] -> 4
    assert stats.max_abs_nav_gap == pytest.approx(4.0)
    assert stats.n_sessions == 3


def test_compute_parity_stats_is_robust_to_degenerate_input():
    # A single session (or a flat series) has no defined correlation; the stat is
    # reported as NaN rather than raising, so a one-row run still renders.
    stats = compute_parity_stats([5.0], [5.0], [5.0], [5.0])
    assert math.isnan(stats.corr)
    assert stats.tracking_error == pytest.approx(0.0)
    assert stats.max_abs_nav_gap == pytest.approx(0.0)
    assert stats.n_sessions == 1


# ── TSV reader ───────────────────────────────────────────────────────────────

def test_reader_skips_the_schedule_magic_line():
    header, rows = parity._read_tsv(str(SCHEDULE))
    # The magic sentinel (ATX_LISTED_DISPERSION_SCHEDULE\t1) is skipped, so the
    # header is the real column row and every leg is a data record.
    assert header[0] == "roll_date"
    assert "symbol" in header
    assert len(rows) == 22
    assert rows[0]["symbol"] == "SPY"


def test_reader_reads_a_headerless_backtest_tsv():
    header, rows = parity._read_tsv(str(LISTED))
    assert header[0] == "date"
    assert len(rows) == 3
    assert rows[0]["date"] == "2026-01-02"


# ── Full report from the fixtures ────────────────────────────────────────────

def _daily_and_nav(path: Path) -> tuple[list[float], list[float]]:
    _, rows = parity._read_tsv(str(path))
    return ([float(r["pnl_total"]) for r in rows], [float(r["nav"]) for r in rows])


def test_build_parity_report_returns_correct_stats(tmp_path):
    out = tmp_path / "parity.html"
    stats = build_parity_report(str(LISTED), str(COLD), str(DIVERGENCE),
                                str(SCHEDULE), str(out))

    lp, ln = _daily_and_nav(LISTED)
    pp, pn = _daily_and_nav(COLD)
    diffs = [a - b for a, b in zip(lp, pp)]
    assert isinstance(stats, ParityStats)
    assert stats.n_sessions == 3
    assert stats.tracking_error == pytest.approx(statistics.pstdev(diffs))
    assert stats.corr == pytest.approx(statistics.correlation(lp, pp))
    assert stats.max_abs_nav_gap == pytest.approx(
        max(abs(a - b) for a, b in zip(ln, pn))
    )


def test_report_is_self_contained_and_well_formed(tmp_path):
    out = tmp_path / "parity.html"
    build_parity_report(str(LISTED), str(COLD), str(DIVERGENCE), str(SCHEDULE), str(out))
    html = out.read_text(encoding="utf-8")

    assert html.startswith("<!doctype html>")
    assert html.rstrip().endswith("</html>")
    # No network, no external assets. The chart SVGs carry no xmlns, so any
    # scheme URL would be a real external reference.
    assert "http://" not in html and "https://" not in html
    assert "@import" not in html and "<img" not in html
    # Balanced figure/section scaffolding actually rendered.
    assert html.count("<section>") == html.count("</section>") == 6
    assert html.count("<svg") == html.count("</svg>")


def test_report_covers_every_required_section(tmp_path):
    out = tmp_path / "parity.html"
    build_parity_report(str(LISTED), str(COLD), str(DIVERGENCE), str(SCHEDULE), str(out))
    html = out.read_text(encoding="utf-8")

    # 1 hero tracking stats, with the tracking-error definition on the tile.
    assert "Correlation" in html
    assert "Tracking error" in html
    assert "Max" in html and "NAV gap" in html
    assert "listed &#8722; projected" in html or "listed − projected" in html
    # 2 overlaid nav tracks + residual subchart.
    assert "NAV" in html and "residual" in html.lower()
    # 3 daily agreement scatter with the y=x reference.
    assert "y = x" in html
    # 4 cumulative attribution paired bars.
    assert "Gamma" in html and "Vega" in html and "Theta" in html and "Unexplained" in html
    # 5 mark divergence + worst offenders (SPY ATM put is the headline outlier).
    assert "divergence" in html.lower()
    assert "799" in html  # the SPY put |diff| in bps
    # 6 methodology: D7 misnomer note + fast-tier defect appendix.
    assert "SIGNED NET" in html
    assert "13.7974396166" in html and "RepresentativeFast" in html
    assert "365" in html  # gross_theta daily accrual note


def test_both_routes_keep_a_stable_route_colour(tmp_path):
    from atxvol.report import theme
    out = tmp_path / "parity.html"
    build_parity_report(str(LISTED), str(COLD), str(DIVERGENCE), str(SCHEDULE), str(out))
    html = out.read_text(encoding="utf-8")
    # Colour follows route identity, consistently, across nav overlay + attribution.
    assert theme.SERIES[0] in html  # listed
    assert theme.SERIES[1] in html  # projected
    # Both routes named in a legend (>=2 series charts carry one).
    assert 'class="legend"' in html


def test_projected_label_is_configurable_for_the_diagnostic_fast_tier(tmp_path):
    out = tmp_path / "parity_fast.html"
    stats = build_parity_report(
        str(LISTED), str(FAST), str(DIVERGENCE), str(SCHEDULE), str(out),
        projected_label="projected (fast, diagnostic)",
    )
    html = out.read_text(encoding="utf-8")
    assert "projected (fast, diagnostic)" in html
    assert html.startswith("<!doctype html>")

    # The fast tier is a genuinely different track, so the stats differ from cold.
    lp, _ = _daily_and_nav(LISTED)
    fp, _ = _daily_and_nav(FAST)
    assert stats.corr == pytest.approx(statistics.correlation(lp, fp))
    assert isinstance(stats, ParityStats)


# ── Amendment 7: no binding import ──────────────────────────────────────────

def test_parity_module_carries_no_binding_import():
    import ast

    src = Path(parity.__file__).read_text(encoding="utf-8")
    # Must render without the compiled extension. Parse real import statements
    # (so a docstring may still name atxvol / io.read_backtest_tsv while
    # explaining why they are deliberately *not* used): none may pull the binding
    # package `atxvol` or the binding-dependent `.io` reader.
    modules: list[str] = []
    for node in ast.walk(ast.parse(src)):
        if isinstance(node, ast.Import):
            modules += [a.name for a in node.names]
        elif isinstance(node, ast.ImportFrom):
            modules.append("." * (node.level or 0) + (node.module or ""))
    assert modules  # sanity: we actually found the import block
    for mod in modules:
        assert not mod.startswith("atxvol"), mod   # no `import atxvol` / `from atxvol ...`
        assert not mod.endswith(".io") and mod != "io", mod  # no `.io` reader (binding object)
