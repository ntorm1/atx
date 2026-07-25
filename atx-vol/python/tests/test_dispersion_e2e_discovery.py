"""Gate on the dispersion e2e module's driver/fixture DISCOVERY (PY-FIX 1).

These tests live outside ``test_dispersion_runarchive_e2e.py`` on purpose. That
module is ``skipif``-guarded at module scope, so nothing inside it can police its
own guard: when discovery misfires, every test in it vanishes and the lane still
reports green. A guard that reports success while observing nothing is the exact
defect class this sprint has been closing, and it is only observable from a
module that the guard does not cover.

The defect these pin: the resolver selected the first candidate DIRECTORY that
existed rather than the first that actually contained the artifact, so a
``<repo>/build/bin`` from a tree configured with ``ATX_BUILD_EXAMPLES=OFF``
(the directory exists, the driver was never built into it) short-circuited the
search and skipped the module on every worktree.
"""

from __future__ import annotations

import test_dispersion_runarchive_e2e as e2e


def test_resolved_bin_dir_actually_holds_the_driver():
    """The selected bin dir must contain the exe, or nothing may be selected."""
    assert e2e._EXE is None or e2e._EXE.is_file(), (
        f"discovery selected {e2e._EXE}, which is not a file: the module will "
        f"skip while claiming it looked for a built exe"
    )


def test_resolved_fixture_root_actually_holds_the_paired_run():
    """Same rule for the fixture half: select on the artifact, not the folder."""
    assert e2e._SP is None or (e2e._SP / "paired" / "run").is_dir(), (
        f"discovery selected fixture root {e2e._SP}, which has no paired/run"
    )


def test_resolver_skips_a_directory_that_exists_but_lacks_the_artifact(tmp_path):
    empty = tmp_path / "build" / "bin"
    empty.mkdir(parents=True)
    real = tmp_path / "build-rel" / "bin"
    real.mkdir(parents=True)
    (real / e2e._EXE_NAME).write_bytes(b"")

    chosen, trace = e2e._resolve_root(
        (("empty", empty), ("real", real)), (e2e._EXE_NAME,)
    )
    assert chosen == real
    # The rejection is recorded, naming the path and the missing artifact.
    assert str(empty) in trace[0] and e2e._EXE_NAME in trace[0]


def test_resolver_returns_none_and_a_full_trace_when_nothing_qualifies(tmp_path):
    absent = tmp_path / "never-created"
    empty = tmp_path / "bin"
    empty.mkdir()

    chosen, trace = e2e._resolve_root(
        (("$SOME_ENV", None), ("absent", absent), ("empty", empty)),
        (e2e._EXE_NAME,),
    )
    assert chosen is None
    # One line per candidate — an unresolved search reports every door it tried.
    assert len(trace) == 3
    assert "not set" in trace[0]
    assert str(absent) in trace[1]
    assert str(empty) in trace[2]


def test_no_bin_candidate_reaches_another_checkout_without_an_explicit_opt_in():
    """A driver from a foreign checkout must never be reachable implicitly.

    WS-Y already found this hazard on the import side (a site-packages editable
    install resolving ``atxvol`` to another checkout produced a green run against
    the wrong sources). Executing another checkout's binary is the same bug: it
    would report green for code this run never built.
    """
    for label, candidate in e2e._BIN_CANDIDATES:
        if candidate is None or label.startswith("$"):
            continue  # env overrides are the explicit opt-in
        assert candidate.is_relative_to(e2e._REPO), (
            f"implicit bin candidate {label} -> {candidate} is outside "
            f"{e2e._REPO}; a driver from another checkout would silently produce "
            f"this module's result"
        )


def test_skip_reason_names_every_searched_path_and_the_remedy():
    """An unresolved search must skip LOUDLY: every path, every reason, the fix."""
    reason = e2e._skip_reason(
        None,
        ["  - $ATXVOL_BIN: not set", "  - <repo>/build/bin: C:/x/bin -- no driver"],
        None,
        ["  - $ATXVOL_FIXTURE_ROOT: not set", "  - <repo>/tests/data: C:/y -- empty"],
    )
    for fragment in ("C:/x/bin", "C:/y", "$ATXVOL_BIN", "$ATXVOL_FIXTURE_ROOT"):
        assert fragment in reason, f"skip reason omits {fragment}:\n{reason}"
    # The actionable remedy, so the reader is not left to guess.
    assert "ATX_BUILD_EXAMPLES=ON" in reason
    assert e2e._EXE_NAME in reason


def test_a_fully_resolved_discovery_produces_no_skip_reason(tmp_path):
    assert e2e._skip_reason(tmp_path, [], tmp_path, [], "") == ""


# ── the capability half: a driver that EXISTS is not a driver that can serve ──


def test_usage_banner_without_the_subcommand_is_rejected():
    banner = (
        "usage:\n"
        "  atxvol_spy_dispersion_backtest build-schedule --run DIR\n"
        "  atxvol_spy_dispersion_backtest run-backtest --run DIR\n"
        "  atxvol_spy_dispersion_backtest verify --run DIR\n"
    )
    assert e2e._usage_declares(banner, e2e._REQUIRED_SUBCOMMAND) is False


def test_usage_banner_with_the_subcommand_is_accepted():
    banner = "  atxvol_spy_dispersion_backtest runarchive dump DIR SECTION [--tsv]\n"
    assert e2e._usage_declares(banner, e2e._REQUIRED_SUBCOMMAND) is True


def test_a_driver_that_cannot_serve_is_skipped_rather_than_run():
    """A resolved-but-incapable driver must produce a skip, never five red tests.

    Rules 1 and 2 alone would have run this tree's own driver against a
    `runarchive` subcommand it does not implement and blamed the Python layer.
    """
    assert e2e._DRIVER_NOTE == "" or e2e._SKIP_REASON, (
        "the driver was rejected on capability but the module was not skipped"
    )


def test_the_skip_reason_records_an_incapable_driver_by_path_and_by_reason():
    reason = e2e._skip_reason(
        object(), [], object(), [], "C:/x/bin/drv.exe does not offer 'runarchive'"
    )
    assert "C:/x/bin/drv.exe" in reason
    assert e2e._REQUIRED_SUBCOMMAND in reason


def test_skipping_is_all_or_nothing_with_the_three_resolution_halves():
    """The guard fires exactly when something is unresolved — no other coupling."""
    resolved = (
        e2e._BIN is not None and e2e._SP is not None and e2e._DRIVER_NOTE == ""
    )
    assert resolved == (e2e._SKIP_REASON == "")
