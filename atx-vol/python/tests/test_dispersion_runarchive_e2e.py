"""Gates for the dispersion RunArchive read path (Wave A T9 cutover, Wave B T9/O1).

Two tiers, because the two things under test have very different costs and very
different owners:

**Fast tier (runs by default, pure Python, no subprocess).** The Python side owns
*reading* ``run.atxrun`` and rendering from it. That is held here against the
committed fixture ``tests/data/runarchive/dispersion_paired.atxrun`` — a real
archive published by the post-cutover CLI over the 3-session paired fixture,
carrying the FULL listed + cold-projected union (9 sections). Everything these
tests assert is a property of the archive bytes and the Python reader, so
re-running the C++ pipeline to obtain them buys nothing and costs minutes.

**Slow tier (``pytest -m slow``, needs the built exe + the local paired fixture).**
The hard cutover itself — ``run-backtest`` publishing ``run.atxrun`` and no longer
writing the loose ``backtest.tsv`` / ``reconciliation.tsv`` / ``contract_marks.tsv``
— is a *CLI* behaviour, not a Python one. It is kept, but deselected by default
(``addopts = -m 'not slow'``) so the default suite stays subsecond and does not
depend on a build. Regenerate the committed fixture from this tier's run dir when
the archive format or the pipeline economics intentionally change.
"""

from __future__ import annotations

import math
import os
import shutil
import subprocess
from pathlib import Path

import pytest

from atxvol.report import parity
from atxvol.report.runarchive import RunArchive

DATA = Path(__file__).resolve().parent / "data" / "runarchive"
FIXTURE = DATA / "dispersion_paired.atxrun"
GOLDEN_TSV = DATA / "backtest_paired_golden.tsv"

# Known-good economics on the paired fixture (T7/T8 golden), formatted like the
# CLI's own ``final_nav=%.10g`` line.
_EXPECTED_FINAL_NAV = "-456.5769067"
_EXPECTED_DATES = 3

# The union a fully-run dispersion pipeline publishes into ONE container: the
# listed sections plus the cold-projected ones, merged by the identity-guarded
# merge-write rather than clobbering each other (Wave A I1).
_EXPECTED_SECTIONS = [
    "backtest", "contract_marks", "diagnostics", "mark_divergence", "meta",
    "projected_cold", "projected_schedule", "reconciliation", "trade_schedule",
]


@pytest.fixture(scope="module")
def archive():
    ar = RunArchive.open(str(FIXTURE))
    yield ar
    ar.close()


# ── merge-write union + divergence closure ───────────────────────────────────

def test_archive_carries_the_listed_and_projected_union(archive):
    # I1 closure: the projected subcommands merge into the listed container
    # instead of rebuilding it from their own sections alone.
    assert archive.sections == _EXPECTED_SECTIONS


def test_mark_divergence_is_empty(archive):
    # The persisted projected_schedule marks equal the marks the cold replay
    # recomputes, because ONE ProjectionConfig{} (analytic + ColdReference)
    # governs both routes. A nonzero row count would mean the two cold routes
    # drifted (Wave B T9/O1).
    assert archive.section("mark_divergence").n_rows == 0


def test_archive_passes_full_crc_validation(archive):
    archive.validate_all()  # does not raise


# ── economics read back through the Python path ──────────────────────────────

def test_io_reads_final_nav_from_archive():
    from atxvol.report import io

    result, meta, extra = io.read_backtest_archive(str(FIXTURE))
    assert result.size() == _EXPECTED_DATES
    assert f"{float(result.nav[-1]):.10g}" == _EXPECTED_FINAL_NAV


def test_archive_reproduces_the_golden_backtest_tsv(archive):
    # The escape hatch's fidelity, checked without spawning the CLI: every column
    # the legacy ``write_backtest_tsv`` byte-shape carried is present in the
    # archive and bit-identical to the golden. ``%.17g`` round-trips an IEEE-754
    # double exactly, so parsing the golden loses nothing and a plain ``==`` on
    # floats is a byte comparison.
    header, rows = parity._read_tsv(str(GOLDEN_TSV))
    section = archive.section("backtest")
    assert len(rows) == _EXPECTED_DATES
    assert [r["date"] for r in rows] == list(section.dict("date"))
    assert [int(r["ts_ns"]) for r in rows] == list(section.i64("ts_ns"))

    numeric = [c for c in header if c not in ("date", "ts_ns")]
    assert numeric, "golden carries no numeric columns"
    for column in numeric:
        assert list(section.f64(column)) == [float(r[column]) for r in rows], column


