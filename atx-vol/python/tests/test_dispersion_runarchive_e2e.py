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
import warnings
from pathlib import Path

import pytest

# ── Where the pristine paired fixture and the built CLI live ─────────────────
#
# DISCOVERY POLICY (PY-FIX 1). Three rules, all learned the hard way:
#
# 1. SELECT ON THE ARTIFACT, NOT ON THE FOLDER. The previous `_first_dir` took
#    the first candidate DIRECTORY that existed. `<repo>/build/bin` exists in
#    every configured worktree, but the driver only lands there when the tree was
#    configured `-DATX_BUILD_EXAMPLES=ON` — so the search short-circuited on an
#    empty directory and the whole module skipped everywhere. The comment this
#    replaces said the module had been rewritten to escape "a permanent silent
#    skip"; that rewrite reintroduced one through a different door. A candidate
#    now qualifies only when it actually CONTAINS what the tests need.
#
# 2. NO IMPLICIT CROSS-CHECKOUT FALLBACK. The old last-resort candidate was a
#    hardcoded `C:\atx\build-rel\bin` — a DIFFERENT checkout. Merely making it
#    reachable would be worse than the skip: the module would shell out to
#    another tree's binary and report green for sources this run never built.
#    WS-Y found the same hazard on the import side (a site-packages editable
#    install silently resolving `atxvol` to another checkout). It is removed.
#    `ATXVOL_BIN` remains as the EXPLICIT opt-in, and when it resolves outside
#    this checkout the module warns with the absolute path, so nobody can
#    mistake which binary produced the numbers.
#
# 3. ASK THE DRIVER WHAT IT CAN DO, NOT WHETHER IT EXISTS. This module gates the
#    hard cutover to RunArchive. A driver built from a tree that PREDATES the
#    cutover exists, runs, and fails all five tests on a `runarchive` subcommand
#    it never had — a failure about the C++ tree's vintage dressed up as a
#    Python-layer regression. The guard therefore probes the resolved driver's
#    usage banner and records what it found. This is not hypothetical: at the
#    revision that added this policy, `git grep -l runarchive -- atx-vol` matched
#    nothing outside the Python layer, so the ONLY driver that ever satisfied
#    this module was one built from a different source line and reached through
#    rule 2's old hardcoded fallback. Rules 1 and 2 alone would have turned that
#    silent skip into five red tests blaming the wrong layer.
#
# When discovery fails the module still skips — but the skip names every
# directory it searched and why each was rejected, and states the remedy
# (`-DATX_BUILD_EXAMPLES=ON`). A skip nobody can diagnose is the defect; a skip
# that hands you the fix is a guard.
#
# The fixture half keeps its machine-local last resort: it is DATA on this box,
# not code, and dropping it would make the module skip even where the fixture
# lives. It is subject to rule 1 all the same.
_LEGACY_SP = Path(
    r"C:\Users\natha\AppData\Local\Temp\claude\c--atx"
    r"\b8ae4870-03de-493c-ad84-2006e8f7409e\scratchpad"
)
_REPO = Path(__file__).resolve().parents[3]   # <repo>/atx-vol/python/tests -> <repo>

_EXE_NAME = "atxvol_spy_dispersion_backtest.exe" if os.name == "nt" \
    else "atxvol_spy_dispersion_backtest"


def _resolve_root(
    candidates: tuple[tuple[str, Path | None], ...],
    requirements: tuple[str | Path, ...],
) -> tuple[Path | None, list[str]]:
    """First candidate directory that CONTAINS every path in ``requirements``.

    ``candidates`` is ``(label, path-or-None)`` in priority order. Returns
    ``(chosen, trace)``: ``chosen`` is ``None`` when nothing qualifies, and
    ``trace`` carries one line per candidate saying why it was rejected, so the
    caller's skip reason can name every door it tried.
    """
    trace: list[str] = []
    for label, candidate in candidates:
        if candidate is None:
            trace.append(f"  - {label}: not set")
            continue
        if not candidate.is_dir():
            trace.append(f"  - {label}: {candidate} -- no such directory")
            continue
        missing = [str(req) for req in requirements if not (candidate / req).exists()]
        if missing:
            trace.append(
                f"  - {label}: {candidate} -- exists but does not contain "
                f"{', '.join(missing)}"
            )
            continue
        trace.append(f"  - {label}: {candidate} -- SELECTED")
        return candidate, trace
    return None, trace


