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


def test_banner_text_clears_wcag_aa_on_every_tone():
    # M1 (rev-ws-y): the band used to be white text on the saturated status
    # colour — `ok` 3.74:1, `warn` 3.19:1, `unknown` 3.87:1 against a 4.5:1 bar,
    # so three of four tones failed. `Banner` is the one component whose
    # docstring makes an accessibility claim, so the claim is measured here.
    # The palette itself is unchanged (it is validated as a set and shared by
    # value with tools/spy_dispersion_tearsheet_report.py); the band is what
    # changed.
    for tone, color in theme.STATUS.items():
        tint = theme.BANNER_TINT[tone]
        badge = theme.contrast_ratio(theme.INK, tint)
        body = theme.contrast_ratio(theme.INK_2, tint)
        assert badge >= 4.5, f"{tone}: badge {badge:.2f}:1 on {tint}"
        assert body >= 4.5, f"{tone}: detail/aside {body:.2f}:1 on {tint}"
        # The colour is now carried by the leading rule — a non-text graphic,
        # held to the 3:1 bar against the page surface it sits on.
        rule = theme.contrast_ratio(color, theme.SURFACE)
        assert rule >= 3.0, f"{tone}: rule {rule:.2f}:1 on the surface"
        # And the tint must still read as a band, not as the page.
        assert theme.contrast_ratio(tint, theme.SURFACE) > 1.0


def test_contrast_ratio_is_the_wcag_formula():
    # Anchors, so the helper the accessibility gate depends on is itself pinned.
    assert theme.contrast_ratio("#000000", "#ffffff") == pytest.approx(21.0, abs=1e-9)
    assert theme.contrast_ratio("#ffffff", "#ffffff") == pytest.approx(1.0, abs=1e-9)
    assert theme.contrast_ratio("#0d9488", "#ffffff") == pytest.approx(3.74, abs=0.01)


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
        # PY-4: no regime, no report — `build_report` is held to the same
        # contract as `build_report_from_run`.
        "friction_regime": "frictionless",
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


# ── PY-4: the friction regime is the first key of every artifact ────────────
#
# The sprint's own numbers: the same strategy over the same surfaces returns
# +24740.62 frictionless, +1280.83 under retail frictions and -6460.23 once impact
# is added. The headline is ~95% friction-dominated and the SIGN FLIPS, so an
# unlabelled number is not merely incomplete, it is misleading. `write_dispersion_
# tearsheet` therefore leads both artifacts with `friction_regime` /
# `friction_detail` (dispersion_run.hpp, "THE REGIME IS NOT OPTIONAL METADATA"),
# and this renderer must honour the same contract.

def _run_result(n: int = 3):
    result = av.BacktestResult()
    result.resize(n)
    result.date = ["2026-07-0%d" % (i + 1) for i in range(n)]
    result.ts_ns = [1_753_920_000_000_000_000 + i for i in range(n)]
    result.pnl_total = [0.0, 120.0, -55.0][:n]
    result.nav = [0.0, 120.0, 65.0][:n]
    result.cost = [0.0, 30.0, 25.0][:n]
    result.gross_vega = [10_000.0, 10_000.0, 10_000.0][:n]
    return result


_REGIME_META = {
    "friction_regime": "frictioned+impact",
    "friction_detail": "spread 0.35 + commission 0.65/contract + sqrt impact 0.10",
    "label": "spy_dispersion_82",
    "opra_root": "OPRA",
}


def _write_run_dir(tmp_path, name: str, *, meta=None, loose=False, spec=None):
    run = tmp_path / "run"
    run.mkdir(exist_ok=True)
    result = _run_result()
    if loose:
        av.write_backtest_tsv(result, str(run / name))
    else:
        av.write_backtest_pnl_tsv(result, dict(meta or {}), str(run / name))
    if spec is not None:
        (run / "run_spec.tsv").write_text(
            "key\tvalue\n" + "".join(f"{k}\t{v}\n" for k, v in spec.items()),
            encoding="utf-8",
        )
    return run


