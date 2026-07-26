"""Minimal gates for the reporting library.

Trimmed from a comprehensive suite to the handful of things that are genuinely
Python-side and have actually broken before:

  * the validated palette order — a CVD-safety mechanism, not a style choice, and
    silently reversible;
  * escaping — an injection defect if it regresses;
  * self-containment — the report must carry no external asset;
  * the TSV round-trip through the binding, which must be bit-exact;
  * the ragged-result guard, which used to take the interpreter down with an
    access violation rather than raising.

Chart geometry (tick spacing, viewBox containment, bar baselines, figure
numbering) was dropped: it is layout detail, cheap to eyeball in a rendered
report, and it was the bulk of the file.
"""

from __future__ import annotations

import re

import pytest

import atxvol as av
from atxvol.report import charts, theme
from atxvol.report.charts import Series
from atxvol.report.components import Column, Figure, Report, Section, Table
from atxvol.report.io import read_backtest_tsv, read_kv_tsv


def test_palette_is_the_validated_order():
    # The slot order IS the colourblind-safety mechanism. If this changes, re-run
    # scripts/validate_palette.js before updating the expectation.
    assert theme.SERIES == (
        "#2f6fb5", "#a8730f", "#0e8a6b", "#c0392b", "#7d4bab", "#4a7a2a",
    )


def test_series_color_never_cycles():
    # A 9th series must be a loud error, not a silently reused hue.
    assert theme.series_color(0) == theme.SERIES[0]
    with pytest.raises(ValueError, match="fold the tail"):
        theme.series_color(len(theme.SERIES))


def test_stylesheet_defines_every_token_it_uses():
    css = theme.stylesheet()
    used = set(re.findall(r"var\((--[a-z0-9-]+)\)", css))
    declared = set(re.findall(r"(--[a-z0-9-]+)\s*:", css))
    assert used - declared == set()


def test_table_escapes_and_report_is_self_contained(tmp_path):
    report = Report(title="T & <b>", eyebrow="e", standfirst="s")
    report.add(Section("S", body=[
        Table([Column("Name"), Column("V", tone="sign")],
              [("<script>x</script>", "-5"), ("ok", "5")]),
        Figure(charts.line_chart([Series("a", [1.0, 2.0])], ["x", "y"]), title="F"),
    ]))
    html = open(report.write(str(tmp_path / "r.html")), encoding="utf-8").read()

    assert "<script>x</script>" not in html and "&lt;script&gt;" in html
    assert 'class="neg"' in html and 'class="pos"' in html
    assert html.startswith("<!doctype html>")
    # No network, no external assets.
    assert "http://" not in html and "https://" not in html
    assert "@import" not in html and "<img" not in html
    assert "<title>T &amp; &lt;b&gt;</title>" in html


def test_ragged_result_raises_instead_of_crashing(tmp_path):
    # Regression: the writers index every column by the row count, so a hand-built
    # result with an unset column used to read out of bounds and take the
    # interpreter down with an access violation.
    result = av.BacktestResult()
    result.date = ["2026-01-02", "2026-01-05"]
    result.ts_ns = [1, 2]
    result.pnl_total = [1.0, 2.0]  # every other column still empty

    with pytest.raises(ValueError, match="must match the row count"):
        av.write_backtest_tsv(result, str(tmp_path / "ragged.tsv"))
    with pytest.raises(ValueError):
        av.tearsheet(result)


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


def test_read_kv_tsv_skips_header(tmp_path):
    path = tmp_path / "spec.tsv"
    path.write_text("key\tvalue\nlabel\tSPY run\nflat_rate\t0.043\n", encoding="utf-8")
    assert read_kv_tsv(str(path)) == {"label": "SPY run", "flat_rate": "0.043"}