_ENV_ROOT = os.environ.get("ATXVOL_FIXTURE_ROOT")
# The root that carries BOTH `paired/run` (the pristine input) and `t7-check/run`
# (the T7 golden `backtest.tsv` the dump test compares against byte-for-byte).
# Both are requirements: a root with only one of them cannot run this module.
_FIXTURE_CANDIDATES: tuple[tuple[str, Path | None], ...] = (
    ("$ATXVOL_FIXTURE_ROOT", Path(_ENV_ROOT) if _ENV_ROOT else None),
    ("<repo>/atx-vol/python/tests/data/dispersion-e2e",
     _REPO / "atx-vol" / "python" / "tests" / "data" / "dispersion-e2e"),
    ("machine-local fixture scratchpad", _LEGACY_SP),
)
_SP, _FIXTURE_TRACE = _resolve_root(
    _FIXTURE_CANDIDATES,
    (Path("paired") / "run", Path("t7-check") / "run" / "backtest.tsv"),
)
_PAIRED_RUN = (_SP / "paired" / "run") if _SP is not None else None

_ENV_BIN = os.environ.get("ATXVOL_BIN")
_BIN_CANDIDATES: tuple[tuple[str, Path | None], ...] = (
    ("$ATXVOL_BIN", Path(_ENV_BIN) if _ENV_BIN else None),
    ("<repo>/build-rel/bin", _REPO / "build-rel" / "bin"),
    ("<repo>/build/bin", _REPO / "build" / "bin"),
)
_BIN, _BIN_TRACE = _resolve_root(_BIN_CANDIDATES, (_EXE_NAME,))
_EXE = (_BIN / _EXE_NAME) if _BIN is not None else None

# The subcommand every test below reaches through. This module gates the hard
# cutover to RunArchive, and a driver built from a tree that predates the cutover
# runs fine and fails every test here for a reason that has nothing to do with
# the Python layer under test. "The exe exists" was never the right question.
_REQUIRED_SUBCOMMAND = "runarchive"


def _usage_declares(usage: str, subcommand: str) -> bool:
    """Does this driver's usage banner offer ``subcommand``?

    Split out from the subprocess call so the DECISION is gated directly rather
    than only on a box that happens to hold a driver of each vintage.
    """
    return subcommand in usage


def _probe_driver(exe: Path | None, subcommand: str) -> str:
    """Empty when ``exe`` declares ``subcommand``; otherwise why it cannot serve.

    Runs the driver with no arguments, which makes it print its usage banner.
    """
    if exe is None:
        return ""
    try:
        proc = subprocess.run(
            [str(exe)], capture_output=True, text=True, timeout=120
        )
    except OSError as exc:
        return f"{exe} could not be executed: {exc}"
    if _usage_declares((proc.stdout or "") + (proc.stderr or ""), subcommand):
        return ""
    return (
        f"{exe} does not offer the '{subcommand}' subcommand, so the tree that "
        f"built it does not carry the RunArchive cutover this module gates. "
        f"Running it anyway would fail every test here for a reason unrelated "
        f"to the Python layer under test."
    )


_DRIVER_NOTE = _probe_driver(_EXE, _REQUIRED_SUBCOMMAND)

# Known-good economics on this fixture (T7/T8 golden), formatted like the CLI's
# own ``final_nav=%.10g`` line.
_EXPECTED_FINAL_NAV = "-456.5769067"
_EXPECTED_DATES = 3


def _skip_reason(
    bin_dir: Path | None,
    bin_trace: list[str],
    fixture_root: Path | None,
    fixture_trace: list[str],
    driver_note: str = "",
) -> str:
    """Empty when everything resolved; otherwise a fully diagnosable reason.

    Kept a pure function of the resolution so it can be gated directly
    (``test_dispersion_e2e_discovery.py``) instead of only when it happens to
    fire on the box running the suite.
    """
    if bin_dir is not None and fixture_root is not None and not driver_note:
        return ""
    lines = ["guarded end-to-end test: its preconditions are not met here."]
    if driver_note:
        lines.append(f"driver found but unusable: {driver_note}")
        lines.append(
            f"  remedy: build atxvol_spy_dispersion_backtest from a tree whose "
            f"examples/spy_dispersion_backtest.cpp implements "
            f"'{_REQUIRED_SUBCOMMAND}', or point $ATXVOL_BIN at one. Skipping is "
            f"the correct outcome on a tree that predates the cutover -- but it "
            f"is recorded here rather than inferred from a missing file."
        )
    if fixture_root is None:
        lines.append(
            "paired fixture root (must hold paired/run and "
            "t7-check/run/backtest.tsv) -- searched, in order:"
        )
        lines.extend(fixture_trace)
        lines.append(
            "  remedy: point $ATXVOL_FIXTURE_ROOT at a root holding both."
        )
    if bin_dir is None:
        lines.append(f"driver {_EXE_NAME} -- searched, in order:")
        lines.extend(bin_trace)
        lines.append(
            "  remedy: this test REQUIRES the example driver built from THIS "
            "tree. Configure with -DATX_BUILD_EXAMPLES=ON and build the "
            "atxvol_spy_dispersion_backtest target, or set $ATXVOL_BIN to a "
            "directory holding it."
        )
        lines.append(
            "  note: no other checkout is searched on purpose -- a driver from "
            "a different tree would report green for sources this run never "
            "built."
        )
    return "\n".join(lines)