def test_build_report_from_run_refuses_a_track_with_no_regime(tmp_path):
    from atxvol.report.dispersion import build_report_from_run

    run = _write_run_dir(tmp_path, "surface_backtest.tsv", loose=True,
                         spec={"label": "spy_dispersion_82"})
    # The refusal is a CODED error, like the rest of this surface: a caller
    # wrapping the pipeline in `except av.AtxError` must not miss this one
    # because it was spelled `ValueError` (rev-ws-y minor).
    with pytest.raises(av.AtxError, match="friction_regime") as excinfo:
        build_report_from_run(str(run), str(tmp_path / "out.html"))
    assert excinfo.value.code == av.ErrorCode.INVALID_ARGUMENT
    assert not (tmp_path / "out.html").exists()


def test_build_report_refuses_an_in_memory_run_with_no_regime(tmp_path):
    # The in-memory entry point is held to the same contract, and to the same
    # exception type: `build_report` writes the same HTML to the same path, and
    # a reader cannot tell which entry point produced it.
    from atxvol.report.dispersion import build_report

    result = av.BacktestResult()
    result.resize(2)
    result.date = ["2026-01-02", "2026-01-05"]
    result.ts_ns = [1, 2]
    result.nav = [100.0, 101.0]
    result.pnl_total = [0.0, 1.0]
    out = tmp_path / "inmem.html"
    with pytest.raises(av.AtxError, match="friction_regime") as excinfo:
        build_report(result, av.tearsheet(result), {"label": "x"}, str(out))
    assert excinfo.value.code == av.ErrorCode.INVALID_ARGUMENT
    assert not out.exists()


def test_build_report_from_run_renders_the_regime_banner(tmp_path):
    from atxvol.report.dispersion import build_report_from_run

    run = _write_run_dir(tmp_path, "surface_pnl_track.tsv", meta=_REGIME_META)
    out = tmp_path / "out.html"
    build_report_from_run(str(run), str(out))
    html = out.read_text(encoding="utf-8")

    # Full-width banner, before any number, carrying a TEXT label — colour is
    # never the only channel.
    assert '<div class="banner alert"' in html
    assert "FRICTIONED + IMPACT" in html
    assert "spread 0.35 + commission 0.65/contract + sqrt impact 0.10" in html
    # Every headline tile is captioned with the regime, so a cropped screenshot
    # of one tile still says which assumptions produced it.
    assert html.count("regime: frictioned+impact") >= 6
    # And the track's own chart title names it.
    assert "Cumulative P&amp;L — frictioned+impact" in html


def test_build_report_from_run_accepts_the_pnl_track_naming(tmp_path):
    # The README's Python pipeline writes `pnl_track.tsv`; the renderer only
    # looked for the C++ run-dir names, so following the README then calling the
    # renderer raised FileNotFoundError.
    from atxvol.report.dispersion import build_report_from_run

    run = _write_run_dir(tmp_path, "pnl_track.tsv", meta=_REGIME_META)
    out = tmp_path / "out.html"
    assert build_report_from_run(str(run), str(out)) == str(out)
    assert "FRICTIONED + IMPACT" in out.read_text(encoding="utf-8")


def test_build_report_from_run_reads_the_regime_from_the_tearsheet_tsv(tmp_path):
    # `surface_tearsheet.tsv` is a `metric<TAB>value` table that also leads with
    # the regime, so a legacy loose-SoA run dir beside one is still renderable.
    from atxvol.report.dispersion import build_report_from_run

    run = _write_run_dir(tmp_path, "surface_backtest.tsv", loose=True)
    (run / "surface_tearsheet.tsv").write_text(
        "metric\tvalue\nfriction_regime\tfrictionless\n"
        "friction_detail\tmid fills, no commission\ntotal_return\t247.41\n",
        encoding="utf-8",
    )
    out = tmp_path / "out.html"
    build_report_from_run(str(run), str(out))
    html = out.read_text(encoding="utf-8")
    assert '<div class="banner ok"' in html
    assert "FRICTIONLESS" in html
    assert "metric" not in read_kv_tsv(str(run / "surface_tearsheet.tsv"))


def test_regime_status_palette_is_the_validated_three_state_set():
    # Ported verbatim from tools/spy_dispersion_tearsheet_report.py, where it was
    # validated as a 3-state set: CVD separation dE 12.5 (protan) / 13.0
    # (tritan), normal-vision floor 21.2, all above the chroma floor and >= 3:1
    # against the surface. Changing a value means re-running the validator.
    assert theme.STATUS == {
        "ok": "#0d9488",
        "warn": "#d97706",
        "alert": "#c2185b",
        "unknown": theme.MUTED,
    }
