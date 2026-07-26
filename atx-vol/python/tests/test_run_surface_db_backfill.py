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
import importlib.util
import pathlib
import subprocess
import sys

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
    min_cells, max_absent = orch.verify_thresholds(10, 20)
    expected = 10 * 20
    assert min_cells == int(0.7 * expected)
    assert max_absent == expected - min_cells
    assert (min_cells, max_absent) == (140, 60)


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


def test_build_verify_command():
    argv = orch.build_verify_command(admin_exe="build/bin/atx-vol-surface-db.exe",
                                     db_prefix="C:/atx-data/surface-db/sp100", year=2022,
                                     min_cells=140, max_absent=60)
    assert argv == [
        "build/bin/atx-vol-surface-db.exe", "verify",
        "--db", "C:/atx-data/surface-db/sp100-2022",
        "--min-cells", "140", "--max-absent", "60",
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
