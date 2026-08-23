"""Logic tests for tools/run_surface_db_backfill.py -- Task 4's chunked
surface-db backfill orchestrator.

No network, no subprocess, no compiled extension. Every case here drives a
PURE helper (or a subprocess-shaped function with the actual `subprocess.run`
call monkeypatched out) -- the orchestrator's own contract is that every
decision (chunking, rates, thresholds, ledger math, command construction) is
importable and testable without spawning `pull_opra_hive.py` or either C++
CLI for real (see the module docstring).

Cases (brief Step 2, amended by the Task 4 addendum):
  * rate_for_date: exact-month lookup off the shipped rates CSV; a date
    outside the table fails closed (ValueError), never a silent default.
  * chunk_sessions: never spans a calendar-month boundary NOR a DST-driven
    snapshot-minute change within a month (addendum §A) -- the brief's
    within-month case, the max-chunk-size case, and a new case pinning the
    2022-03-11(EST)/2022-03-14(EDT) split.
  * year_roots_partition / bisect_chunk / verify_thresholds / SpendLedger, per
    the brief's exact numbers.
  * Plus the orchestrator's own subprocess-plumbing helpers this module needed
    to actually run the phases: month_bounds, parse_estimate_line,
    parse_failed_dates, parse_minute_mismatch_dates (tolerating `have=mixed`,
    addendum §D), truncate_universe, the four command builders (pull/build/
    verify/info/query -- including one EDT and one EST --r/--snapshot-suffix
    example), atm_strike_from_forward, pick_spot_check_symbols,
    parse_query_field, hive_sessions_present, the early-close-aware calendar,
    the bisect-and-retry build policy, and the subprocess/logging seam.
"""

from __future__ import annotations

import csv
import datetime as dt
import importlib.util
import json
import math
import pathlib
import subprocess
import sys
import types

import pytest

# tools/ is not an importable package -- load the script by path (same
# convention as test_pull_opra_hive.py).
_ROOT = pathlib.Path(__file__).resolve().parents[2]
_TOOL = _ROOT / "tools" / "run_surface_db_backfill.py"
_spec = importlib.util.spec_from_file_location("run_surface_db_backfill", _TOOL)
orch = importlib.util.module_from_spec(_spec)
sys.modules["run_surface_db_backfill"] = orch
_spec.loader.exec_module(orch)

RATES_CSV = _ROOT / "data" / "rates" / "us_3m_monthly.csv"


# ── shared fakes/helpers for the phase-driver (monkeypatched subprocess.run)
# tests below (review round 1, Important-5) ─────────────────────────────────

