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


# ── Runtime diagnostics section (Task 10) ────────────────────────────────────

LISTED_DIAG = DATA / "diagnostics_run_backtest.tsv"
PROJECTED_DIAG = DATA / "diagnostics_run_projected_backtest.tsv"
SCHEDULE_DIAG = DATA / "diagnostics_project_schedule.tsv"


def _diag_total(path: Path) -> tuple[float, int]:
    _, rows = parity._read_tsv(str(path))
    total = next(r for r in rows if r["phase"] == "total")
    return float(total["wall_ms"]), int(total["count"])


def _diag_phase(path: Path, phase: str) -> tuple[float, int]:
    _, rows = parity._read_tsv(str(path))
    row = next(r for r in rows if r["phase"] == phase)
    return float(row["wall_ms"]), int(row["count"])


def test_diagnostics_fixture_parses_with_magic_skipped():
    # The ATX_DISPERSION_DIAGNOSTICS\t1 sentinel is skipped by the shared reader,
    # so the header is the real column row and every phase (incl. the final total)
    # is a data record.
    header, rows = parity._read_tsv(str(PROJECTED_DIAG))
    assert header == ["subcommand", "phase", "wall_ms", "count"]
    assert rows[0]["phase"] == "setup_read"
    assert rows[-1]["phase"] == "total"
    assert rows[-1]["count"] == "3"


def test_diagnostics_section_absent_by_default(tmp_path):
    # Existing callers pass no diagnostics -> no new section, section count unchanged.
    out = tmp_path / "parity.html"
    build_parity_report(str(LISTED), str(COLD), str(DIVERGENCE), str(SCHEDULE), str(out))
    html = out.read_text(encoding="utf-8")
    assert "Runtime diagnostics" not in html
    assert html.count("<section>") == 6


def test_diagnostics_section_renders_with_computed_per_unit(tmp_path):
    out = tmp_path / "parity.html"
    build_parity_report(
        str(LISTED), str(COLD), str(DIVERGENCE), str(SCHEDULE), str(out),
        listed_diagnostics_tsv=str(LISTED_DIAG),
        projected_diagnostics_tsv=str(PROJECTED_DIAG),
        schedule_diagnostics_tsv=str(SCHEDULE_DIAG),
    )
    html = out.read_text(encoding="utf-8")
    assert "Runtime diagnostics" in html
    # One extra section, placed before methodology -> 7 balanced sections.
    assert html.count("<section>") == html.count("</section>") == 7

    # Projected hero: total wall time and the computed ms/session (900 / 3 = 300).
    total_ms, sessions = _diag_total(PROJECTED_DIAG)
    assert sessions == 3
    assert f"{total_ms / sessions:.2f}" in html   # 300.00 ms/session
    assert f"{total_ms:,.1f}" in html              # 900.0 ms total

    # Listed reconciliation dominates: its computed per-session (510 / 3 = 170.000).
    recon_ms, recon_n = _diag_phase(LISTED_DIAG, "reconciliation")
    assert f"{recon_ms / recon_n:,.3f}" in html    # 170.000

    # Schedule cold_solve per-leg (230 / 46 = 5.000).
    cold_ms, cold_n = _diag_phase(SCHEDULE_DIAG, "cold_solve")
    assert f"{cold_ms / cold_n:,.3f}" in html      # 5.000

    # Still self-contained (tables only, no new external assets).
    assert "http://" not in html and "https://" not in html
    assert html.count("<svg") == html.count("</svg>")


def test_diagnostics_section_renders_with_only_projected(tmp_path):
    # Any one of the three files is enough to raise the section.
    out = tmp_path / "parity.html"
    build_parity_report(
        str(LISTED), str(COLD), str(DIVERGENCE), str(SCHEDULE), str(out),
        projected_diagnostics_tsv=str(PROJECTED_DIAG),
    )
    html = out.read_text(encoding="utf-8")
    assert "Runtime diagnostics" in html
    total_ms, sessions = _diag_total(PROJECTED_DIAG)
    assert f"{total_ms / sessions:.2f}" in html    # 300.00 hero even alone


