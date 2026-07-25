"""Gates for the reporting library.

Covers the invariants that are easy to regress silently: the validated palette
order, chart geometry (axis ticks that collide, labels that escape the plot),
escaping, TSV round-trip fidelity, and self-containment of the rendered file.
"""

from __future__ import annotations

import math
import re

import pytest

import atxvol as av
from atxvol.report import charts, theme
from atxvol.report.charts import Series
from atxvol.report.components import (
    Column, Figure, Report, Section, Stat, StatRow, Table,
)
from atxvol.report.io import read_backtest_tsv, read_kv_tsv


# ── Theme ───────────────────────────────────────────────────────────────────

def test_palette_is_the_validated_order():
    # The slot order is the CVD-safety mechanism, not cosmetic. If this changes,
    # re-run scripts/validate_palette.js before updating the expectation.
    assert theme.SERIES == (
        "#2f6fb5", "#a8730f", "#0e8a6b", "#c0392b", "#7d4bab", "#4a7a2a",
    )


def test_series_color_never_cycles():
    assert theme.series_color(0) == theme.SERIES[0]
    assert theme.series_color(len(theme.SERIES) - 1) == theme.SERIES[-1]
    # A 9th series must be a loud error, not a silently reused hue.
    with pytest.raises(ValueError, match="fold the tail"):
        theme.series_color(len(theme.SERIES))
    with pytest.raises(ValueError):
        theme.series_color(-1)


def test_stylesheet_defines_every_token_it_uses():
    css = theme.stylesheet()
    used = set(re.findall(r"var\((--[a-z0-9-]+)\)", css))
    declared = set(re.findall(r"(--[a-z0-9-]+)\s*:", css))
    assert used - declared == set()


# ── Chart geometry ──────────────────────────────────────────────────────────

def test_nice_ticks_are_round_and_span_the_data():
    ticks = charts.nice_ticks(-1116.9, 0.0)
    assert ticks[0] <= -1116.9 and ticks[-1] >= 0.0
    assert any(abs(t) < 1e-12 for t in ticks)  # zero is on a P&L axis
    steps = {round(b - a, 9) for a, b in zip(ticks, ticks[1:])}
    assert len(steps) == 1  # evenly spaced


def test_nice_ticks_handles_degenerate_range():
    assert charts.nice_ticks(0.0, 0.0)
    assert charts.nice_ticks(5.0, 5.0)


def test_x_ticks_include_last_without_crowding():
    # The bug this guards: appending the final index next to a stride tick
    # renders two overprinted labels ("04-2830") at the right edge.
    for n in range(2, 200):
        idx = charts._x_tick_indices(n)
        assert idx[0] == 0 and idx[-1] == n - 1
        assert idx == sorted(set(idx))
        if n > 6 and len(idx) >= 2:
            stride = max(1, round((n - 1) / 5))
            assert idx[-1] - idx[-2] >= stride * 0.6


def test_line_chart_keeps_marks_inside_the_viewbox():
    svg = charts.line_chart(
        [Series("a", [0.0, 5.0, -3.0, 9.0], area=True, label_end=True)],
        ["01-01", "01-02", "01-03", "01-04"], width=800, height=300,
    )
    assert 'viewBox="0 0 800 300"' in svg
    for x, y in re.findall(r'<circle cx="([\d.]+)" cy="([\d.]+)"', svg):
        assert 0 <= float(x) <= 800 and 0 <= float(y) <= 300
    for pts in re.findall(r'points="([^"]+)"', svg):
        for pair in pts.split():
            px, py = (float(v) for v in pair.split(","))
            assert 0 <= px <= 800 and 0 <= py <= 300


def test_bar_chart_grows_from_the_zero_baseline():
    svg = charts.bar_chart([10.0, -10.0], ["a", "b"], width=400, height=200)
    assert theme.POSITIVE in svg and theme.NEGATIVE in svg
    assert svg.count("<title>") == 2  # every bar reachable on hover


def test_charts_are_empty_for_empty_input():
    assert charts.line_chart([], []) == ""
    assert charts.bar_chart([], []) == ""
    assert charts.small_multiple([], [], color=theme.SERIES[0]) == ""


def test_gridlines_are_solid_never_dashed():
    svg = charts.line_chart([Series("a", [1.0, 2.0])], ["x", "y"])
    assert "stroke-dasharray" not in svg


