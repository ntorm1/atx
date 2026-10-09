"""Minimal gates for the dispersion two-route comparative report.

Trimmed to the two things that regress silently and are Python-only: the
tracking-stat arithmetic (pinned on a toy frame with hand-computed numbers) and
the projected-track label derivation (a `projected_nodiv` run must never render
as "cold"). The section-by-section content inventory and the diagnostics matrix
were dropped — they pinned document copy, not behaviour, and made up most of the
file.

Deliberately binding-free: the report module must import without the compiled
_atxvol extension, so nothing here imports `atxvol` (controller amendment 7).
"""

from __future__ import annotations

import ast
import math
import statistics
from pathlib import Path

import pytest

from atxvol.report import parity
from atxvol.report.parity import ParityStats, build_parity_report, compute_parity_stats

DATA = Path(__file__).parent / "data" / "dispersion_parity"
LISTED = DATA / "backtest.tsv"
COLD = DATA / "projected_cold_backtest.tsv"
DIVERGENCE = DATA / "mark_divergence.tsv"
SCHEDULE = DATA / "trade_schedule.tsv"


# ── Stats arithmetic ─────────────────────────────────────────────────────────

def test_compute_parity_stats_on_toy_frame():
    # Hand-computed against a three-session toy. tracking_error is the population
    # std of the per-session P&L differences; corr is the Pearson r of the two
    # daily P&L series; max_abs_nav_gap is the peak absolute cumulative divergence.
    stats = compute_parity_stats([0.0, 10.0, -4.0], [0.0, 6.0, -2.0],
                                 [0.0, 10.0, 6.0], [0.0, 6.0, 4.0])
    assert stats.tracking_error == pytest.approx(statistics.pstdev([0.0, 4.0, -2.0]))
    assert stats.corr == pytest.approx(0.9992600812897369)
    assert stats.max_abs_nav_gap == pytest.approx(4.0)
    assert stats.n_sessions == 3


def test_compute_parity_stats_is_robust_to_degenerate_input():
    # A single session has no defined correlation; report NaN rather than raise,
    # so a one-row run still renders.
    stats = compute_parity_stats([5.0], [5.0], [5.0], [5.0])
    assert math.isnan(stats.corr)
    assert stats.tracking_error == pytest.approx(0.0)
    assert stats.n_sessions == 1


# ── Report over the committed fixtures ───────────────────────────────────────

def test_build_parity_report_renders_and_returns_correct_stats(tmp_path):
    out = tmp_path / "parity.html"
    stats = build_parity_report(str(LISTED), str(COLD), str(DIVERGENCE),
                                str(SCHEDULE), str(out))

    _, listed_rows = parity._read_tsv(str(LISTED))
    _, cold_rows = parity._read_tsv(str(COLD))
    listed_pnl = [float(r["pnl_total"]) for r in listed_rows]
    cold_pnl = [float(r["pnl_total"]) for r in cold_rows]

    assert isinstance(stats, ParityStats)
    assert stats.n_sessions == 3
    assert stats.corr == pytest.approx(statistics.correlation(listed_pnl, cold_pnl))

    html = out.read_text(encoding="utf-8")
    assert html.startswith("<!doctype html>") and html.rstrip().endswith("</html>")
    assert "http://" not in html and "https://" not in html  # self-contained
    assert html.count("<section>") == html.count("</section>")


def test_reader_skips_the_schedule_magic_line():
    # The ATX_LISTED_DISPERSION_SCHEDULE sentinel is skipped, so the header is the
    # real column row and every leg is a data record.
    header, rows = parity._read_tsv(str(SCHEDULE))
    assert header[0] == "roll_date"
    assert len(rows) == 22 and rows[0]["symbol"] == "SPY"


# ── Archive projected-track label derivation ─────────────────────────────────
#
# build_parity_report_from_archive resolves the projected section dynamically
# (explicit projected_section, else first of projected_cold / projected_nodiv,
# else the listed track as a self-parity). The default label must follow the
# section actually resolved. Stubbed so no real mmap is needed.


class _FakeArchive:
    def __init__(self, sections):
        self._sections = set(sections)

    def __contains__(self, name):
        return name in self._sections

    def close(self):
        pass


def _patch_archive(monkeypatch, sections):
    captured: dict[str, str] = {}
    archive = _FakeArchive(sections)
    monkeypatch.setattr(parity.RunArchive, "open", classmethod(lambda cls, path: archive))
    monkeypatch.setattr(parity, "_backtest_rows", lambda a, section: [])
    monkeypatch.setattr(parity, "_schedule_rows", lambda a, section="trade_schedule": [])
    monkeypatch.setattr(parity, "_divergence_rows", lambda a, section="mark_divergence": [])

    def _fake_render(listed_rows, projected_rows, divergence_rows, schedule_rows,
                     out_html, projected_label, *rest):
        captured["label"] = projected_label
        return ParityStats(float("nan"), 0.0, 0.0, 0)

    monkeypatch.setattr(parity, "_render_parity", _fake_render)
    return captured


@pytest.mark.parametrize("sections,expected", [
    ({"projected_nodiv"}, "projected (no-divergence)"),   # must NOT render as cold
    ({"projected_cold"}, "projected (cold)"),
    ({"projected_cold", "projected_nodiv"}, "projected (cold)"),  # cold wins
    (set(), "listed (self)"),                             # listed-only self-parity
])
def test_archive_label_follows_the_resolved_section(monkeypatch, sections, expected):
    captured = _patch_archive(monkeypatch, sections)
    parity.build_parity_report_from_archive("run.atxrun", "out.html")
    assert captured["label"] == expected


def test_archive_explicit_projected_label_overrides_derivation(monkeypatch):
    captured = _patch_archive(monkeypatch, {"projected_nodiv"})
    parity.build_parity_report_from_archive(
        "run.atxrun", "out.html", projected_label="projected (custom)")
    assert captured["label"] == "projected (custom)"


# ── Amendment 7: no binding import ──────────────────────────────────────────

def test_parity_module_carries_no_binding_import():
    # Must render without the compiled extension. Parse real import statements, so
    # a docstring may still name atxvol while explaining why it is not used.
    src = Path(parity.__file__).read_text(encoding="utf-8")
    modules: list[str] = []
    for node in ast.walk(ast.parse(src)):
        if isinstance(node, ast.Import):
            modules += [a.name for a in node.names]
        elif isinstance(node, ast.ImportFrom):
            modules.append("." * (node.level or 0) + (node.module or ""))
    assert modules  # sanity: we actually found the import block
    for mod in modules:
        assert not mod.startswith("atxvol"), mod
        assert not mod.endswith(".io") and mod != "io", mod