class _FakeCompleted:
    """Stands in for ``subprocess.CompletedProcess`` -- all ``run_subprocess``
    reads off it are ``.returncode``/``.stdout``/``.stderr``."""

    def __init__(self, returncode=0, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def _pull_args(tmp_path, *, from_date, to_date, dry_run=False, cap=90.0, index="SPY"):
    return types.SimpleNamespace(
        from_date=from_date, to_date=to_date, dry_run=dry_run,
        pull_tool_path=pathlib.Path("FAKE_pull_opra_hive.py"),
        pull_universe_path=pathlib.Path("FAKE_universe.csv"),
        hive=tmp_path / "hive", snap_et="15:55", cap=cap,
        env_file="C:/atx/.env", index=index,
    )


def _verify_args(tmp_path, *, from_date, to_date, hive, dry_run=False, index="SPY",
                 min_cell_fraction=orch.DEFAULT_MIN_CELL_FRACTION, max_absent=None):
    return types.SimpleNamespace(
        from_date=from_date, to_date=to_date, snap_et="15:55", hive=hive, dry_run=dry_run,
        admin_exe="ADMIN.exe", db_prefix=str(tmp_path / "surface-db" / "sp100"), index=index,
        min_cell_fraction=min_cell_fraction, max_absent=max_absent,
    )


def _make_hive_with_sessions(tmp_path, dates, name="hive"):
    hive = tmp_path / name
    for d in dates:
        p = hive / f"date={d}"
        p.mkdir(parents=True)
        (p / "data.parquet").write_bytes(b"")
    return hive


# ── rate_for_date / load_rates_csv ──────────────────────────────────────────

def test_rate_for_date_lookup():
    rates = orch.load_rates_csv(RATES_CSV)
    assert rates["2022-01"] == pytest.approx(0.0015)
    assert orch.rate_for_date(rates, "2022-01-15") == pytest.approx(0.0015)
    assert orch.rate_for_date(rates, "2024-09-30") == pytest.approx(0.0480)


def test_rate_for_date_missing_month_fails_closed():
    rates = {"2022-01": 0.0015}
    with pytest.raises(ValueError):
        orch.rate_for_date(rates, "2099-01-01")


def test_load_rates_csv_ignores_header_comment_lines():
    rates = orch.load_rates_csv(RATES_CSV)
    # The header-comment block (operator-refinable note) must not become a
    # bogus "month" entry, and the real header row must not either.
    assert "month" not in rates
    assert not any(k.startswith("#") for k in rates)
    assert rates["2026-07"] == pytest.approx(0.0430)


# ── chunk_sessions (addendum §A: month AND snapshot-minute grouping) ────────

def test_sessions_chunked_within_month():
    sessions = ["2022-01-28", "2022-01-31", "2022-02-01", "2022-02-02",
                "2022-02-03", "2022-02-04"]
    assert orch.chunk_sessions(sessions, 4) == [
        ["2022-01-28", "2022-01-31"],
        ["2022-02-01", "2022-02-02", "2022-02-03", "2022-02-04"],
    ]


def test_chunks_respect_max_size():
    sessions = [f"2022-05-{d:02d}" for d in range(2, 9)]  # 7 sessions, one month
    chunks = orch.chunk_sessions(sessions, 4)
    assert [len(c) for c in chunks] == [4, 3]
    assert sum(chunks, []) == sessions


def test_chunk_splits_on_dst_transition_even_within_one_month():
    # 2022 spring-forward is the 2nd Sunday of March (2022-03-13): the whole
    # week of 03-07..03-11 is EST (20:55Z), 03-14..03-18 is EDT (19:55Z) --
    # same calendar month, different snapshot minute. A chunk size large
    # enough to hold all 10 sessions must still split on the DST boundary.
    sessions = ["2022-03-07", "2022-03-08", "2022-03-09", "2022-03-10", "2022-03-11",
                "2022-03-14", "2022-03-15", "2022-03-16", "2022-03-17", "2022-03-18"]
    chunks = orch.chunk_sessions(sessions, 10)
    assert chunks == [
        ["2022-03-07", "2022-03-08", "2022-03-09", "2022-03-10", "2022-03-11"],
        ["2022-03-14", "2022-03-15", "2022-03-16", "2022-03-17", "2022-03-18"],
    ]


# ── year_of / year_roots_partition ──────────────────────────────────────────

def test_year_roots_partition():
    dates = ["2022-12-30", "2022-12-31", "2023-01-01", "2023-01-02",
             "2023-01-03", "2023-01-04"]
    roots = orch.year_roots_partition(dates)
    assert roots == {
        2022: ["2022-12-30", "2022-12-31"],
        2023: ["2023-01-01", "2023-01-02", "2023-01-03", "2023-01-04"],
    }


# ── bisect_chunk ─────────────────────────────────────────────────────────────

def test_bisect_chunk():
    assert orch.bisect_chunk(["a", "b", "c", "d"]) == (["a", "b"], ["c", "d"])
    with pytest.raises(ValueError):
        orch.bisect_chunk(["a"])


# ── verify_thresholds ────────────────────────────────────────────────────────

def test_verify_thresholds():
    """FIX-I-2, CONTRACT CHANGE (deliberate). This test used to assert
    ``min_cells == int(0.7 * expected)`` and ``max_absent == expected -
    min_cells`` -- i.e. it locked in as contract the very thing the review
    found: ``max_absent`` was not an absent-cell budget at all but the
    arithmetic COMPLEMENT of a 70% coverage floor, which made it 3183 for the
    2025 root against an observed 325. The new contract is two INDEPENDENT
    thresholds, because they detect different things: ``min_cells`` is a
    grid-size floor (``cells_checked`` counts holes) and ``max_absent`` is the
    destroyed-surface detector.

    FIX-IMPORTANT-1, SECOND CONTRACT CHANGE (deliberate). ``max_absent`` used to
    be ``ceil(0.04 * expected)`` -- purely proportional, so the per-session
    allowance was the SAME 4.08 cells whether the walk covered 1 session or 104.
    Absences are not uniform across sessions, so that ceiling sat essentially at
    the MEAN of a spread distribution and short windows false-alarmed. It is now
    an upper tail bound on the same rate -- ``mu + z*sigma`` for
    ``Binomial(expected, 0.04)`` -- plus the exactly-known latched-absent count.
    The rate is UNCHANGED at 0.04; only the shape of the bound changed."""
    min_cells, max_absent = orch.verify_thresholds(10, 20)
    expected = 10 * 20
    # floor(0.95*200)=190; 0.04*200 = 8 mean, sigma = sqrt(200*.04*.96) = 2.7713,
    # so ceil(8 + 3*2.7713) = ceil(16.3138) = 17.
    assert (min_cells, max_absent) == (190, 17)
    assert max_absent != expected - min_cells, "must no longer be the floor's complement"
    assert max_absent > math.ceil(orch.DEFAULT_MAX_ABSENT_FRACTION * expected), \
        "the tail bound must sit strictly above the bare mean it replaced"


def test_verify_thresholds_per_session_allowance_shrinks_as_the_window_grows():
    """FIX-IMPORTANT-1, the shape itself, stated as a property rather than as
    numbers. A purely proportional ceiling has a CONSTANT per-session allowance;
    that is exactly why it cannot be right at both ends. The tail bound's
    per-session allowance must be LARGE for a one-session walk (absences cluster,
    so one session can legitimately carry far more than the mean) and CONVERGE
    DOWN toward the mean as the window grows (concentration), which is what makes
    the same threshold usable on a 1-session resume and a 140-session year."""
    per_session = [orch.verify_thresholds(102, n)[1] / n for n in (1, 2, 4, 10, 40, 140)]
    assert per_session == sorted(per_session, reverse=True), \
        "the per-session allowance must be monotonically non-increasing in window length"
    assert per_session[0] > 2 * per_session[-1], \
        "a one-session walk must get materially more headroom than a whole year"
    mean_rate = orch.DEFAULT_MAX_ABSENT_FRACTION * 102
    assert per_session[-1] > mean_rate, "and must never fall below the mean it is bounding"
    # The old shape, for the record: essentially the same per-session allowance
    # at every window length -- which is exactly why it could not be right at
    # both ends of the range.
    old = [math.ceil(0.04 * 102 * n) / n for n in (1, 2, 4, 10, 40, 140)]
    assert max(old) - min(old) < 1.0
    assert old[0] - old[-1] < per_session[0] - per_session[-1]


def test_verify_thresholds_credits_the_latched_absences_exactly():
    """FIX-IMPORTANT-1's other half. A latched absence is the provider
    CONFIRMING a symbol had no data that session -- the absent-latch invariant
    ``underlyings_on_disk | absent_latched == universe`` treats it as complete
    (2025-11-24 is legitimately complete at 95 of 102). Those cells are known
    a priori, per session, so they are credited EXACTLY rather than being
    charged against a modelled budget."""
    _mc, base = orch.verify_thresholds(102, 1)
    _mc, credited = orch.verify_thresholds(102, 1, latched_absent=7)
    assert credited == base + 7, "latched cells are added one-for-one, not modelled"
    # An operator-supplied absolute --max-absent still wins over everything.
    assert orch.verify_thresholds(102, 1, max_absent=3, latched_absent=7)[1] == 3


def test_verify_thresholds_max_absent_can_actually_fire_on_the_production_baselines():
    """The point of the finding: the default ceiling must sit ABOVE the two
    observed baselines (so a healthy re-verify of the landed roots still exits
    0) and BELOW the review's failure scenario (400 surfaces destroyed across 4
    dates, 325 -> 725), so that scenario turns the verdict ABSENT / exit 4
    instead of exiting 0. Task 8 Gate 3: sp100-2025 is 325 absent of 102x104 =
    10608; sp100-2026 is 358 of 102x140 = 14280."""
    for n_sessions, observed in ((104, 325), (140, 358)):
        _min_cells, max_absent = orch.verify_thresholds(102, n_sessions)
        assert max_absent > observed, "a healthy production root must still verify ok"
        assert max_absent < observed + 400, "400 destroyed surfaces must turn the verdict"
    # And the old 0.7-complement values, for the record, could not do either.
    # (`absent_sigma_z=0` recovers the bare-mean shape those numbers came from,
    # so this historical record survives FIX-IMPORTANT-1's change of shape.)
    assert orch.verify_thresholds(102, 104, max_absent_fraction=0.30, absent_sigma_z=0.0)[1] == 3183
    assert orch.verify_thresholds(102, 140, max_absent_fraction=0.30, absent_sigma_z=0.0)[1] == 4284


def test_verify_thresholds_are_operator_overridable():
    # An absolute --max-absent wins over the fraction entirely (the C++ tool's
    # own advice: "wire the expected count into the script with --max-absent N").
    assert orch.verify_thresholds(102, 104, max_absent=325)[1] == 325
    # ... and the grid-size floor is a separate knob.
    assert orch.verify_thresholds(102, 104, min_cell_fraction=0.7)[0] == 7425
    assert orch.verify_thresholds(102, 104, min_cell_fraction=0.99)[0] == 10501


def test_build_parser_exposes_both_verify_thresholds():
    """Before this fix ``build_parser()`` exposed neither, so an operator could
    not TIGHTEN the only automated destroyed-surface detector without editing
    the source -- which is a large part of why it shipped inert."""
    ap = orch.build_parser()
    base = ["--universe", "u.csv", "--hive", "H", "--db-prefix", "P",
            "--from", "2022-01-03", "--to", "2022-01-04",
            "--build-exe", "B.exe", "--admin-exe", "A.exe", "--log-dir", "L"]
    defaults = ap.parse_args(base)
    assert defaults.min_cell_fraction == orch.DEFAULT_MIN_CELL_FRACTION
    assert defaults.max_absent is None
    overridden = ap.parse_args(base + ["--max-absent", "325", "--min-cell-fraction", "0.98"])
    assert overridden.max_absent == 325
    assert overridden.min_cell_fraction == pytest.approx(0.98)


# ── SpendLedger ──────────────────────────────────────────────────────────────

def test_spend_ledger_abort(tmp_path):
    ledger = orch.SpendLedger(path=tmp_path / "spend.csv", abort_threshold=100.0)
    ledger.record("2022-01-01", "2022-01-31", 40.0)
    ledger.record("2022-02-01", "2022-02-28", 40.0)
    assert ledger.cumulative == pytest.approx(80.0)
    with pytest.raises(SystemExit):
        ledger.record("2022-03-01", "2022-03-31", 30.0)  # 110 > 100
    assert ledger.cumulative == pytest.approx(110.0)


def test_spend_ledger_writes_one_row_per_invocation(tmp_path):
    path = tmp_path / "spend.csv"
    ledger = orch.SpendLedger(path=path, abort_threshold=1000.0)
    ledger.record("2022-01-01", "2022-01-31", 10.0)
    ledger.record("2022-02-01", "2022-02-28", 5.0)
    with open(path, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    assert len(rows) == 2
    assert rows[0]["start"] == "2022-01-01" and rows[0]["end"] == "2022-01-31"
    assert float(rows[0]["estimate"]) == pytest.approx(10.0)
    assert float(rows[0]["cumulative"]) == pytest.approx(10.0)
    assert float(rows[1]["cumulative"]) == pytest.approx(15.0)
    assert rows[0]["ts"]  # non-empty timestamp


# ── month_bounds ─────────────────────────────────────────────────────────────

def test_month_bounds_splits_and_clips_to_the_window():
    assert orch.month_bounds("2022-01-15", "2022-03-10") == [
        ("2022-01-15", "2022-01-31"),
        ("2022-02-01", "2022-02-28"),
        ("2022-03-01", "2022-03-10"),
    ]


def test_month_bounds_single_month():
    assert orch.month_bounds("2026-07-06", "2026-07-10") == [("2026-07-06", "2026-07-10")]


# ── parse_estimate_line / parse_failed_dates / parse_minute_mismatch_dates ──

def test_parse_estimate_line():
    stdout = (
        "universe=51 sessions=21 ...\n"
        "\nFREE preflight (metadata.get_cost -- no egress):\n"
        "  2022-01-03: 51 syms est=$0.001200  (unit $0.00002353/sym-day)\n"
        "\nESTIMATE (remaining spend): $12.3456 = $0.00002353/sym-day x 525 cells (cap $90.00)\n"
        "\nAuthorized estimate $12.3456 within cap $90.00. Symbols kept: 51 (dropped 0).\n"
    )
    assert orch.parse_estimate_line(stdout) == pytest.approx(12.3456)


def test_parse_estimate_line_nothing_to_pull_is_zero():
    stdout = "universe=51 sessions=21 ...\nALL boards already on disk -- nothing to pull, $0.00.\n"
    assert orch.parse_estimate_line(stdout) == pytest.approx(0.0)


def test_parse_estimate_line_missing_is_an_error():
    with pytest.raises(ValueError):
        orch.parse_estimate_line("garbage, no estimate line here\n")


def test_parse_failed_dates():
    stderr = (
        "  2022-01-05: pull retry 1: timeout\n"
        "  2022-01-05: FAILED after retries -- left for a later resume\n"
        "  2022-01-06: FAILED after retries -- left for a later resume\n"
    )
    assert orch.parse_failed_dates(stderr) == ["2022-01-05", "2022-01-06"]


def test_parse_minute_mismatch_dates_tolerates_have_mixed():
    stderr = (
        "MINUTE-MISMATCH 2022-03-11 have=19:55 want=20:55 \u2014 repull\n"
        "MINUTE-MISMATCH 2022-03-14 have=mixed want=19:55 \u2014 repull\n"
    )
    assert orch.parse_minute_mismatch_dates(stderr) == ["2022-03-11", "2022-03-14"]


# ── truncate_universe ────────────────────────────────────────────────────────

def test_truncate_universe():
    entries = [("SPY", 100.0), ("NVDA", 90.0), ("MSFT", 80.0), ("AAPL", 70.0)]
    assert orch.truncate_universe(entries, 2) == [("SPY", 100.0), ("NVDA", 90.0)]
    assert orch.truncate_universe(entries, None) == entries
    assert orch.truncate_universe(entries, 100) == entries


# ── FIX-I-7: chunk minute uniformity is ASSERTED, not merely documented ─────
#
# ``build_build_command`` derives ``--r`` and ``--snapshot-suffix`` from
# ``chunk[0]`` alone and then passes ``--from c0 --to c1`` -- a RANGE. The C++
# CLI re-derives its own date list from the hive between those bounds, so any
# hive date inside the window that ``chunk_sessions`` excluded is swept in and
# stamped with the chunk's suffix. Until this fix the invariant that made that
# safe ("valid for the whole chunk by chunk_sessions's own grouping invariant")
# was prose in a docstring and checked nowhere -- and it had already leaked
# once, with 2025-12-24 (an early close, excluded from chunking) falling inside
# the window 2025-12-23..2025-12-30, benign only because that date has no hive
# file and December is uniformly EST.

def test_build_build_command_rejects_a_chunk_that_straddles_a_dst_boundary():
    rates = orch.load_rates_csv(RATES_CSV)
    # 2022-03-13 is the spring-forward Sunday: 2022-03-11 is EST (20:55Z),
    # 2022-03-14 is EDT (19:55Z). One suffix cannot be right for both, and
    # before this fix chunk[0]'s 20:55Z was silently applied to both dates.
    chunk = ["2022-03-11", "2022-03-14"]
    with pytest.raises(ValueError) as exc:
        orch.build_build_command(
            build_exe="BUILD.exe", hive="H", db_prefix="P", year=2022, chunk=chunk,
            symbols=["SPY"], index_symbol="SPY", rates=rates, fit_workers=0,
            report_path="r.csv", snap_et="15:55",
        )
    msg = str(exc.value)
    assert "19:55" in msg and "20:55" in msg
    assert "2022-03-11" in msg and "2022-03-14" in msg


def test_build_build_command_checks_the_whole_range_not_only_the_chunk_list():
    """The list-vs-range half. ``chunk`` is the session LIST the orchestrator
    selected; ``--from``/``--to`` is the RANGE the C++ CLI re-enumerates the
    hive over, and the two are not the same set. This chunk's own two dates
    agree on the minute (both EST) yet the range between them spans an entire
    EDT summer, so every hive date in the middle would be stamped an hour
    wrong. Checking only ``chunk`` misses it; checking the range does not.
    (``chunk_sessions`` would never emit this chunk -- it groups by calendar
    month first -- which is precisely why the range check has to stand on its
    own rather than lean on that invariant.)"""
    rates = orch.load_rates_csv(RATES_CSV)
    chunk = ["2022-03-11", "2022-11-07"]  # both EST: 20:55Z. Between them: EDT.
    assert orch.snapshot_minute_utc(chunk[0], "15:55") == orch.snapshot_minute_utc(chunk[1], "15:55")
    with pytest.raises(ValueError) as exc:
        orch.build_build_command(
            build_exe="BUILD.exe", hive="H", db_prefix="P", year=2022, chunk=chunk,
            symbols=["SPY"], index_symbol="SPY", rates=rates, fit_workers=0,
            report_path="r.csv", snap_et="15:55",
        )
    assert "19:55" in str(exc.value) and "20:55" in str(exc.value)


def test_assert_snapshot_minute_uniform_returns_the_single_minute():
    assert orch.assert_snapshot_minute_uniform(
        ["2022-07-05", "2022-07-06", "2022-07-07", "2022-07-08"], "15:55") == "19:55"
    assert orch.assert_snapshot_minute_uniform(["2022-12-01", "2022-12-02"], "15:55") == "20:55"


def test_chunk_sessions_output_always_satisfies_the_uniformity_assertion():
    """Every chunk the orchestrator actually produces over a DST-crossing
    window must pass the assertion -- i.e. the new check cannot fire on a
    legitimate run. Both 2022 transitions plus both surrounding months."""
    for lo, hi in [("2022-03-01", "2022-03-31"), ("2022-10-25", "2022-11-15")]:
        sessions = orch.trading_sessions_excluding_early_close(lo, hi, "15:55")
        for chunk in orch.chunk_sessions(sessions, 4, snap_et="15:55"):
            orch.assert_snapshot_minute_uniform(chunk, "15:55")  # must not raise


# ── command construction: one EDT chunk, one EST chunk ──────────────────────

def test_build_build_command_edt_chunk_resolves_r_and_snapshot_suffix():
    rates = orch.load_rates_csv(RATES_CSV)
    chunk = ["2022-07-05", "2022-07-06", "2022-07-07", "2022-07-08"]  # July -> EDT
    argv = orch.build_build_command(
        build_exe="build/bin/atx-vol-surface-db-build.exe", hive="C:/atx-data/opra-hive",
        db_prefix="C:/atx-data/surface-db/sp100", year=2022, chunk=chunk,
        symbols=["SPY", "AAPL"], index_symbol="SPY", rates=rates, fit_workers=0,
        report_path="C:/atx-data/logs/sp100/build_2022_2022-07-05_2022-07-08.csv",
        snap_et="15:55",
    )
    assert argv == [
        "build/bin/atx-vol-surface-db-build.exe",
        "--db", "C:/atx-data/surface-db/sp100-2022",
        "--hive", "C:/atx-data/opra-hive",
        "--from", "2022-07-05", "--to", "2022-07-08",
        "--symbols", "SPY,AAPL",
        "--index", "SPY",
        "--preset", "populate",
        "--r", "0.023000",
        "--fit-workers", "0",
        "--snapshot-suffix", "T19:55:00Z",
        "--report", "C:/atx-data/logs/sp100/build_2022_2022-07-05_2022-07-08.csv",
    ]


def test_build_build_command_est_chunk_resolves_r_and_snapshot_suffix():
    rates = orch.load_rates_csv(RATES_CSV)
    chunk = ["2022-12-01", "2022-12-02"]  # December -> EST
    argv = orch.build_build_command(
        build_exe="build/bin/atx-vol-surface-db-build.exe", hive="C:/atx-data/opra-hive",
        db_prefix="C:/atx-data/surface-db/sp100", year=2022, chunk=chunk,
        symbols=["SPY", "AAPL"], index_symbol="SPY", rates=rates, fit_workers=4,
        report_path="C:/atx-data/logs/sp100/build_2022_2022-12-01_2022-12-02.csv",
        snap_et="15:55",
    )
    assert argv == [
        "build/bin/atx-vol-surface-db-build.exe",
        "--db", "C:/atx-data/surface-db/sp100-2022",
        "--hive", "C:/atx-data/opra-hive",
        "--from", "2022-12-01", "--to", "2022-12-02",
        "--symbols", "SPY,AAPL",
        "--index", "SPY",
        "--preset", "populate",
        "--r", "0.043000",
        "--fit-workers", "4",
        "--snapshot-suffix", "T20:55:00Z",
        "--report", "C:/atx-data/logs/sp100/build_2022_2022-12-01_2022-12-02.csv",
    ]


def test_build_pull_command():
    argv = orch.build_pull_command(
        python_exe=sys.executable, pull_tool="atx-vol/tools/pull_opra_hive.py",
        universe_path="U.csv", start="2022-01-01", end="2022-01-31",
        hive="HIVE", snap_et="15:55", cap=90.0, env_file="C:/atx/.env",
        index_symbol="SPY",
    )
    assert argv == [
        sys.executable, "atx-vol/tools/pull_opra_hive.py",
        "--universe", "U.csv", "--start", "2022-01-01", "--end", "2022-01-31",
        "--out", "HIVE", "--snap-et", "15:55", "--cap", "90", "--index-symbol", "SPY",
        "--env-file", "C:/atx/.env",
    ]


def test_build_pull_command_dry_run_appends_flag():
    argv = orch.build_pull_command(
        python_exe=sys.executable, pull_tool="p.py", universe_path="U.csv",
        start="2022-01-01", end="2022-01-31", hive="HIVE", snap_et="15:55",
        cap=90.0, env_file="C:/atx/.env", index_symbol="SPY", dry_run=True,
    )
    assert argv[-1] == "--dry-run"


def test_build_pull_command_never_passes_retry_empty(tmp_path, monkeypatch):
    """FIX-IMPORTANT-2's spend rider, pinned at the orchestrator boundary.
    `--retry-empty` clears a settled-empty latch and RE-BILLS the date. Recovery
    from a settled-empty date is operator-initiated only, so no automated
    orchestrator invocation -- ever, in any phase, dry-run or not -- may carry
    that flag. This is the assertion that catches someone "helpfully" wiring it
    into the retry ladder later."""
    for dry_run in (False, True):
        argv = orch.build_pull_command(
            python_exe=sys.executable, pull_tool="p.py", universe_path="U.csv",
            start="2022-01-01", end="2022-01-31", hive="HIVE", snap_et="15:55",
            cap=90.0, env_file="C:/atx/.env", index_symbol="SPY", dry_run=dry_run,
        )
        assert "--retry-empty" not in argv and "--force" not in argv

    # ... and the same for every command `phase_pull` actually spawns.
    spawned = []

    def fake_run(argv, **kw):
        spawned.append(list(argv))
        if "--dry-run" in argv:
            return _FakeCompleted(0, stdout="ESTIMATE (remaining spend): $0.0000 = ...\n")
        return _FakeCompleted(0, stdout="DONE boards_written=0 dates_written=0\n")

    monkeypatch.setattr(subprocess, "run", fake_run)
    args = _pull_args(tmp_path, from_date="2026-07-01", to_date="2026-07-10")
    ledger = orch.SpendLedger(path=tmp_path / "ledger.csv", abort_threshold=1000.0)
    assert orch.phase_pull(args, {}, ledger, tmp_path) == 0
    assert spawned, "the phase must actually have spawned something to be meaningful"
    assert not any("--retry-empty" in argv for argv in spawned)


def test_build_verify_command():
    argv = orch.build_verify_command(admin_exe="build/bin/atx-vol-surface-db.exe",
                                     db_prefix="C:/atx-data/surface-db/sp100", year=2022,
                                     min_cells=140, max_absent=60)
    assert argv == [
        "build/bin/atx-vol-surface-db.exe", "verify",
        "--db", "C:/atx-data/surface-db/sp100-2022",
        "--min-cells", "140", "--max-absent", "60",
    ]


def test_build_verify_command_scopes_the_walk_to_the_key_range():
    """FIX-N-2. The thresholds are sized off a SESSION SET; the C++ walk must be
    restricted to that same set or the comparison is between two different
    populations. `verify` takes --from/--to (`surface_db_main.cpp`, setting
    `verify_spec.key_lo/key_hi`), and the partition loop applies them BEFORE
    `cells_checked`/`cells_absent` are incremented, so both counters land on the
    scoped set."""
    argv = orch.build_verify_command(admin_exe="ADMIN", db_prefix="PREFIX", year=2026,
                                     min_cells=1646, max_absent=70,
                                     key_lo="2026-07-01", key_hi="2026-07-24")
    assert argv == [
        "ADMIN", "verify", "--db", "PREFIX-2026",
        "--min-cells", "1646", "--max-absent", "70",
        "--from", "2026-07-01", "--to", "2026-07-24",
    ]


def test_build_info_command():
    argv = orch.build_info_command(admin_exe="ADMIN", db_prefix="PREFIX", year=2023)
    assert argv == ["ADMIN", "info", "--db", "PREFIX-2023"]


def test_build_query_command():
    argv = orch.build_query_command(admin_exe="ADMIN", db_prefix="PREFIX", year=2022,
                                    key="2022-07-24", symbol="SPY", strike=740, tenor=0.0833)
    assert argv == ["ADMIN", "query", "--db", "PREFIX-2022", "--key", "2022-07-24",
                    "--symbol", "SPY", "--strike", "740", "--tenor", "0.0833"]


# ── atm_strike_from_forward / pick_spot_check_symbols / parse_query_field ───

def test_atm_strike_from_forward_matches_task1_example():
    # Task 1's own committed example: forward=741.148... -> strike 740 was used.
    assert orch.atm_strike_from_forward(741.14846087468391, step=5.0) == 740.0


def test_pick_spot_check_symbols():
    symbols = ["SPY", "NVDA", "MSFT", "AAPL"]
    assert orch.pick_spot_check_symbols(symbols, "SPY", 2) == ["SPY", "NVDA", "MSFT"]


def test_parse_query_field():
    stdout = (
        "key 2026-07-24\nsymbol SPY\nstrike 740\ntenor 0.0833\n"
        "iv 0.1525641177446623\ntotal_variance 0.0019388749749331587\n"
        "forward 741.14846087468391\nuid 1478221309\nn_slices 33\n"
    )
    assert orch.parse_query_field(stdout, "forward") == pytest.approx(741.14846087468391)
    assert orch.parse_query_field(stdout, "iv") == pytest.approx(0.1525641177446623)
    assert orch.parse_query_field(stdout, "nonexistent") is None


# ── hive_sessions_present ────────────────────────────────────────────────────

def test_hive_sessions_present(tmp_path):
    (tmp_path / "date=2022-07-05").mkdir()
    (tmp_path / "date=2022-07-05" / "data.parquet").write_bytes(b"")
    (tmp_path / "date=2022-07-06").mkdir()  # no data.parquet -- not present
    (tmp_path / "date=2022-08-01").mkdir()
    (tmp_path / "date=2022-08-01" / "data.parquet").write_bytes(b"")
    assert orch.hive_sessions_present(tmp_path, 2022) == ["2022-07-05", "2022-08-01"]


# ── early-close-aware trading calendar (addendum §C) ────────────────────────

def test_trading_sessions_excludes_early_close_sessions():
    # 2022-11-25 (day after Thanksgiving) closes 13:00 ET; a 15:55 ET snapshot
    # window falls after the close, so this session must be excluded.
    sessions = orch.trading_sessions_excluding_early_close("2022-11-21", "2022-11-28", "15:55")
    assert "2022-11-25" not in sessions
    assert "2022-11-23" in sessions  # ordinary full session in the same window


def test_trading_sessions_requires_exchange_calendars(monkeypatch):
    import builtins
    real_import = builtins.__import__

    def fake_import(name, *args, **kwargs):
        if name == "exchange_calendars":
            raise ImportError("simulated missing dependency")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", fake_import)
    with pytest.raises(SystemExit):
        orch.trading_sessions_excluding_early_close("2022-01-01", "2022-01-10", "15:55")


# ── bisect-and-retry build policy (dependency-injected executor) ───────────

def test_execute_build_chunk_with_retry_ok():
    calls = []

    def executor(chunk):
        calls.append(list(chunk))
        return 0

    failed = []
    status = orch.execute_build_chunk_with_retry(executor, ["a", "b", "c"],
                                                  max_failed_sessions=10, failed_sessions=failed)
    assert status == "ok"
    assert calls == [["a", "b", "c"]]
    assert failed == []


def test_execute_build_chunk_with_retry_total_fit_failure_aborts():
    status = orch.execute_build_chunk_with_retry(lambda chunk: 3, ["a", "b"],
                                                  max_failed_sessions=10, failed_sessions=[])
    assert status == "total_fit_failure"


def test_execute_build_chunk_with_retry_coverage_regression_aborts():
    status = orch.execute_build_chunk_with_retry(lambda chunk: 5, ["a", "b"],
                                                  max_failed_sessions=10, failed_sessions=[])
    assert status == "coverage_regression"


def test_execute_build_chunk_with_retry_bisects_on_crash():
    # A crash-like nonzero (not 0/3/5) on the whole 4-chunk bisects into two
    # halves; only the half containing "bad" keeps failing down to size 1.
    calls = []

    def executor(chunk):
        calls.append(list(chunk))
        return 9 if "bad" in chunk else 0

    failed = []
    status = orch.execute_build_chunk_with_retry(executor, ["a", "b", "bad", "d"],
                                                  max_failed_sessions=10, failed_sessions=failed)
    assert status == "ok"
    assert failed == ["bad"]
    assert ["a", "b"] in calls and ["bad", "d"] in calls and ["bad"] in calls and ["d"] in calls


def test_execute_build_chunk_with_retry_caps_permanent_failures():
    with pytest.raises(RuntimeError):
        orch.execute_build_chunk_with_retry(lambda chunk: 9, ["only"],
                                            max_failed_sessions=0, failed_sessions=[])


# ── subprocess/logging seam ──────────────────────────────────────────────────

def test_run_subprocess_dry_run_never_calls_subprocess_run(tmp_path, monkeypatch):
    def boom(*a, **k):
        raise AssertionError("subprocess.run must not be called under dry_run")

    monkeypatch.setattr(subprocess, "run", boom)
    outcome = orch.run_subprocess(["echo", "hi"], log_dir=tmp_path, tag="t1", dry_run=True)
    assert outcome.dry_run is True
    assert outcome.exit_code == 0
    log_text = (tmp_path / "orchestrator.log").read_text(encoding="utf-8")
    assert "echo" in log_text and "t1" in log_text


def test_run_subprocess_logs_command_exit_and_duration_and_tees_output(tmp_path, monkeypatch):
    class FakeCompleted:
        returncode = 5
        stdout = "some stdout\n"
        stderr = "some stderr\n"

    monkeypatch.setattr(subprocess, "run", lambda *a, **k: FakeCompleted())
    outcome = orch.run_subprocess(["mytool", "--flag"], log_dir=tmp_path, tag="mytag", dry_run=False)
    assert outcome.exit_code == 5
    assert outcome.stdout == "some stdout\n"
    log_text = (tmp_path / "orchestrator.log").read_text(encoding="utf-8")
    assert "mytool" in log_text and "--flag" in log_text and "exit=5" in log_text and "mytag" in log_text
    assert (tmp_path / "mytag.stdout.txt").read_text(encoding="utf-8") == "some stdout\n"
    assert (tmp_path / "mytag.stderr.txt").read_text(encoding="utf-8") == "some stderr\n"


# ── CLI --dry-run smoke (brief Step 6): no subprocess is ever spawned ───────

def test_cli_dry_run_never_spawns_a_subprocess(tmp_path, monkeypatch, capsys):
    def boom(*a, **k):
        raise AssertionError("subprocess.run must not be called under --dry-run")

    monkeypatch.setattr(subprocess, "run", boom)
    universe = _ROOT / "data" / "universe" / "sp100_2026-07.csv"
    argv = [
        "--universe", str(universe),
        "--hive", str(tmp_path / "hive-does-not-exist"),
        "--db-prefix", str(tmp_path / "surface-db" / "pilot-dry"),
        "--from", "2026-07-06", "--to", "2026-07-10",
        "--phase", "all", "--dry-run",
        "--build-exe", "build/bin/atx-vol-surface-db-build.exe",
        "--admin-exe", "build/bin/atx-vol-surface-db.exe",
        "--log-dir", str(tmp_path / "logs"),
        "--max-symbols", "3",
    ]
    code = orch.main(argv)
    assert code == 0
    out = capsys.readouterr().out
    assert "atx-vol-surface-db-build" in out
    assert "atx-vol-surface-db.exe" in out


# ═════════════════════════════════════════════════════════════════════════
# Review round 1 fixes: early-close pull windowing, hive-present verify
# thresholds/key, ledger persistence across invocations, build-report
# aggregation, and phase-driver behavioral coverage.
# ═════════════════════════════════════════════════════════════════════════

# ── CRITICAL 1: pull_windows_for_month (early closes excluded from PULL
# planning, not just build/verify) ──────────────────────────────────────────

def test_pull_windows_for_month_splits_at_early_close():
    # November 2022: Thanksgiving (Nov 24) is a holiday (not a session at
    # all); the day after (Nov 25, Black Friday) IS a session but closes
    # 13:00 ET -- a 15:55 ET snapshot never exists for it, so it must be
    # dropped entirely and never appear as (or inside) a window endpoint.
    windows = orch.pull_windows_for_month("2022-11-01", "2022-11-30", "15:55")
    assert windows == [("2022-11-01", "2022-11-23"), ("2022-11-28", "2022-11-30")]
    for w0, w1 in windows:
        assert not (w0 <= "2022-11-25" <= w1)


def test_pull_windows_for_month_no_early_close_is_one_contiguous_window():
    sessions = orch.trading_sessions_excluding_early_close("2022-01-01", "2022-01-31", "15:55")
    windows = orch.pull_windows_for_month("2022-01-01", "2022-01-31", "15:55")
    assert windows == [(sessions[0], sessions[-1])]


def test_pull_windows_for_month_all_early_close_is_empty():
    # A degenerate single-day "month" that is itself an early close yields no
    # window at all (nothing to pull -- the caller must skip it).
    assert orch.pull_windows_for_month("2022-11-25", "2022-11-25", "15:55") == []


# ── phase_pull behavioral coverage (Important 5) ────────────────────────────

def test_phase_pull_splits_at_early_close_and_never_requests_it(tmp_path, monkeypatch):
    calls = []

    def fake_run(argv, **kw):
        calls.append(list(argv))
        if "--dry-run" in argv:
            return _FakeCompleted(0, stdout="ESTIMATE (remaining spend): $1.0000 = ...\n")
        return _FakeCompleted(0, stdout="DONE boards_written=0 dates_written=0\n")

    monkeypatch.setattr(subprocess, "run", fake_run)
    args = _pull_args(tmp_path, from_date="2022-11-01", to_date="2022-11-30")
    ledger = orch.SpendLedger(path=tmp_path / "ledger.csv", abort_threshold=1000.0)
    code = orch.phase_pull(args, {}, ledger, tmp_path)
    assert code == 0

    real_calls = [c for c in calls if "--dry-run" not in c]
    starts_ends = sorted((c[c.index("--start") + 1], c[c.index("--end") + 1]) for c in real_calls)
    assert starts_ends == [("2022-11-01", "2022-11-23"), ("2022-11-28", "2022-11-30")]


def test_phase_pull_blocked_exit_aborts_before_the_next_month(tmp_path, monkeypatch):
    calls = []

    def fake_run(argv, **kw):
        calls.append(list(argv))
        if "--dry-run" in argv:
            return _FakeCompleted(0, stdout="ESTIMATE (remaining spend): $1.0000 = ...\n")
        return _FakeCompleted(3, stderr="BLOCKED: even SPY + 3 names exceed cap $90.00.\n")

    monkeypatch.setattr(subprocess, "run", fake_run)
    args = _pull_args(tmp_path, from_date="2022-01-03", to_date="2022-02-28")
    ledger = orch.SpendLedger(path=tmp_path / "ledger.csv", abort_threshold=1000.0)
    code = orch.phase_pull(args, {}, ledger, tmp_path)
    assert code == 3
    real_calls = [c for c in calls if "--dry-run" not in c]
    assert len(real_calls) == 1, "must abort before ever attempting the second month"


def test_phase_pull_exit_5_accumulates_failures_and_continues_to_the_next_month(tmp_path, monkeypatch):
    calls = []
    seen_real = {"n": 0}

    def fake_run(argv, **kw):
        calls.append(list(argv))
        if "--dry-run" in argv:
            return _FakeCompleted(0, stdout="ESTIMATE (remaining spend): $1.0000 = ...\n")
        seen_real["n"] += 1
        if seen_real["n"] == 1:
            return _FakeCompleted(5, stderr="  2022-01-05: FAILED after retries -- left for a later resume\n")
        return _FakeCompleted(0, stdout="DONE boards_written=1 dates_written=1\n")

    monkeypatch.setattr(subprocess, "run", fake_run)
    args = _pull_args(tmp_path, from_date="2022-01-03", to_date="2022-02-28")
    ledger = orch.SpendLedger(path=tmp_path / "ledger.csv", abort_threshold=1000.0)
    code = orch.phase_pull(args, {}, ledger, tmp_path)
    assert code == 1  # nonzero overall, but the run continued
    real_calls = [c for c in calls if "--dry-run" not in c]
    assert len(real_calls) == 2, "exit 5 must not stop the walk over remaining months"


def test_phase_pull_records_the_probe_estimate_into_the_ledger(tmp_path, monkeypatch):
    def fake_run(argv, **kw):
        if "--dry-run" in argv:
            return _FakeCompleted(0, stdout="ESTIMATE (remaining spend): $12.5000 = $0.01/sym-day x 10 cells\n")
        return _FakeCompleted(0, stdout="DONE boards_written=1 dates_written=1\n")

    monkeypatch.setattr(subprocess, "run", fake_run)
    args = _pull_args(tmp_path, from_date="2022-01-03", to_date="2022-01-31")
    ledger = orch.SpendLedger(path=tmp_path / "ledger.csv", abort_threshold=1000.0)
    code = orch.phase_pull(args, {}, ledger, tmp_path)
    assert code == 0
    assert ledger.cumulative == pytest.approx(12.5)
    with open(tmp_path / "ledger.csv", newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    assert len(rows) == 1
    assert float(rows[0]["estimate"]) == pytest.approx(12.5)


def test_phase_pull_spend_abort_returns_1_before_any_real_pull(tmp_path, monkeypatch):
    calls = []

    def fake_run(argv, **kw):
        calls.append(list(argv))
        if "--dry-run" in argv:
            return _FakeCompleted(0, stdout="ESTIMATE (remaining spend): $50.0000 = ...\n")
        return _FakeCompleted(0, stdout="DONE boards_written=1 dates_written=1\n")

    monkeypatch.setattr(subprocess, "run", fake_run)
    args = _pull_args(tmp_path, from_date="2022-01-03", to_date="2022-02-28")
    ledger = orch.SpendLedger(path=tmp_path / "ledger.csv", abort_threshold=10.0)  # $50 > $10
    code = orch.phase_pull(args, {}, ledger, tmp_path)
    assert code == 1
    real_calls = [c for c in calls if "--dry-run" not in c]
    assert real_calls == [], "the ledger must abort before the first real pull ever runs"


# ── IMPORTANT 3: SpendLedger persists cumulative across process restarts ───

def test_spend_ledger_seeds_cumulative_from_an_existing_file(tmp_path):
    path = tmp_path / "ledger.csv"
    first = orch.SpendLedger(path=path, abort_threshold=1000.0)
    first.record("2022-01-01", "2022-01-31", 40.0)
    first.record("2022-02-01", "2022-02-28", 10.0)
    assert first.cumulative == pytest.approx(50.0)

    # A brand-new process (new SpendLedger instance) pointed at the SAME file
    # must resume from the true running total, not reset to 0.
    second = orch.SpendLedger(path=path, abort_threshold=1000.0)
    assert second.cumulative == pytest.approx(50.0)
    second.record("2022-03-01", "2022-03-31", 5.0)
    assert second.cumulative == pytest.approx(55.0)


def test_spend_ledger_seed_tolerates_a_missing_file(tmp_path):
    ledger = orch.SpendLedger(path=tmp_path / "does_not_exist.csv", abort_threshold=1000.0)
    assert ledger.cumulative == 0.0


def test_spend_ledger_seed_tolerates_a_corrupt_file(tmp_path):
    path = tmp_path / "ledger.csv"
    path.write_text("not,a,valid,ledger\ngarbage\n", encoding="utf-8")
    ledger = orch.SpendLedger(path=path, abort_threshold=1000.0)
    assert ledger.cumulative == 0.0


# ── IMPORTANT 4: build-report CSV parsing + per-year aggregation ───────────

def test_parse_build_report_csv_reads_only_section_one(tmp_path):
    path = tmp_path / "report.csv"
    path.write_text(
        "key,value\n"
        "config.n_symbols,3\n"
        "coverage.cells_ok,10\n"
        "coverage.cells_failed,2\n"
        "coverage.cells_refit,4\n"
        "config_disabled_symbol\n"
        "AAPL\n",
        encoding="utf-8",
    )
    fields = orch.parse_build_report_csv(path)
    assert fields == {
        "config.n_symbols": "3", "coverage.cells_ok": "10",
        "coverage.cells_failed": "2", "coverage.cells_refit": "4",
    }


def test_aggregate_build_summary_sums_numeric_fields_across_chunks():
    reports = [
        {"coverage.cells_ok": "10", "coverage.cells_failed": "2"},
        {"coverage.cells_ok": "5", "coverage.cells_failed": "0", "n_dates_loaded": "3"},
    ]
    summary = orch.aggregate_build_summary(reports)
    assert summary == {"coverage.cells_ok": 15.0, "coverage.cells_failed": 2.0, "n_dates_loaded": 3.0}


def test_aggregate_build_summary_does_not_sum_non_additive_config_counters():
    """FIX-I-3(2), CONTRACT CHANGE (deliberate). The test above asserted that
    EVERY numeric key is summed, and `config.*` is a per-invocation snapshot of
    the symbol-config stage -- every invocation re-declares the whole universe
    -- so a 102-name universe over 29 chunks reported `config.n_symbols` 2958.
    Non-additive keys now reduce with `max`; `coverage.*` and `n_*` still sum."""
    reports = [
        {"config.n_symbols": "102", "config.n_skipped_existing": "102",
         "coverage.cells_ok": "400", "n_load_errors": "1"},
        {"config.n_symbols": "102", "config.n_skipped_existing": "100",
         "coverage.cells_ok": "404", "n_load_errors": "0"},
    ]
    summary = orch.aggregate_build_summary(reports)
    assert summary["config.n_symbols"] == 102.0, "the universe is 102 names, not 204"
    assert summary["config.n_skipped_existing"] == 102.0
    assert summary["coverage.cells_ok"] == 804.0
    assert summary["n_load_errors"] == 1.0


def test_dedupe_chunk_reports_drops_a_bisected_parent_in_favour_of_its_children():
    """FIX-I-3(3). `execute_build_chunk_with_retry` re-runs sub-chunks of a
    failed parent and a report is recorded for EVERY invocation, so a partial
    parent report used to be aggregated on top of both children's and every
    date in the chunk counted twice."""
    parent = ("d1", "d2", "d3", "d4")
    reports = {
        parent: {"coverage.cells_ok": "1"},         # partial, before the failure
        ("d1", "d2"): {"coverage.cells_ok": "10"},
        ("d3", "d4"): {"coverage.cells_ok": "20"},
    }
    kept = orch.dedupe_chunk_reports(reports)
    assert orch.aggregate_build_summary(kept) == {"coverage.cells_ok": 30.0}

    # Recursive: a twice-bisected chunk keeps only the quarters.
    reports2 = dict(reports)
    reports2[("d1",)] = {"coverage.cells_ok": "4"}
    reports2[("d2",)] = {"coverage.cells_ok": "5"}
    assert orch.aggregate_build_summary(orch.dedupe_chunk_reports(reports2)) == \
        {"coverage.cells_ok": 29.0}

    # No bisect: nothing is dropped.
    plain = {("a", "b"): {"coverage.cells_ok": "1"}, ("c", "d"): {"coverage.cells_ok": "2"}}
    assert orch.aggregate_build_summary(orch.dedupe_chunk_reports(plain)) == \
        {"coverage.cells_ok": 3.0}


def test_year_summary_name_is_keyed_on_the_range_not_only_the_year():
    """FIX-I-3(1). `year_summary_{year}.csv` + open("w") + a per-INVOCATION
    report list meant any sub-range resume replaced the whole-year aggregate
    with just that sub-range. It did exactly that to year_summary_2026.csv,
    which now describes 17 of 140 sessions."""
    whole = orch.year_summary_name(2026, "2026-01-02", "2026-07-31")
    resumed = orch.year_summary_name(2026, "2026-07-06", "2026-07-31")
    assert whole == "year_summary_2026_2026-01-02_2026-07-31.csv"
    assert whole != resumed, "a sub-range resume must not target the same file"


def test_write_year_summary_csv_round_trips(tmp_path):
    path = tmp_path / "year_summary_2022.csv"
    orch.write_year_summary_csv({"coverage.cells_ok": 15.0, "coverage.cells_failed": 2.0}, path)
    with open(path, newline="", encoding="utf-8") as f:
        rows = {r["key"]: r["value"] for r in csv.DictReader(f)}
    assert rows == {"coverage.cells_ok": "15.0", "coverage.cells_failed": "2.0"}


def test_phase_build_writes_a_year_summary_aggregated_from_chunk_reports(tmp_path, monkeypatch):
    hive = _make_hive_with_sessions(tmp_path, ["2022-07-05", "2022-07-06"])

    def fake_run(argv, **kw):
        report_path = pathlib.Path(argv[argv.index("--report") + 1])
        report_path.write_text(
            "key,value\ncoverage.cells_ok,2\ncoverage.cells_failed,0\nconfig_disabled_symbol\n",
            encoding="utf-8",
        )
        return _FakeCompleted(0, stdout="report ...\n")

    monkeypatch.setattr(subprocess, "run", fake_run)
    args = types.SimpleNamespace(
        from_date="2022-07-05", to_date="2022-07-06", snap_et="15:55", hive=hive,
        dry_run=False, build_exe="BUILD.exe", db_prefix=str(tmp_path / "surface-db" / "sp100"),
        index="SPY", chunk_sessions=4, fit_workers=0, max_failed_sessions=10,
    )
    code = orch.phase_build(args, ["SPY"], {"2022-07": 0.02}, tmp_path)
    assert code == 0
    summary_path = tmp_path / "year_summary_2022_2022-07-05_2022-07-06.csv"
    assert summary_path.exists()
    assert not (tmp_path / "year_summary_2022.csv").exists(), \
        "FIX-I-3: the year-keyed name is what a sub-range resume destroyed"
    with open(summary_path, newline="", encoding="utf-8") as f:
        rows = {r["key"]: r["value"] for r in csv.DictReader(f)}
    assert rows["coverage.cells_ok"] == "2.0"
    assert rows["coverage.cells_failed"] == "0.0"


def test_phase_build_sub_range_resume_does_not_destroy_the_earlier_year_summary(tmp_path,
                                                                                monkeypatch):
    """FIX-I-3(1) end to end -- the production accident, reproduced. Build the
    whole (tiny) year, then resume a strict sub-range of it in a SECOND
    phase_build call, exactly as a real resume does. The first invocation's
    aggregate must survive intact."""
    hive = _make_hive_with_sessions(tmp_path, ["2022-07-05", "2022-07-06", "2022-07-07"])

    def fake_run(argv, **kw):
        report_path = pathlib.Path(argv[argv.index("--report") + 1])
        n_dates = 1 + (dt.date.fromisoformat(argv[argv.index("--to") + 1])
                       - dt.date.fromisoformat(argv[argv.index("--from") + 1])).days
        report_path.write_text(
            f"key,value\nconfig.n_symbols,1\ncoverage.cells_ok,{n_dates}\n"
            "config_disabled_symbol\n", encoding="utf-8")
        return _FakeCompleted(0, stdout="report ...\n")

    monkeypatch.setattr(subprocess, "run", fake_run)

    def _args(from_date, to_date):
        return types.SimpleNamespace(
            from_date=from_date, to_date=to_date, snap_et="15:55", hive=hive,
            dry_run=False, build_exe="BUILD.exe",
            db_prefix=str(tmp_path / "surface-db" / "sp100"),
            index="SPY", chunk_sessions=4, fit_workers=0, max_failed_sessions=10)

    assert orch.phase_build(_args("2022-07-05", "2022-07-07"), ["SPY"], {"2022-07": 0.02},
                            tmp_path) == 0
    whole = tmp_path / "year_summary_2022_2022-07-05_2022-07-07.csv"
    assert whole.exists()
    before = whole.read_text(encoding="utf-8")

    assert orch.phase_build(_args("2022-07-07", "2022-07-07"), ["SPY"], {"2022-07": 0.02},
                            tmp_path) == 0
    resumed = tmp_path / "year_summary_2022_2022-07-07_2022-07-07.csv"
    assert resumed.exists(), "the resume must still record its own summary"
    assert whole.read_text(encoding="utf-8") == before, \
        "the whole-year aggregate must survive a sub-range resume"

    with open(whole, newline="", encoding="utf-8") as f:
        rows = {r["key"]: float(r["value"]) for r in csv.DictReader(f)}
    assert rows["coverage.cells_ok"] == 3.0     # all three sessions
    assert rows["config.n_symbols"] == 1.0      # not summed across chunks


# ── IMPORTANT 2: verify uses hive-present sessions (not the requested
# calendar) for thresholds AND the spot-check key; `info`'s exit code is no
# longer discarded ─────────────────────────────────────────────────────────

def test_phase_verify_uses_hive_present_sessions_for_thresholds_and_key(tmp_path, monkeypatch):
    # Requested window spans 3 sessions; only 2 are actually present in the hive.
    hive = _make_hive_with_sessions(tmp_path, ["2022-07-05", "2022-07-06"])
    seen = {}

    def fake_run(argv, **kw):
        if "verify" in argv:
            seen["min_cells"] = argv[argv.index("--min-cells") + 1]
            seen["max_absent"] = argv[argv.index("--max-absent") + 1]
            return _FakeCompleted(0)
        if "info" in argv:
            return _FakeCompleted(0)
        if "query" in argv:
            seen.setdefault("keys", set()).add(argv[argv.index("--key") + 1])
            if argv[argv.index("--strike") + 1] == "100":
                return _FakeCompleted(0, stdout="forward 100.0\n")
            return _FakeCompleted(0, stdout="iv 0.2\nforward 100.0\n")
        raise AssertionError(f"unexpected argv: {argv}")

    monkeypatch.setattr(subprocess, "run", fake_run)
    args = _verify_args(tmp_path, from_date="2022-07-05", to_date="2022-07-07", hive=hive)
    code = orch.phase_verify(args, [("SPY", 100.0)], tmp_path)
    assert code == 0
    # n_symbols=1, n_sessions=2 (HIVE-PRESENT, not the 3 requested) ->
    # expected=2, min_cells=floor(0.95*2)=1, max_absent=ceil(0.04*2)=1 (FIX-I-2).
    assert seen["min_cells"] == "1"
    assert seen["max_absent"] == "1"
    assert seen["keys"] == {"2022-07-06"}, "key must be the last HIVE-PRESENT session, not 07-07"


def test_phase_verify_passes_an_operator_supplied_max_absent_through(tmp_path, monkeypatch):
    """FIX-I-2: the override has to reach the argv, not just the parser."""
    hive = _make_hive_with_sessions(tmp_path, ["2022-07-05", "2022-07-06"])
    seen = {}

    def fake_run(argv, **kw):
        if "verify" in argv:
            seen["max_absent"] = argv[argv.index("--max-absent") + 1]
            seen["min_cells"] = argv[argv.index("--min-cells") + 1]
            return _FakeCompleted(0)
        if "info" in argv:
            return _FakeCompleted(0)
        return _FakeCompleted(0, stdout="iv 0.2\nforward 100.0\n")

    monkeypatch.setattr(subprocess, "run", fake_run)
    args = _verify_args(tmp_path, from_date="2022-07-05", to_date="2022-07-06", hive=hive,
                        max_absent=325, min_cell_fraction=0.5)
    assert orch.phase_verify(args, [("SPY", 100.0)], tmp_path) == 0
    assert seen["max_absent"] == "325"
    assert seen["min_cells"] == "1"


def test_phase_verify_surfaces_the_absent_over_limit_exit_4(tmp_path, monkeypatch):
    """FIX-I-2, the other half: nothing anywhere drove `verdict ABSENT` (exit 4)
    through the orchestrator, so even a threshold that COULD fire had no test
    proving the orchestrator notices when it does. Exit 4 is the verdict a
    destroyed-surface growth produces."""
    hive = _make_hive_with_sessions(tmp_path, ["2022-07-05"])

    def fake_run(argv, **kw):
        if "verify" in argv:
            return _FakeCompleted(4, stderr="verdict ABSENT: cells_absent 725 > --max-absent 425\n")
        if "info" in argv:
            return _FakeCompleted(0)
        return _FakeCompleted(0, stdout="iv 0.2\nforward 100.0\n")

    monkeypatch.setattr(subprocess, "run", fake_run)
    args = _verify_args(tmp_path, from_date="2022-07-05", to_date="2022-07-05", hive=hive)
    assert orch.phase_verify(args, [("SPY", 100.0)], tmp_path) == 1, \
        "a verify that exits 4 (ABSENT) must fail the phase, not be swallowed"


# ── FIX-N-2: the threshold's denominator and the count it is compared against
# must cover the SAME set of sessions ──────────────────────────────────────

def _fake_verify_over_an_absent_map(sessions, absent_per_session, n_symbols):
    """A stand-in for the C++ ``verify`` that honours ``--from``/``--to`` the
    way `surface_db_admin.cpp` does: the partition walk is restricted to the
    key range FIRST, and only then are ``cells_checked`` / ``cells_absent``
    incremented -- so both counters describe the scoped set. With no range on
    the argv it walks the whole database, which is precisely the N-2 bug."""

    def fake_run(argv, **kw):
        if "verify" in argv:
            lo = argv[argv.index("--from") + 1] if "--from" in argv else sessions[0]
            hi = argv[argv.index("--to") + 1] if "--to" in argv else sessions[-1]
            walked = [d for d in sessions if lo <= d <= hi]
            cells_absent = sum(absent_per_session[d] for d in walked)
            cells_checked = n_symbols * len(walked)
            if cells_absent > int(argv[argv.index("--max-absent") + 1]):
                return _FakeCompleted(4, stderr=f"verdict ABSENT: cells_absent {cells_absent} > "
                                                f"--max-absent {argv[argv.index('--max-absent') + 1]}\n")
            if cells_checked < int(argv[argv.index("--min-cells") + 1]):
                return _FakeCompleted(1, stderr=f"verdict FAILED: cells_checked {cells_checked}\n")
            return _FakeCompleted(0)
        if "info" in argv:
            return _FakeCompleted(0)
        if argv[argv.index("--strike") + 1] == "100":
            return _FakeCompleted(0, stdout="forward 100.0\n")
        return _FakeCompleted(0, stdout="iv 0.2\nforward 100.0\n")

    return fake_run


# ── the landed roots' REAL per-session absence distributions ─────────────────
#
# FIX-IMPORTANT-1. The fixture these tests used to run on spread `sp100-2026`'s
# 358 absences with `absent[sessions[i % len(sessions)]] += 1`, i.e. a dead-flat
# 2-3 per session. That distribution CANNOT exceed a per-session ceiling of 5 at
# any window length, so the "even a one-session resume must pass" assertion below
# was true of the fixture and unfalsifiable by it -- the fixture was idealized in
# precisely the dimension the threshold is sensitive to, which is how a defect
# that fires on 12 of 101 real four-session windows passed its own gate twice.
#
# These are the REAL counts, read off the landed read-only roots under
# C:\atx-data and pinned here so the distribution cannot silently flatten again:
#
#   absent[date]  = 102 - <surfaces>   from `atx-vol-surface-db partitions --db
#                                      C:\atx-data\surface-db\sp100-<year>`
#   latched[date] = len(symbols)       from C:\atx-data\opra-hive\_absent\<date>.json
#
# sp100-2025: 104 sessions, 325 absent, 0..12 per session, 8 latched.
# sp100-2026: 140 sessions, 358 absent, 0..7  per session, 48 latched.
#
# Both partition lists are date-for-date equal to
# `trading_sessions_excluding_early_close(<first>, <last>, "15:55")`, which is
# what lets these counts be attached positionally below.

_REAL_ABSENT_2025 = (
    "6 3 1 2 4 2 3 8 4 2 3 2 3 4 2 4 5 3 1 3 4 2 7 1 2 6 6 3 4 8 2 3 1 4 2 2 2 "
    "4 3 1 7 2 2 3 7 3 2 1 2 1 4 4 2 1 5 2 1 2 3 1 1 0 2 4 2 3 3 6 6 3 4 1 5 3 "
    "4 3 2 1 3 5 12 1 0 2 4 1 2 0 3 1 4 3 4 4 4 2 4 6 6 4 2 6 1 1 "
)
_REAL_LATCHED_2025 = {"2025-10-30": 1, "2025-11-24": 7}

_REAL_ABSENT_2026 = (
    "2 4 3 3 3 1 3 3 4 0 1 1 3 1 1 6 3 0 6 2 4 5 4 1 3 3 2 0 1 3 2 3 1 3 4 2 3 "
    "4 4 1 4 2 5 3 1 6 5 1 2 0 3 5 1 3 1 5 2 2 1 0 3 0 4 2 2 5 1 3 1 2 2 4 1 3 "
    "3 3 0 1 1 5 4 1 3 3 3 5 1 1 1 1 2 1 3 1 1 4 2 5 1 7 2 2 4 6 2 3 2 2 3 1 2 "
    "3 3 2 1 3 3 1 3 3 2 5 1 4 2 2 3 4 2 3 1 3 3 4 3 3 3 3 4 1 "
)
_REAL_LATCHED_2026 = {
    "2026-01-05": 1, "2026-05-21": 1, "2026-05-22": 1, "2026-05-26": 1,
    "2026-05-27": 1, "2026-05-28": 1, "2026-05-29": 1, "2026-06-01": 2,
    "2026-06-02": 1, "2026-06-03": 1, "2026-06-04": 1, "2026-06-05": 1,
    "2026-06-08": 1, "2026-06-09": 1, "2026-06-10": 1, "2026-06-11": 1,
    "2026-06-12": 1, "2026-06-15": 1, "2026-06-16": 1, "2026-06-17": 1,
    "2026-06-18": 1, "2026-06-22": 1, "2026-06-23": 1, "2026-06-24": 1,
    "2026-06-25": 1, "2026-06-26": 1, "2026-06-29": 2, "2026-06-30": 1,
    "2026-07-01": 2, "2026-07-02": 1, "2026-07-06": 1, "2026-07-07": 1,
    "2026-07-08": 1, "2026-07-09": 1, "2026-07-10": 1, "2026-07-13": 1,
    "2026-07-14": 1, "2026-07-15": 1, "2026-07-16": 1, "2026-07-17": 1,
    "2026-07-20": 1, "2026-07-21": 1, "2026-07-22": 1, "2026-07-23": 1,
    "2026-07-24": 1,
}

_LANDED_ROOTS = {
    2025: ("2025-08-01", "2025-12-31", _REAL_ABSENT_2025, _REAL_LATCHED_2025),
    2026: ("2026-01-02", "2026-07-24", _REAL_ABSENT_2026, _REAL_LATCHED_2026),
}

_N_SYMBOLS = 102
_UNIVERSE = [(f"S{i:03d}", 100.0) for i in range(_N_SYMBOLS)]


def _landed_root_fixture(tmp_path, year, extra_absent=None):
    """A hive + absence map reproducing a landed root's REAL shape: 102 symbols
    x its real sessions, with each session's real absent-cell count and its real
    ``_absent/<date>.json`` latch sidecar written to disk (so ``phase_verify``
    reads the latch the same way it does in production).

    ``extra_absent`` adds destroyed surfaces at named dates."""
    first, last, absent_str, latched = _LANDED_ROOTS[year]
    sessions = orch.trading_sessions_excluding_early_close(first, last, "15:55")
    counts = [int(x) for x in absent_str.split()]
    assert len(sessions) == len(counts), (
        f"{year}: the pinned real absent counts ({len(counts)}) no longer line up with "
        f"the calendar ({len(sessions)}) -- re-derive them from the landed root")
    absent = dict(zip(sessions, counts))

    hive = _make_hive_with_sessions(tmp_path, sessions, name=f"hive{year}")
    for date, n_latched in latched.items():
        assert n_latched <= absent[date], (
            f"{date}: a latched absence is a cell that can never be filled, so it must "
            f"be a subset of that session's absent cells")
        _write_absent_sidecar(hive, date, n_latched)

    for date, extra in (extra_absent or {}).items():
        absent[date] += extra
    return sessions, hive, absent, _N_SYMBOLS, list(_UNIVERSE)


def _write_absent_sidecar(hive, date, n_symbols_absent, minute=None):
    """The exact on-disk shape `pull_opra_hive.py` writes (`_write_absent_sidecar`):
    one JSON per date holding one snapshot minute's confirmed-absent symbols."""
    path = hive / "_absent" / f"{date}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({
        "minute_utc": minute or orch.snapshot_minute_utc(date, "15:55"),
        "symbols": [s for s, _ in _UNIVERSE[:n_symbols_absent]],
        "asof": "2026-07-28T23:25:51.459675+00:00",
    }), encoding="utf-8")