# ── Components ──────────────────────────────────────────────────────────────

def test_figure_numbering_is_continuous_and_legend_rules_hold():
    report = Report(title="T")
    report.add(Section("S", body=[
        Figure(charts.line_chart([Series("a", [1.0, 2.0])], ["x", "y"]), title="One"),
        Figure(charts.line_chart([Series("a", [1.0, 2.0])], ["x", "y"]), title="Two",
               legend=[("a", theme.SERIES[0]), ("b", theme.SERIES[1])]),
    ]))
    html = report.render()
    assert "Figure 1. One" in html and "Figure 2. Two" in html
    # A single-series figure gets no legend box; a two-series figure does.
    assert html.count('<ul class="legend">') == 1


def test_table_signs_cells_and_escapes():
    table = Table(
        [Column("Name"), Column("V", tone="sign")],
        [("<script>x</script>", "-5"), ("ok", "5")],
    )
    report = Report(title="T").add(Section("S", body=[table]))
    out = report.render()
    assert "<script>x</script>" not in out
    assert "&lt;script&gt;" in out
    assert 'class="neg"' in out and 'class="pos"' in out


def test_report_is_self_contained(tmp_path):
    report = Report(title="T & <b>", eyebrow="e", standfirst="s")
    report.add(Section("S", body=[
        StatRow([Stat("k", "1", "note")]),
        Figure(charts.line_chart([Series("a", [1.0, 2.0])], ["x", "y"]), title="F"),
    ]))
    path = report.write(str(tmp_path / "r.html"))
    html = open(path, encoding="utf-8").read()
    assert html.startswith("<!doctype html>")
    # No network, no external assets.
    assert "http://" not in html and "https://" not in html
    assert "@import" not in html and "<img" not in html
    assert "<title>T &amp; &lt;b&gt;</title>" in html


# ── TSV round-trip ──────────────────────────────────────────────────────────

def test_ragged_result_raises_instead_of_crashing(tmp_path):
    # Regression: the writers index every column by the row count, so a
    # hand-built result with an unset column used to read out of bounds and take
    # the interpreter down with an access violation.
    result = av.BacktestResult()
    result.date = ["2026-01-02", "2026-01-05"]
    result.ts_ns = [1, 2]
    result.pnl_total = [1.0, 2.0]  # every other column still empty

    with pytest.raises(ValueError, match="must match the row count"):
        av.write_backtest_tsv(result, str(tmp_path / "ragged.tsv"))
    with pytest.raises(ValueError):
        av.tearsheet(result)
    with pytest.raises(ValueError):
        av.write_backtest_pnl_tsv(result, {}, str(tmp_path / "ragged2.tsv"))


def test_resize_makes_a_hand_built_result_valid():
    result = av.BacktestResult()
    result.resize(3)
    assert len(result) == 3
    assert len(result.nav) == 3 and len(result.gross_vega) == 3
    result.validate()  # does not raise

    result.date = ["a", "b"]  # shorter than the sized columns
    with pytest.raises(ValueError):
        result.validate()


def test_backtest_tsv_round_trip_is_bit_exact(tmp_path):
    result = av.BacktestResult()
    result.resize(3)
    result.date = ["2026-01-02", "2026-01-05", "2026-01-06"]
    result.ts_ns = [1, 2, 3]
    result.pnl_total = [0.0, 1.0 / 3.0, -2.7182818284590452]
    result.nav = [0.0, 1.0 / 3.0, 1.0 / 3.0 - 2.7182818284590452]

    path = str(tmp_path / "bt.tsv")
    av.write_backtest_tsv(result, path)
    back, meta, extra = read_backtest_tsv(path)

    assert meta == {} and extra == {}
    assert back.date == result.date
    assert list(back.ts_ns) == [1, 2, 3]
    # %.17g round-trips an IEEE-754 double exactly.
    assert back.pnl_total.tobytes() == result.pnl_total.tobytes()
    assert back.nav.tobytes() == result.nav.tobytes()


