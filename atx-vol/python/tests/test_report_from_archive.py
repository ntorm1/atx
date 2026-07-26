"""The HTML report wrapper must read the RunArchive, not a deleted TSV.

This is the regression the suite did not have. ``build_report_from_run`` kept
calling ``read_backtest_tsv`` on a loose ``backtest.tsv`` for the whole life of
the binary cutover, so it raised ``FileNotFoundError`` on every post-cutover run
directory. Nothing caught it because nothing tested it: the archive reader
(``io.read_backtest_archive``) was written and tested on the day of the cutover,
and the wrapper that was supposed to consume it never was.

The type mismatch is why the fix was not a one-line import swap.
``read_backtest_archive`` returns a binding-free ``runarchive.BacktestSection``,
but ``_av.tearsheet`` accepts only ``atxvol._core.BacktestResult`` — so a naive
swap would have traded a FileNotFoundError for a TypeError. The report layer
still must not fold its own metrics, so ``io.read_backtest_archive_result``
converts, and the tests below exercise that path end to end.
"""

from __future__ import annotations

import shutil
from pathlib import Path

import pytest

DATA = Path(__file__).resolve().parent / "data" / "runarchive"
FIXTURE = DATA / "dispersion_paired.atxrun"

# The paired fixture's known-good economics, as the CLI prints them:
# final_nav=-456.5769067 over 3 sessions.
_EXPECTED_NAV_MONEY = "-$457"      # hero / caption, money at zero dp
_EXPECTED_NAV_CENTS = "-$456.58"   # the daily P&L table


def test_build_report_from_run_reads_the_archive(tmp_path):
    """Renders from ``run.atxrun`` alone, with no TSV anywhere in the run dir.

    Falsifiable in the direction that matters: reintroducing a TSV read makes this
    fail, because the run directory deliberately contains no TSV to find.
    """
    from atxvol.report.dispersion import build_report_from_run

    run_dir = tmp_path / "run"
    run_dir.mkdir()
    shutil.copy(FIXTURE, run_dir / "run.atxrun")
    assert not list(run_dir.glob("*.tsv")), "the point is that no TSV is present"

    out = tmp_path / "report.html"
    written = build_report_from_run(str(run_dir), str(out), label="archive-only")

    assert Path(written) == out and out.exists()
    html = out.read_text(encoding="utf-8")

    # A rendered document, not an empty shell.
    assert "<svg" in html and "</html>" in html
    assert 'src="http' not in html and 'href="http' not in html, "must stay self-contained"

    # The library's tearsheet fold ran, which also proves the conversion to the C++
    # BacktestResult held — a BacktestSection would have raised instead.
    assert _EXPECTED_NAV_MONEY in html, "the fixture's final nav is absent from the report"
    assert _EXPECTED_NAV_CENTS in html, "the daily P&L table did not render the fixture's nav"


def test_build_report_from_run_rejects_a_directory_with_no_archive(tmp_path):
    """Negative control for the test above — it must be able to fail."""
    from atxvol.report.dispersion import build_report_from_run

    empty = tmp_path / "empty"
    empty.mkdir()
    with pytest.raises(FileNotFoundError, match="run.atxrun"):
        build_report_from_run(str(empty), str(tmp_path / "x.html"))


def test_section_selects_which_economics_are_rendered(tmp_path):
    """``section`` must actually change the document.

    The archive holds several backtest-shaped sections. Rendering two of them from
    the SAME run directory has to produce different bytes, or the parameter is
    decorative. Skips rather than lies when the fixture carries only one such
    section — a same-bytes assertion would pass vacuously in that case.
    """
    from atxvol.report.dispersion import build_report_from_run
    from atxvol.report.runarchive import RunArchive

    archive = RunArchive.open(str(FIXTURE))
    try:
        present = set(archive.sections)
    finally:
        archive.close()

    alternates = [s for s in ("projected_cold", "projected_nodiv") if s in present]
    if not alternates:
        pytest.skip(f"fixture has no alternate backtest section: {sorted(present)}")

    run_dir = tmp_path / "run"
    run_dir.mkdir()
    shutil.copy(FIXTURE, run_dir / "run.atxrun")

    a = tmp_path / "listed.html"
    b = tmp_path / "alt.html"
    build_report_from_run(str(run_dir), str(a), section="backtest")
    build_report_from_run(str(run_dir), str(b), section=alternates[0])

    assert a.read_bytes() != b.read_bytes(), (
        f"'backtest' and '{alternates[0]}' rendered identical bytes — the section "
        "parameter is not reaching the reader"
    )