def test_phase_verify_sub_range_resume_does_not_false_alarm_on_a_healthy_root(tmp_path, monkeypatch):
    """FIX-N-2, the regression. `verify_thresholds` sizes `max_absent` off the
    sessions in `--from`/`--to`, but `build_verify_command` passed NO date scope,
    so the C++ walk counted `cells_absent` over the WHOLE database. The operator
    guide's own prescribed workflow -- "then --phase verify with the SAME
    --from/--to" -- therefore returned `verdict ABSENT` / exit 4 on a healthy
    production root for any resume under ~88 of `sp100-2026`'s 140 sessions:

        max_absent = ceil(0.04 * 102 * 17) = 70   vs   whole-DB cells_absent 358

    Nothing is wrong with the database. It fails CLOSED, so it is not a data
    risk -- it is worse in a different way: it trains an operator to ignore exit
    4, the one verdict FIX-I-2 exists to make meaningful."""
    sessions, hive, absent, n_symbols, universe = _landed_root_fixture(tmp_path, 2026)
    monkeypatch.setattr(subprocess, "run",
                        _fake_verify_over_an_absent_map(sessions, absent, n_symbols))

    # The whole-year verify was always green and must stay green.
    whole = _verify_args(tmp_path, from_date="2026-01-02", to_date="2026-07-24", hive=hive)
    assert orch.phase_verify(whole, universe, tmp_path) == 0, \
        "the healthy full-year root must verify green"

    # The resume-verify the guide prescribes: 17 sessions of 140.
    resume = _verify_args(tmp_path, from_date="2026-07-01", to_date="2026-07-24", hive=hive)
    assert orch.phase_verify(resume, universe, tmp_path) == 0, \
        "a sub-range resume-verify must size and count over the SAME session set"

    # And the pathological end of the old window: a single-session resume.
    one = _verify_args(tmp_path, from_date="2026-07-24", to_date="2026-07-24", hive=hive)
    assert orch.phase_verify(one, universe, tmp_path) == 0, \
        "even a one-session resume must not be judged against the whole DB's absent count"