def test_diagnostics_returns_unchanged_parity_stats(tmp_path):
    # ParityStats shape/values are untouched by the diagnostics params.
    out1 = tmp_path / "a.html"
    out2 = tmp_path / "b.html"
    base = build_parity_report(str(LISTED), str(COLD), str(DIVERGENCE), str(SCHEDULE), str(out1))
    withd = build_parity_report(
        str(LISTED), str(COLD), str(DIVERGENCE), str(SCHEDULE), str(out2),
        listed_diagnostics_tsv=str(LISTED_DIAG),
        projected_diagnostics_tsv=str(PROJECTED_DIAG),
        schedule_diagnostics_tsv=str(SCHEDULE_DIAG),
    )
    assert base == withd


# ── Archive projected-track label derivation (FIX-NOW #3) ────────────────────
#
# build_parity_report_from_archive resolves the projected section dynamically
# (explicit projected_section, else first of projected_cold / projected_nodiv,
# else the listed track as a self-parity). The default label must follow the
# section that was actually resolved — a projected_nodiv archive must never be
# labelled "cold" — while an explicitly passed projected_label still wins. These
# tests monkeypatch the archive + section readers so no real mmap is needed and
# capture the projected_label handed to the render tail.


class _FakeArchive:
    """Membership + close stand-in for RunArchive.

    Only ``name in archive`` (section resolution) and ``close`` are exercised by
    build_parity_report_from_archive once the row readers are stubbed out.
    """

    def __init__(self, sections):
        self._sections = set(sections)

    def __contains__(self, name):
        return name in self._sections

    def close(self):
        pass


def _patch_archive(monkeypatch, sections):
    """Stub RunArchive.open + the section readers; capture the render label.

    Returns a dict that build_parity_report_from_archive's render tail fills with
    the ``projected_label`` it was handed, so a test can assert on it directly.
    """
    captured: dict[str, str] = {}
    archive = _FakeArchive(sections)
    monkeypatch.setattr(parity.RunArchive, "open",
                        classmethod(lambda cls, path: archive))
    monkeypatch.setattr(parity, "_backtest_rows", lambda a, section: [])
    monkeypatch.setattr(parity, "_schedule_rows",
                        lambda a, section="trade_schedule": [])
    monkeypatch.setattr(parity, "_divergence_rows",
                        lambda a, section="mark_divergence": [])

    def _fake_render(listed_rows, projected_rows, divergence_rows, schedule_rows,
                     out_html, projected_label, *rest):
        captured["label"] = projected_label
        return ParityStats(float("nan"), 0.0, 0.0, 0)

    monkeypatch.setattr(parity, "_render_parity", _fake_render)
    return captured


def test_archive_label_defaults_to_no_divergence_for_nodiv_section(monkeypatch):
    # A projected_nodiv run must not render as cold.
    captured = _patch_archive(monkeypatch, {"projected_nodiv"})
    parity.build_parity_report_from_archive("run.atxrun", "out.html")
    assert captured["label"] == "projected (no-divergence)"


def test_archive_label_defaults_to_cold_for_cold_section(monkeypatch):
    # Regression guard: the canonical cold section keeps its historical label.
    captured = _patch_archive(monkeypatch, {"projected_cold"})
    parity.build_parity_report_from_archive("run.atxrun", "out.html")
    assert captured["label"] == "projected (cold)"


def test_archive_label_prefers_cold_when_both_sections_present(monkeypatch):
    # Resolution order is (projected_cold, projected_nodiv); cold wins, and the
    # label follows the section actually chosen.
    captured = _patch_archive(monkeypatch, {"projected_cold", "projected_nodiv"})
    parity.build_parity_report_from_archive("run.atxrun", "out.html")
    assert captured["label"] == "projected (cold)"


def test_archive_label_defaults_to_self_when_no_projected_section(monkeypatch):
    # A listed-only run stands in as a self-parity; the label says so.
    captured = _patch_archive(monkeypatch, set())
    parity.build_parity_report_from_archive("run.atxrun", "out.html")
    assert captured["label"] == "listed (self)"


def test_archive_explicit_projected_label_overrides_derivation(monkeypatch):
    # An explicit label wins over the section-derived default.
    captured = _patch_archive(monkeypatch, {"projected_nodiv"})
    parity.build_parity_report_from_archive(
        "run.atxrun", "out.html", projected_label="projected (custom)")
    assert captured["label"] == "projected (custom)"


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