def test_pnl_tsv_meta_header_is_parsed(tmp_path):
    result = av.BacktestResult()
    result.resize(1)
    result.date = ["2026-01-02"]
    result.ts_ns = [1]
    result.pnl_total = [1.5]
    path = str(tmp_path / "pnl.tsv")
    av.write_backtest_pnl_tsv(result, {"strategy": "x", "n_steps": "1"}, path)

    back, meta, _ = read_backtest_tsv(path)
    assert meta["strategy"] == "x" and meta["n_steps"] == "1"
    assert back.date == ["2026-01-02"]


def test_read_kv_tsv_skips_header(tmp_path):
    path = tmp_path / "spec.tsv"
    path.write_text("key\tvalue\nlabel\tSPY run\nflat_rate\t0.043\n", encoding="utf-8")
    spec = read_kv_tsv(str(path))
    assert spec == {"label": "SPY run", "flat_rate": "0.043"}


def test_loaded_result_folds_through_the_library_tearsheet(tmp_path):
    result = av.BacktestResult()
    result.resize(3)
    result.date = ["2026-01-02", "2026-01-05", "2026-01-06"]
    result.ts_ns = [1, 2, 3]
    result.pnl_total = [0.0, 100.0, -40.0]
    result.nav = [0.0, 100.0, 60.0]
    path = str(tmp_path / "bt.tsv")
    av.write_backtest_tsv(result, path)

    back, _, _ = read_backtest_tsv(path)
    sheet = av.tearsheet(back)
    assert sheet.total_return == pytest.approx(60.0)
    assert sheet.max_drawdown == pytest.approx(40.0)


# ── PY-2 / io fidelity ──────────────────────────────────────────────────────

def test_ts_ns_round_trips_at_a_real_nanosecond_epoch(tmp_path):
    # Regression (PY-2): the reader routed the int64 `ts_ns` column through
    # `float()`. Nanosecond epochs are ~1.7e18, past 2^53, where a double's ulp
    # is 256 — so stamps came back off by up to +/-128ns while the module
    # advertised a bit-exact round-trip. The old test used ts_ns=[1,2,3] and
    # could not observe it.
    stamps = [1_753_920_000_123_456_789, 1_753_920_086_400_000_001,
              1_753_920_172_800_000_003]
    result = av.BacktestResult()
    result.resize(3)
    result.date = ["2026-07-31", "2026-08-01", "2026-08-02"]
    result.ts_ns = stamps

    path = str(tmp_path / "ns.tsv")
    av.write_backtest_tsv(result, path)
    back, _, _ = read_backtest_tsv(path)

    assert list(back.ts_ns) == stamps


def test_read_kv_tsv_skips_the_header_after_a_comment(tmp_path):
    # Regression (PY-io): the header skip was pinned to line index 0, so any
    # leading comment let the literal column names enter the spec dict as a
    # {"key": "value"} entry, which then rendered as a configuration row.
    path = tmp_path / "spec.tsv"
    path.write_text(
        "# emitted by run_surface_backtest_command\nkey\tvalue\nlabel\tSPY run\n",
        encoding="utf-8",
    )
    assert read_kv_tsv(str(path)) == {"label": "SPY run"}


def test_colophon_escapes_run_supplied_values(tmp_path):
    # Regression (PY-8): `spec` values are read from run artifacts and were
    # interpolated raw into the colophon, which components.Report joins as
    # markup. A '<' in a path or label corrupts (or injects into) the document.
    from atxvol.report.dispersion import build_report

    result = av.BacktestResult()
    result.resize(2)
    result.date = ["2026-01-02", "2026-01-05"]
    result.ts_ns = [1, 2]
    result.pnl_total = [0.0, 100.0]
    result.nav = [0.0, 100.0]
    sheet = av.tearsheet(result)

    meta = {
        "strategy": "<script>alert(1)</script>",
        "opra_root": r"C:\data\<opra>",
        "min_names": "4 & 5",
        "min_weight_coverage": "<0.8",
    }
    path = str(tmp_path / "inject.html")
    build_report(result, sheet, meta, path)
    html = (tmp_path / "inject.html").read_text(encoding="utf-8")

    assert "<script>alert(1)</script>" not in html
    assert "&lt;script&gt;alert(1)&lt;/script&gt;" in html
    assert "&lt;opra&gt;" in html
    assert "4 &amp; 5" in html
    assert "&lt;0.8" in html
    # The colophon's own markup must survive escaping of the interpolated data.
    assert "<b>Run</b>" in html