@pytest.mark.parametrize("year", [2025, 2026])
def test_phase_verify_every_short_window_of_the_landed_roots_verifies_green(
        tmp_path, monkeypatch, year):
    """FIX-IMPORTANT-1, the regression, driven over the REAL per-session absence
    distributions rather than an invented flat one.

    `--max-absent` was `ceil(0.04 * n_symbols * n_sessions)` -- a YEAR-AVERAGED
    rate applied to CONCENTRATED absences. Real per-session counts run 0..12 on
    `sp100-2025` against a one-session ceiling of `ceil(0.04*102) = 5`, so a short
    resume-verify returned `verdict ABSENT` / exit 4 on a root with nothing wrong
    with it. Measured on the landed roots at HEAD~:

        sp100-2025:  1 session -> 14 of 104 windows false-alarm
                     4 sessions -> 12 of 101   <- `--chunk-sessions` DEFAULTS to 4,
                     5 sessions -> 10 of 100      and the operator guide's own
                    11 sessions ->  1 of  94      production invocation uses 4
        sp100-2026:  1 session ->  5 of 140
                     2 sessions ->  2 of 139

    The guide prescribes exactly this workflow: a build chunk fails, the operator
    resumes it, then runs `--phase verify` with the SAME `--from`/`--to`. This
    sweeps EVERY window of every length up to 12 over both landed roots, so it
    cannot pass by picking a lucky window, and it fails loudly on a flattened
    fixture (a uniform distribution has no window that stresses the ceiling)."""
    sessions, hive, absent, n_symbols, universe = _landed_root_fixture(tmp_path, year)
    monkeypatch.setattr(subprocess, "run",
                        _fake_verify_over_an_absent_map(sessions, absent, n_symbols))

    # Guard the fixture itself: a flat distribution makes this test vacuous.
    counts = [absent[d] for d in sessions]
    assert max(counts) >= 2 * (sum(counts) / len(counts)), \
        "the fixture's absences must be CLUSTERED -- a flat one cannot falsify this"

    failures = []
    for width in (1, 2, 3, 4, 5, 8, 11, 12):
        for i in range(len(sessions) - width + 1):
            window = sessions[i:i + width]
            args = _verify_args(tmp_path, from_date=window[0], to_date=window[-1], hive=hive)
            if orch.phase_verify(args, universe, tmp_path) != 0:
                failures.append((width, window[0], window[-1],
                                 sum(absent[d] for d in window)))
    assert failures == [], (
        f"{len(failures)} healthy window(s) of the landed sp100-{year} root were judged "
        f"ABSENT; worst: {max(failures, key=lambda f: f[3])}")