def test_parity_report_renders_from_the_archive(tmp_path):
    out = tmp_path / "parity.html"
    stats = parity.build_parity_report_from_archive(str(FIXTURE), str(out))

    assert out.is_file()
    assert stats.n_sessions == _EXPECTED_DATES
    assert math.isfinite(stats.corr)
    html = out.read_text(encoding="utf-8")
    assert html.startswith("<!doctype html>")
    # The projected track resolved to the cold section, so the label follows it.
    assert "projected (cold)" in html


# ── slow tier: the CLI hard-cutover gate ─────────────────────────────────────
#
# Deselected by default. Everything below drives the built example exe over the
# local paired fixture; it is the only place that can prove the CLI stopped
# writing the loose result TSVs, which no read-side test can observe.

_SP = Path(
    r"C:\Users\natha\AppData\Local\Temp\claude\c--atx"
    r"\b8ae4870-03de-493c-ad84-2006e8f7409e\scratchpad"
)
_PAIRED_RUN = _SP / "paired" / "run"
_BIN = Path(r"C:\atx\build-rel\bin")
_EXE = _BIN / "atxvol_spy_dispersion_backtest.exe"

_STALE_TSVS = ("backtest.tsv", "reconciliation.tsv", "contract_marks.tsv")


def _env() -> dict:
    env = dict(os.environ)
    env["PATH"] = str(_BIN) + os.pathsep + env.get("PATH", "")
    return env


def _run_argv(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(_EXE), *args], env=_env(), capture_output=True, text=True, timeout=600
    )


@pytest.fixture(scope="module")
def cli_run_dir(tmp_path_factory) -> Path:
    """A fresh copy of the paired fixture with the listed pipeline run through it."""
    dst = tmp_path_factory.mktemp("cutover") / "run"
    shutil.copytree(_PAIRED_RUN, dst)

    # The paired fixture was built by an OLD pipeline that still wrote the loose
    # result TSVs; remove them so we test that the post-cutover pipeline does NOT
    # recreate them (they are pure outputs — no subcommand reads them as input).
    for stale in _STALE_TSVS:
        (dst / stale).unlink(missing_ok=True)

    # Rewrite the OCC-ESS inventory path column into the copy's envelope. The
    # fixture stores absolute paths under ``paired\run\occ_ess``; point them at
    # the copy so verify_occ_ess_evidence's containment check passes. Content
    # fingerprints are unchanged (only the directory prefix moves).
    # Byte-level rewrite so the C++ writer's `\n` line endings survive (Python
    # text mode would translate them to `\r\n` on Windows and break the header).
    inv = dst / "occ_ess_inventory.tsv"
    data = inv.read_bytes()
    for old, new in ((str(_PAIRED_RUN), str(dst)),
                     (str(_PAIRED_RUN).replace("\\", "/"), str(dst).replace("\\", "/"))):
        data = data.replace(old.encode("utf-8"), new.encode("utf-8"))
    inv.write_bytes(data)

    for sub in ("build-schedule", "run-backtest"):
        proc = _run_argv(sub, "--run", str(dst))
        assert proc.returncode == 0, f"{sub} failed: {proc.stdout}\n{proc.stderr}"
    return dst


@pytest.mark.slow
@pytest.mark.skipif(
    not (_PAIRED_RUN.is_dir() and _EXE.is_file()),
    reason="paired fixture or built example exe absent",
)
def test_cli_publishes_the_archive_and_drops_the_loose_tsvs(cli_run_dir: Path):
    assert (cli_run_dir / "run.atxrun").is_file()
    for gone in _STALE_TSVS:
        assert not (cli_run_dir / gone).exists(), f"{gone} should no longer be written"


@pytest.mark.slow
@pytest.mark.skipif(
    not (_PAIRED_RUN.is_dir() and _EXE.is_file()),
    reason="paired fixture or built example exe absent",
)
def test_cli_dump_reproduces_backtest_tsv_byteshape(cli_run_dir: Path):
    # `runarchive dump <dir> backtest --tsv` reproduces the legacy
    # write_backtest_tsv byte-shape (header + %.17g doubles) exactly.
    proc = subprocess.run(
        [str(_EXE), "runarchive", "dump", str(cli_run_dir), "backtest", "--tsv"],
        env=_env(), capture_output=True, timeout=120,
    )
    assert proc.returncode == 0, proc.stderr.decode("utf-8", "replace")
    assert proc.stdout == GOLDEN_TSV.read_bytes()