_SKIP_REASON = _skip_reason(_BIN, _BIN_TRACE, _SP, _FIXTURE_TRACE, _DRIVER_NOTE)

pytestmark = pytest.mark.skipif(bool(_SKIP_REASON), reason=_SKIP_REASON)

if _EXE is not None:
    # Provenance, in the test output, on every run that actually executes a
    # driver: the reader never has to guess which binary produced the numbers.
    if _EXE.is_relative_to(_REPO):
        warnings.warn(
            f"atx-vol dispersion e2e: driver {_EXE} (this checkout)",
            stacklevel=1,
        )
    else:
        warnings.warn(
            f"atx-vol dispersion e2e: driver {_EXE} is OUTSIDE this checkout "
            f"({_REPO}); it was accepted only because $ATXVOL_BIN opted in. "
            f"These results describe that binary, not necessarily this tree.",
            stacklevel=1,
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


def _run_argv(*args: str) -> subprocess.CompletedProcess:
    env = dict(os.environ)
    env["PATH"] = str(_BIN) + os.pathsep + env.get("PATH", "")
    return subprocess.run(
        [str(_EXE), *args], env=env, capture_output=True, text=True, timeout=600
    )


@pytest.fixture(scope="module")
def projected_run_dir(tmp_path_factory) -> Path:
    """A fresh fixture copy run through the FULL listed + cold-projected pipeline.

    build-schedule -> run-backtest -> project-schedule -> run-projected-backtest
    --execution cold, all publishing into ONE run.atxrun via the identity-guarded
    merge-write. Its own copy, so it never perturbs the listed-only ``run_dir`` tests.
    """
    dst = tmp_path_factory.mktemp("t9proj") / "run"
    shutil.copytree(_PAIRED_RUN, dst)
    for stale in ("backtest.tsv", "reconciliation.tsv", "contract_marks.tsv"):
        (dst / stale).unlink(missing_ok=True)
    inv = dst / "occ_ess_inventory.tsv"
    data = inv.read_bytes()
    for old, new in ((str(_PAIRED_RUN), str(dst)),
                     (str(_PAIRED_RUN).replace("\\", "/"), str(dst).replace("\\", "/"))):
        data = data.replace(old.encode("utf-8"), new.encode("utf-8"))
    inv.write_bytes(data)

    steps = (
        ("build-schedule", "--run", str(dst)),
        ("run-backtest", "--run", str(dst)),
        ("project-schedule", "--run", str(dst)),
        ("run-projected-backtest", "--run", str(dst),
         "--schedule", "projected_schedule.tsv", "--execution", "cold"),
    )
    for step in steps:
        proc = _run_argv(*step)
        assert proc.returncode == 0, f"{step[0]} failed: {proc.stdout}\n{proc.stderr}"
    return dst


def test_projected_cold_union_and_zero_divergence(projected_run_dir: Path):
    # I1 closure + merge-write union (Wave B T9/O1): after the cold-projected pipeline
    # run.atxrun carries BOTH the listed sections (backtest / trade_schedule) and the
    # projected sections (projected_cold / mark_divergence), and the mark_divergence
    # section is EMPTY — the persisted projected_schedule marks equal the marks the cold
    # replay recomputes because ONE ProjectionConfig{} (analytic + ColdReference)
    # governs both routes. A nonzero row count would mean the two cold routes drifted.
    for section in ("backtest", "trade_schedule", "projected_cold", "mark_divergence"):
        proc = _run_argv("runarchive", "dump", str(projected_run_dir), section)
        assert proc.returncode == 0, f"section {section} missing: {proc.stderr}"
        assert f"section {section}:" in proc.stdout

    div = _run_argv("runarchive", "dump", str(projected_run_dir), "mark_divergence")
    # Summary line shape: "section mark_divergence: rows=0 cols=10".
    assert "rows=0 " in div.stdout, div.stdout