def test_phase_verify_short_windows_still_fire_on_destroyed_surfaces(tmp_path, monkeypatch):
    """FIX-IMPORTANT-1 must not re-inert FIX-I-2, and I-2's property is now the
    standing contract: the review's destruction scenario -- 400 surfaces
    destroyed across 4 dates by a whole-file partition rewrite -- must still turn
    the verdict ABSENT and exit 4, at EVERY scope the operator might verify at.

    This is the assertion that stops the threshold being "loosened until the
    false alarms stop", which would re-open I-2."""
    destroyed = {"2026-07-20": 100, "2026-07-21": 100, "2026-07-22": 100, "2026-07-23": 100}
    sessions, hive, absent, n_symbols, universe = _landed_root_fixture(
        tmp_path, 2026, extra_absent=destroyed)
    monkeypatch.setattr(subprocess, "run",
                        _fake_verify_over_an_absent_map(sessions, absent, n_symbols))

    for lo, hi, why in (
        ("2026-07-20", "2026-07-23", "the 4-session chunk that was destroyed"),
        ("2026-07-01", "2026-07-24", "a 17-session resume containing it"),
        ("2026-01-02", "2026-07-24", "the whole-year verify"),
    ):
        args = _verify_args(tmp_path, from_date=lo, to_date=hi, hive=hive)
        assert orch.phase_verify(args, universe, tmp_path) == 1, \
            f"400 destroyed surfaces must fail {why}"


