"""End-to-end gate for the dispersion CLI's hard cutover to RunArchive (Wave A T9).

Guarded to the local 3-session paired fixture and a built example exe: the whole
module skips cleanly when either is absent (CI / a fresh checkout), so it never
turns red away from the box that carries the fixture.

The flow mirrors the pipeline the sprint runs by hand:

  1. copy the pristine ``scratchpad/paired/run`` fixture into a fresh temp dir
     (NEVER mutating ``paired``), rewriting only the ``occ_ess_inventory.tsv``
     path column so the OCC-ESS envelope check resolves inside the copy;
  2. run ``build-schedule`` then ``run-backtest`` — which, post-cutover, publish
     ``run.atxrun`` instead of the loose ``backtest.tsv`` / ``reconciliation.tsv``
     / ``contract_marks.tsv`` result files;
  3. read the economic result back through the RunArchive Python path
     (``io.read_backtest_archive`` and ``parity.build_parity_report_from_archive``)
     and assert the known-good economics: final NAV ``-456.5769067`` over 3 dates,
     and a finite daily-P&L correlation from the parity stats.

Before the cutover this fails RED: ``run-backtest`` still writes ``backtest.tsv``
and never emits ``run.atxrun``, so ``RunArchive.open`` raises FileNotFoundError.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

# The session scratchpad root that carries the pristine paired fixture. Absolute
# and machine-specific by design; the guard below skips everywhere it is absent.
_SP = Path(
    r"C:\Users\natha\AppData\Local\Temp\claude\c--atx"
    r"\b8ae4870-03de-493c-ad84-2006e8f7409e\scratchpad"
)
_PAIRED_RUN = _SP / "paired" / "run"
_BIN = Path(r"C:\atx\build-rel\bin")
_EXE = _BIN / "atxvol_spy_dispersion_backtest.exe"

# Known-good economics on this fixture (T7/T8 golden), formatted like the CLI's
# own ``final_nav=%.10g`` line.
_EXPECTED_FINAL_NAV = "-456.5769067"
_EXPECTED_DATES = 3

pytestmark = pytest.mark.skipif(
    not (_PAIRED_RUN.is_dir() and _EXE.is_file()),
    reason="paired fixture or built example exe absent (guarded end-to-end test)",
)


def _run_cli(subcommand: str, run_dir: Path) -> subprocess.CompletedProcess:
    env = dict(os.environ)
    env["PATH"] = str(_BIN) + os.pathsep + env.get("PATH", "")
    return subprocess.run(
        [str(_EXE), subcommand, "--run", str(run_dir)],
        env=env, capture_output=True, text=True, timeout=600,
    )


@pytest.fixture(scope="module")
def run_dir(tmp_path_factory) -> Path:
    """A fresh copy of the paired fixture with the pipeline run through run.atxrun."""
    dst = tmp_path_factory.mktemp("t9run") / "run"
    shutil.copytree(_PAIRED_RUN, dst)

    # The paired fixture was built by an OLD pipeline that still wrote the loose
    # result TSVs; remove them so we test that the post-cutover pipeline does NOT
    # recreate them (they are pure outputs — no subcommand reads them as input).
    for stale in ("backtest.tsv", "reconciliation.tsv", "contract_marks.tsv"):
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
        proc = _run_cli(sub, dst)
        assert proc.returncode == 0, f"{sub} failed: {proc.stdout}\n{proc.stderr}"
    return dst


def test_run_archive_is_published(run_dir: Path):
    # The hard cutover: run.atxrun exists and the loose result TSVs do not.
    assert (run_dir / "run.atxrun").is_file()
    for gone in ("backtest.tsv", "reconciliation.tsv", "contract_marks.tsv"):
        assert not (run_dir / gone).exists(), f"{gone} should no longer be written"


def test_io_reads_final_nav_from_archive(run_dir: Path):
    from atxvol.report import io

    result, meta, extra = io.read_backtest_archive(str(run_dir / "run.atxrun"))
    assert result.size() == _EXPECTED_DATES
    assert f"{float(result.nav[-1]):.10g}" == _EXPECTED_FINAL_NAV


def test_parity_reads_archive_and_reproduces_economics(run_dir: Path, tmp_path: Path):
    from atxvol.report.parity import build_parity_report_from_archive

    out = tmp_path / "parity.html"
    stats = build_parity_report_from_archive(str(run_dir / "run.atxrun"), str(out))
    assert out.is_file()
    assert stats.n_sessions == _EXPECTED_DATES
    # Self-parity (no projected route in this fixture): listed vs listed is a
    # finite, perfect correlation, and the render still produces a document.
    import math
    assert math.isfinite(stats.corr)
    html = out.read_text(encoding="utf-8")
    assert html.startswith("<!doctype html>")


def test_dump_reproduces_backtest_tsv_byteshape(run_dir: Path):
    # The escape hatch: `runarchive dump <dir> backtest --tsv` reproduces the
    # legacy write_backtest_tsv byte-shape (header + %.17g doubles) exactly.
    env = dict(os.environ)
    env["PATH"] = str(_BIN) + os.pathsep + env.get("PATH", "")
    proc = subprocess.run(
        [str(_EXE), "runarchive", "dump", str(run_dir), "backtest", "--tsv"],
        env=env, capture_output=True, timeout=120,
    )
    assert proc.returncode == 0, proc.stderr.decode("utf-8", "replace")
    golden = _SP / "t7-check" / "run" / "backtest.tsv"
    # Compare byte-for-byte against the T7 golden backtest.tsv.
    assert proc.stdout == golden.read_bytes()