def test_phase_verify_credits_the_latched_absences_of_the_sessions_in_scope(
        tmp_path, monkeypatch):
    """FIX-IMPORTANT-1's design, at its load-bearing point. 2025-11-24 is
    legitimately COMPLETE at 95 of 102 underlyings -- the absent-latch invariant
    `underlyings_on_disk | absent_latched == universe` holds, and its
    `_absent/2025-11-24.json` names the 7 confirmed-absent symbols. It carries 12
    absent DB cells, which is the single worst session of either landed root.

    Without the latch credit, that ONE session is the only one in either root that
    a one-session verify still false-alarms on. Sizing off the latch is what makes
    the check mean "surfaces were destroyed" rather than "this window is sparse",
    and this test is what stops the credit being quietly dropped as redundant."""
    sessions, hive, absent, n_symbols, universe = _landed_root_fixture(tmp_path, 2025)
    assert absent["2025-11-24"] == 12 and _REAL_LATCHED_2025["2025-11-24"] == 7
    monkeypatch.setattr(subprocess, "run",
                        _fake_verify_over_an_absent_map(sessions, absent, n_symbols))

    args = _verify_args(tmp_path, from_date="2025-11-24", to_date="2025-11-24", hive=hive)
    assert orch.phase_verify(args, universe, tmp_path) == 0

    # Delete the sidecar and the SAME session must now fail: the credit is real,
    # it comes from disk, and it is what carries this date.
    (hive / "_absent" / "2025-11-24.json").unlink()
    assert orch.phase_verify(args, universe, tmp_path) == 1, \
        "without the latch this session is over the ceiling -- the credit is load-bearing"


def test_latched_absent_cells_reads_the_sidecars_and_fails_safe(tmp_path):
    """The latch reader is the only new input to a SAFETY threshold, so every way
    it can be wrong must make the ceiling TIGHTER (credit 0), never looser."""
    hive = tmp_path / "hive"
    uni = [s for s, _ in _UNIVERSE]

    def latched(dates):
        return orch.latched_absent_cells(hive, dates, uni, "15:55")

    _write_absent_sidecar(hive, "2025-11-24", 7)                       # EST -> 20:55
    _write_absent_sidecar(hive, "2025-10-30", 1)                       # EDT -> 19:55
    assert latched(["2025-11-24", "2025-10-30"]) == 8
    # A session with no sidecar contributes nothing.
    assert latched(["2025-11-25"]) == 0
    # A sidecar stamped at a DIFFERENT snapshot minute describes different data.
    _write_absent_sidecar(hive, "2025-11-25", 5, minute="19:55")
    assert latched(["2025-11-25"]) == 0
    # Symbols outside this run's universe are not cells of this database.
    (hive / "_absent" / "2025-11-26.json").write_text(json.dumps(
        {"minute_utc": "20:55", "symbols": ["S000", "NOT_IN_UNIVERSE"]}), encoding="utf-8")
    assert latched(["2025-11-26"]) == 1
    # Corrupt / unreadable / missing hive -> no credit, never an exception.
    (hive / "_absent" / "2025-11-27.json").write_text("{not json", encoding="utf-8")
    assert latched(["2025-11-27"]) == 0
    assert orch.latched_absent_cells(tmp_path / "nope", ["2025-11-24"], uni, "15:55") == 0


def test_phase_verify_scopes_the_verify_walk_to_the_sized_session_set(tmp_path, monkeypatch):
    """FIX-N-2, directly: the range must reach argv, and it must be the bounds
    of the population the thresholds were SIZED off (the hive-present sessions),
    not the raw requested calendar. 2022-07-04 is a holiday and 2022-07-08 is
    absent from this hive, so the two differ at both ends."""
    hive = _make_hive_with_sessions(tmp_path, ["2022-07-05", "2022-07-06", "2022-07-07"])
    seen = {}

    def fake_run(argv, **kw):
        if "verify" in argv:
            seen["from"] = argv[argv.index("--from") + 1]
            seen["to"] = argv[argv.index("--to") + 1]
            seen["max_absent"] = argv[argv.index("--max-absent") + 1]
            return _FakeCompleted(0)
        if "info" in argv:
            return _FakeCompleted(0)
        return _FakeCompleted(0, stdout="iv 0.2\nforward 100.0\n")

    monkeypatch.setattr(subprocess, "run", fake_run)
    args = _verify_args(tmp_path, from_date="2022-07-01", to_date="2022-07-08", hive=hive)
    assert orch.phase_verify(args, [("SPY", 100.0)], tmp_path) == 0
    assert (seen["from"], seen["to"]) == ("2022-07-05", "2022-07-07"), \
        "the walk must be scoped to the hive-present sessions the thresholds were sized off"
    # expected = 1 x 3, no latched absences -> max_absent = ceil(0.04*3 + 3*sqrt(3*.04*.96))
    # = ceil(0.12 + 1.018) = 2, over exactly those 3 partitions. (FIX-IMPORTANT-1
    # changed this from the bare mean's ceil(0.04*3) = 1.)
    assert seen["max_absent"] == "2"


def test_phase_verify_dry_run_falls_back_to_requested_sessions_needing_no_hive(tmp_path, monkeypatch):
    def boom(*a, **k):
        raise AssertionError("subprocess.run must not be called under --dry-run")

    monkeypatch.setattr(subprocess, "run", boom)
    args = _verify_args(tmp_path, from_date="2022-07-05", to_date="2022-07-06",
                        hive=tmp_path / "hive-does-not-exist", dry_run=True)
    code = orch.phase_verify(args, [("SPY", 100.0)], tmp_path)
    assert code == 0


def test_phase_verify_surfaces_a_nonzero_info_exit_code(tmp_path, monkeypatch):
    hive = _make_hive_with_sessions(tmp_path, ["2022-07-05"])

    def fake_run(argv, **kw):
        if "verify" in argv:
            return _FakeCompleted(0)
        if "info" in argv:
            return _FakeCompleted(1, stderr="info: db root not found\n")
        if "query" in argv:
            if argv[argv.index("--strike") + 1] == "100":
                return _FakeCompleted(0, stdout="forward 100.0\n")
            return _FakeCompleted(0, stdout="iv 0.2\nforward 100.0\n")
        raise AssertionError(f"unexpected argv: {argv}")

    monkeypatch.setattr(subprocess, "run", fake_run)
    args = _verify_args(tmp_path, from_date="2022-07-05", to_date="2022-07-05", hive=hive)
    code = orch.phase_verify(args, [("SPY", 100.0)], tmp_path)
    assert code == 1, "a failed `info` invocation must no longer be silently discarded"


def test_phase_verify_finite_spot_check_iv_passes(tmp_path, monkeypatch):
    hive = _make_hive_with_sessions(tmp_path, ["2022-07-05"])

    def fake_run(argv, **kw):
        if "verify" in argv or "info" in argv:
            return _FakeCompleted(0)
        if "query" in argv:
            if argv[argv.index("--strike") + 1] == "100":
                return _FakeCompleted(0, stdout="forward 741.148\n")
            return _FakeCompleted(0, stdout="iv 0.1525641177446623\nforward 741.148\n")
        raise AssertionError(f"unexpected argv: {argv}")

    monkeypatch.setattr(subprocess, "run", fake_run)
    args = _verify_args(tmp_path, from_date="2022-07-05", to_date="2022-07-05", hive=hive)
    code = orch.phase_verify(args, [("SPY", 100.0)], tmp_path)
    assert code == 0


def test_phase_verify_non_finite_spot_check_iv_fails(tmp_path, monkeypatch):
    hive = _make_hive_with_sessions(tmp_path, ["2022-07-05"])

    def fake_run(argv, **kw):
        if "verify" in argv or "info" in argv:
            return _FakeCompleted(0)
        if "query" in argv:
            if argv[argv.index("--strike") + 1] == "100":
                return _FakeCompleted(0, stdout="forward 741.148\n")
            return _FakeCompleted(0, stdout="iv nan\nforward 741.148\n")
        raise AssertionError(f"unexpected argv: {argv}")

    monkeypatch.setattr(subprocess, "run", fake_run)
    args = _verify_args(tmp_path, from_date="2022-07-05", to_date="2022-07-05", hive=hive)
    code = orch.phase_verify(args, [("SPY", 100.0)], tmp_path)
    assert code == 1


# ═══════════════════════════════════════════════════════════════════════════
# Wall-clock guard (--session-timeout) and symbol sharding (--symbol-shards)
#
# WHY THESE EXIST. `run_subprocess` shipped with NO `timeout=`, and
# `execute_build_chunk_with_retry` branched only on the exit code, so a
# pathological board had nothing capping it. Measured, from the live
# orchestrator log (C:/atx-data/logs/xsec-fit/orchestrator.log):
#
#   2026-08-18T21:56:17.286253+00:00 tag=build_2026_2026-06-29_2026-06-29
#   exit=1073807364 duration_s=74573.328 cmd='build-rel\bin\atx-vol-surface-db-build.exe' ...
#
# ONE single-session invocation at 616 names ran 74573 s (20.7 h) and then
# crashed with 0x40000364. Nothing halved it, nothing killed it, and the year
# it belonged to was blocked for the whole time.
# ═══════════════════════════════════════════════════════════════════════════


class _FakePopen:
    """Stands in for ``subprocess.Popen`` on the TIMEOUT path.

    ``run_subprocess`` only ever touches ``.pid``, ``.communicate(timeout=)``,
    ``.kill()`` and ``.returncode``, so those are all this models. Raising
    ``TimeoutExpired`` from the FIRST ``communicate`` and returning on the
    second is exactly the real object's contract: after a timeout the caller
    must kill and then reap, or the child becomes a zombie holding the pipes."""

    def __init__(self, *, timeout_first_call=True, returncode=0, stdout="", stderr=""):
        self.pid = 4242
        self._timeout_first_call = timeout_first_call
        self._calls = 0
        self.returncode = returncode
        self._stdout = stdout
        self._stderr = stderr
        self.kill_calls = 0

    def communicate(self, timeout=None):
        self._calls += 1
        if self._timeout_first_call and self._calls == 1:
            raise subprocess.TimeoutExpired(cmd="fake", timeout=timeout)
        return (self._stdout, self._stderr)

    def kill(self):
        self.kill_calls += 1


def _build_args(tmp_path, *, session_timeout=None, symbol_shards=1, chunk_sessions=4,
                dry_run=False, max_failed_sessions=10):
    return types.SimpleNamespace(
        from_date="2026-06-01", to_date="2026-06-30", snap_et="15:55", dry_run=dry_run,
        hive=tmp_path / "hive", db_prefix=str(tmp_path / "db" / "xsec"),
        build_exe="BUILD.exe", index="SPY", fit_workers=0,
        chunk_sessions=chunk_sessions, max_failed_sessions=max_failed_sessions,
        log_dir=tmp_path / "logs", session_timeout=session_timeout,
        symbol_shards=symbol_shards,
    )


# ── chunk_timeout_s: the per-SESSION budget, scaled by chunk size ───────────

def test_chunk_timeout_scales_by_session_count():
    # The operator-set budget is PER TRADING DAY (10 min). A chunk covering N
    # sessions therefore gets N x that, so the flag's semantic stays "a day fit
    # must not exceed 10 minutes" no matter how the date axis is chunked.
    assert orch.chunk_timeout_s(1, 600.0) == pytest.approx(600.0)
    assert orch.chunk_timeout_s(4, 600.0) == pytest.approx(2400.0)
    assert orch.chunk_timeout_s(8, 600.0) == pytest.approx(4800.0)


def test_chunk_timeout_disabled_by_non_positive_budget():
    # An explicit opt-out must be expressible -- and must mean "no cap at all"
    # (None), never "cap of zero seconds", which would kill every invocation.
    assert orch.chunk_timeout_s(4, 0.0) is None
    assert orch.chunk_timeout_s(4, -1.0) is None
    assert orch.chunk_timeout_s(4, None) is None


def test_default_session_timeout_is_ten_minutes():
    assert orch.DEFAULT_SESSION_TIMEOUT_S == pytest.approx(600.0)


def test_timeout_exit_code_is_not_an_aborting_status():
    # 3 (total_fit_failure) and 5 (coverage_regression) ABORT the year. A
    # timeout must NOT be either: it has to fall into the crash-shaped branch
    # so the bisect ladder halves it.
    assert orch.BUILD_TIMEOUT_EXIT not in (0, 3, 5)


# ── timeout routing into the bisect ladder ─────────────────────────────────

def test_timeout_routes_into_the_bisect_ladder_not_an_abort():
    """A slow 4-session chunk gets HALVED, exactly like a crash-shaped exit."""
    chunk = ["2026-06-01", "2026-06-02", "2026-06-03", "2026-06-04"]
    seen: list[tuple[str, ...]] = []

    def executor(sub_chunk):
        seen.append(tuple(sub_chunk))
        # The whole chunk times out; each half is fine.
        return orch.BUILD_TIMEOUT_EXIT if len(sub_chunk) == 4 else 0

    failed: list[str] = []
    timed_out: list[str] = []
    status = orch.execute_build_chunk_with_retry(
        executor, chunk, max_failed_sessions=10, failed_sessions=failed,
        timed_out_sessions=timed_out)

    assert status == "ok"                      # NOT an abort
    assert seen[0] == tuple(chunk)             # tried whole first
    assert tuple(chunk[:2]) in seen            # then both halves
    assert tuple(chunk[2:]) in seen
    assert failed == []                        # halves succeeded: nothing failed
    assert timed_out == []


def test_single_session_timeout_lands_in_failed_and_timed_out_sessions():
    """A chunk that is ALREADY one session and still breaches the budget cannot
    be halved further -- it is recorded, not fatal, and it is recorded in the
    dedicated timeout list too so an operator can be sent to investigate it."""
    failed: list[str] = []
    timed_out: list[str] = []
    status = orch.execute_build_chunk_with_retry(
        lambda _c: orch.BUILD_TIMEOUT_EXIT, ["2026-06-29"],
        max_failed_sessions=10, failed_sessions=failed, timed_out_sessions=timed_out)

    assert status == "ok"
    assert failed == ["2026-06-29"]      # same treatment as a single-session crash
    assert timed_out == ["2026-06-29"]   # AND separately flagged for investigation


def test_single_session_crash_does_not_populate_the_timeout_list():
    """A crash is not a timeout. The two must stay distinguishable."""
    failed: list[str] = []
    timed_out: list[str] = []
    orch.execute_build_chunk_with_retry(
        lambda _c: 1073807364, ["2026-06-29"],
        max_failed_sessions=10, failed_sessions=failed, timed_out_sessions=timed_out)
    assert failed == ["2026-06-29"]
    assert timed_out == []


def test_timeout_still_respects_max_failed_sessions():
    """The spend/abort guard is not weakened by the new routing."""
    failed: list[str] = []
    timed_out: list[str] = []
    with pytest.raises(RuntimeError, match="max-failed-sessions"):
        orch.execute_build_chunk_with_retry(
            lambda _c: orch.BUILD_TIMEOUT_EXIT,
            ["2026-06-01", "2026-06-02", "2026-06-03", "2026-06-04"],
            max_failed_sessions=2, failed_sessions=failed, timed_out_sessions=timed_out)


def test_bisect_ladder_still_works_without_a_timeout_list():
    """``timed_out_sessions`` is optional: the pre-guard call shape still works."""
    failed: list[str] = []
    status = orch.execute_build_chunk_with_retry(
        lambda _c: 1073807364, ["2026-06-29"], max_failed_sessions=10,
        failed_sessions=failed)
    assert status == "ok"
    assert failed == ["2026-06-29"]


# ── run_subprocess: kill the TREE, log it distinctly ───────────────────────

def test_run_subprocess_timeout_kills_the_whole_process_tree(tmp_path, monkeypatch):
    fake = _FakePopen()
    killed: list[int] = []
    monkeypatch.setattr(subprocess, "Popen", lambda *a, **k: fake)
    monkeypatch.setattr(orch, "_kill_process_tree", lambda p: killed.append(p.pid))

    out = orch.run_subprocess(["slow.exe"], log_dir=tmp_path, tag="t", timeout_s=600.0)

    # The GRANDCHILDREN matter: subprocess.run(timeout=) kills only the direct
    # child, so the tree killer is the whole point of this path.
    assert killed == [fake.pid], "the process TREE must be killed, not just proc.kill()"
    assert out.timed_out is True
    assert out.exit_code == orch.BUILD_TIMEOUT_EXIT
    # ...and the child must be REAPED after the kill, or it zombies on the pipes.
    assert fake._calls == 2


def test_run_subprocess_timeout_logs_distinctly_from_a_crash(tmp_path, monkeypatch):
    monkeypatch.setattr(subprocess, "Popen", lambda *a, **k: _FakePopen())
    monkeypatch.setattr(orch, "_kill_process_tree", lambda p: None)
    orch.run_subprocess(["slow.exe"], log_dir=tmp_path, tag="slow", timeout_s=600.0)

    line = (tmp_path / "orchestrator.log").read_text(encoding="utf-8")
    # An investigation signal, not a flaky exit -- it must be greppable as such.
    assert "TIMEOUT" in line
    assert "timeout_s=600.000" in line
    assert f"exit={orch.BUILD_TIMEOUT_EXIT}" in line


def test_run_subprocess_without_timeout_keeps_the_old_subprocess_run_path(tmp_path, monkeypatch):
    """Back-compat: ``timeout_s=None`` must not change how anything spawns."""
    monkeypatch.setattr(subprocess, "run",
                        lambda *a, **k: _FakeCompleted(0, stdout="hi", stderr=""))
    monkeypatch.setattr(subprocess, "Popen",
                        lambda *a, **k: pytest.fail("must not Popen without a timeout"))
    out = orch.run_subprocess(["x.exe"], log_dir=tmp_path, tag="t")
    assert out.exit_code == 0
    assert out.timed_out is False
    assert "TIMEOUT" not in (tmp_path / "orchestrator.log").read_text(encoding="utf-8")


def test_run_subprocess_completing_under_the_budget_is_not_a_timeout(tmp_path, monkeypatch):
    fake = _FakePopen(timeout_first_call=False, returncode=0, stdout="ok", stderr="")
    monkeypatch.setattr(subprocess, "Popen", lambda *a, **k: fake)
    out = orch.run_subprocess(["fast.exe"], log_dir=tmp_path, tag="t", timeout_s=600.0)
    assert out.timed_out is False
    assert out.exit_code == 0
    assert fake.kill_calls == 0


# ── symbol sharding: exact membership ──────────────────────────────────────

def test_split_symbol_shards_loses_and_duplicates_nothing():
    symbols = [f"SYM{i:04d}" for i in range(6189)]   # the OPRA universe size
    shards = orch.split_symbol_shards(symbols, 7)
    flat = [s for shard in shards for s in shard]
    assert flat == symbols, "concatenation must reproduce the input EXACTLY, in order"
    assert len(flat) == len(set(flat)) == 6189
    # Balanced to within one name, so no shard is a hidden whole-universe run.
    assert max(len(s) for s in shards) - min(len(s) for s in shards) <= 1


def test_split_symbol_shards_balances_an_uneven_split():
    shards = orch.split_symbol_shards(list("ABCDEFGHIJ"), 3)   # 10 into 3
    assert [len(s) for s in shards] == [4, 3, 3]
    assert [x for s in shards for x in s] == list("ABCDEFGHIJ")


def test_split_symbol_shards_single_shard_is_the_identity():
    symbols = ["SPY", "QQQ", "AAPL"]
    assert orch.split_symbol_shards(symbols, 1) == [symbols]


def test_split_symbol_shards_never_emits_an_empty_shard():
    # More shards than symbols must not produce empty invocations: an empty
    # --symbols-file would build NOTHING and still cost a process.
    shards = orch.split_symbol_shards(["SPY", "QQQ"], 5)
    assert shards == [["SPY"], ["QQQ"]]
    assert all(shards)


def test_split_symbol_shards_rejects_a_non_positive_count():
    for bad in (0, -1):
        with pytest.raises(ValueError):
            orch.split_symbol_shards(["SPY"], bad)


def test_split_symbol_shards_rejects_an_empty_universe():
    with pytest.raises(ValueError):
        orch.split_symbol_shards([], 2)


def test_write_shard_symbols_file_is_one_symbol_per_line(tmp_path):
    p = tmp_path / "shard_00.txt"
    orch.write_shard_symbols_file(["SPY", "QQQ", "BRK.B"], p)
    assert p.read_text(encoding="utf-8").splitlines() == ["SPY", "QQQ", "BRK.B"]


# ── build command: --symbols-file vs inline --symbols ──────────────────────

def test_build_command_uses_symbols_file_when_sharded(tmp_path):
    argv = orch.build_build_command(
        build_exe="B.exe", hive="H", db_prefix="P", year=2026,
        chunk=["2026-06-29"], symbols=["SPY", "QQQ"], index_symbol="SPY",
        rates={"2026-06": 0.043}, fit_workers=0, report_path=tmp_path / "r.csv",
        snap_et="15:55", symbols_file=tmp_path / "shard_00.txt")
    assert "--symbols-file" in argv
    assert argv[argv.index("--symbols-file") + 1] == str(tmp_path / "shard_00.txt")
    # The two are mutually exclusive: passing both would let the CLI pick, and
    # which one wins is not this orchestrator's decision to leave open.
    assert "--symbols" not in argv


def test_build_command_without_a_shard_file_keeps_inline_symbols(tmp_path):
    argv = orch.build_build_command(
        build_exe="B.exe", hive="H", db_prefix="P", year=2026,
        chunk=["2026-06-29"], symbols=["SPY", "QQQ"], index_symbol="SPY",
        rates={"2026-06": 0.043}, fit_workers=0, report_path=tmp_path / "r.csv",
        snap_et="15:55")
    assert "--symbols" in argv
    assert argv[argv.index("--symbols") + 1] == "SPY,QQQ"
    assert "--symbols-file" not in argv


# ── timeout records reach an operator: stderr + the summary CSV ────────────

def test_timeout_records_land_in_the_year_summary_csv(tmp_path):
    rec = orch.TimeoutRecord(db="C:/atx-data/surface-db/xsec-2026", year=2026,
                             session="2026-06-29", minute="19:55", timeout_s=600.0)
    p = tmp_path / "year_summary_2026_2026-06-29_2026-06-29.csv"
    orch.write_year_summary_csv({"cells_ok": 12.0}, p, timeouts=[rec])

    rows = dict(csv.reader(p.read_text(encoding="utf-8").splitlines()[1:]))
    assert rows["cells_ok"] == "12.0"
    assert rows["n_timeout_sessions"] == "1"
    # The operator must be able to read (db, year, session, minute) straight off
    # the row -- that is the whole point of recording it here.
    detail = rows["timeout_session_00"]
    for field in ("C:/atx-data/surface-db/xsec-2026", "2026", "2026-06-29", "19:55"):
        assert field in detail


def test_year_summary_csv_without_timeouts_is_unchanged(tmp_path):
    p = tmp_path / "s.csv"
    orch.write_year_summary_csv({"cells_ok": 1.0}, p)
    rows = dict(csv.reader(p.read_text(encoding="utf-8").splitlines()[1:]))
    assert rows == {"cells_ok": "1.0"}
    assert "n_timeout_sessions" not in rows


# ── both flags together, end to end through phase_build ────────────────────

def test_phase_build_runs_one_invocation_per_shard_and_passes_the_timeout(tmp_path, monkeypatch):
    """`--symbol-shards N` must produce N SEPARATE sequential build invocations
    per chunk (so no single process holds the whole universe), each carrying the
    scaled wall-clock budget."""
    hive = _make_hive_with_sessions(tmp_path, ["2026-06-29", "2026-06-30"])
    args = _build_args(tmp_path, session_timeout=600.0, symbol_shards=3, chunk_sessions=2)
    args.hive = hive

    calls = []

    def fake_run_subprocess(argv, *, log_dir, tag, dry_run=False, timeout_s=None):
        calls.append((list(argv), timeout_s))
        return orch.SubprocessOutcome(argv=list(argv), exit_code=0, stdout="", stderr="",
                                      duration_s=0.1)

    monkeypatch.setattr(orch, "run_subprocess", fake_run_subprocess)
    rc = orch.phase_build(args, ["A", "B", "C", "D", "E", "F"], {"2026-06": 0.043},
                          tmp_path / "logs")
    assert rc == 0

    # One chunk of 2 sessions x 3 shards = 3 sequential invocations.
    assert len(calls) == 3
    # 2 sessions x 600 s/session, applied to EACH shard invocation.
    assert all(t == pytest.approx(1200.0) for _argv, t in calls)

    # Every invocation is symbols-FILE driven, and the files partition the
    # universe exactly -- nothing lost, nothing built twice.
    seen = []
    for argv, _t in calls:
        assert "--symbols-file" in argv, "a sharded run must not inline --symbols"
        f = pathlib.Path(argv[argv.index("--symbols-file") + 1])
        seen.extend(f.read_text(encoding="utf-8").split())
    assert sorted(seen) == ["A", "B", "C", "D", "E", "F"]
    assert len(seen) == len(set(seen))


def test_phase_build_unsharded_is_a_single_inline_symbols_invocation(tmp_path, monkeypatch):
    """Back-compat: the default (1 shard) keeps the pre-shard call shape."""
    hive = _make_hive_with_sessions(tmp_path, ["2026-06-29"])
    args = _build_args(tmp_path, session_timeout=600.0, symbol_shards=1, chunk_sessions=4)
    args.hive = hive
    calls = []

    def fake_run_subprocess(argv, *, log_dir, tag, dry_run=False, timeout_s=None):
        calls.append((list(argv), timeout_s))
        return orch.SubprocessOutcome(argv=list(argv), exit_code=0, stdout="", stderr="",
                                      duration_s=0.1)

    monkeypatch.setattr(orch, "run_subprocess", fake_run_subprocess)
    assert orch.phase_build(args, ["A", "B"], {"2026-06": 0.043}, tmp_path / "logs") == 0
    assert len(calls) == 1
    argv, timeout_s = calls[0]
    assert "--symbols" in argv and "--symbols-file" not in argv
    assert timeout_s == pytest.approx(600.0)   # 1 session in the chunk


def test_phase_build_timeout_reaches_stderr_and_the_summary(tmp_path, monkeypatch, capsys):
    """A single-session chunk that STILL breaches the budget must be shouted
    about: the user wants these investigated, not silently absorbed."""
    hive = _make_hive_with_sessions(tmp_path, ["2026-06-29"])
    log_dir = tmp_path / "logs"
    args = _build_args(tmp_path, session_timeout=600.0, symbol_shards=1, chunk_sessions=1)
    args.hive = hive

    def fake_run_subprocess(argv, *, log_dir, tag, dry_run=False, timeout_s=None):
        return orch.SubprocessOutcome(argv=list(argv), exit_code=orch.BUILD_TIMEOUT_EXIT,
                                      stdout="", stderr="", duration_s=601.0, timed_out=True)

    monkeypatch.setattr(orch, "run_subprocess", fake_run_subprocess)
    assert orch.phase_build(args, ["A"], {"2026-06": 0.043}, log_dir) == 0

    err = capsys.readouterr().err
    assert "TIMEOUT" in err
    assert "2026-06-29" in err
    assert "2026" in err

    summaries = list(log_dir.glob("year_summary_2026_*.csv"))
    assert summaries, "a timed-out session must still produce a summary to read"
    rows = dict(csv.reader(summaries[0].read_text(encoding="utf-8").splitlines()[1:]))
    assert rows["n_timeout_sessions"] == "1"
    assert "2026-06-29" in rows["timeout_session_00"]


def test_symbol_shards_flag_and_session_timeout_flag_parse(tmp_path):
    ap = orch.build_parser()
    args = ap.parse_args([
        "--universe", "u.csv", "--hive", "h", "--db-prefix", "p",
        "--from", "2026-01-01", "--to", "2026-01-31",
        "--build-exe", "b.exe", "--admin-exe", "a.exe", "--log-dir", str(tmp_path),
    ])
    assert args.session_timeout == pytest.approx(600.0)   # 10 min/day, operator-set
    assert args.symbol_shards == 1                        # off by default

    args2 = ap.parse_args([
        "--universe", "u.csv", "--hive", "h", "--db-prefix", "p",
        "--from", "2026-01-01", "--to", "2026-01-31",
        "--build-exe", "b.exe", "--admin-exe", "a.exe", "--log-dir", str(tmp_path),
        "--session-timeout", "900", "--symbol-shards", "7",
    ])
    assert args2.session_timeout == pytest.approx(900.0)
    assert args2.symbol_shards == 7
