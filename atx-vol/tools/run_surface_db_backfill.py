#!/usr/bin/env python3
"""Chunked SP100 surface-db backfill orchestrator (Task 4).

Drives the three subprocess CLIs Tasks 1-3 built -- Task 3's cost-gated OPRA
pull (``pull_opra_hive.py``), and Task 1's two C++ CLIs
(``atx-vol-surface-db-build`` / ``atx-vol-surface-db``) -- over a multi-year
window, one calendar year per SurfaceDb root. Every DECISION this script makes
(rate lookup, session chunking, verify thresholds, spend-ledger math, command
construction, the bisect-and-retry build policy) lives in a plain importable
function with no subprocess, network, or filesystem-mandatory dependency, so
the whole decision surface is unit-testable
(atx-vol/python/tests/test_run_surface_db_backfill.py) without ever spawning
the real tools. Only the thin ``run_subprocess``/phase-driver layer actually
executes anything, and every invocation -- real or ``--dry-run``-suppressed --
is logged (full command line, exit code, duration) to
``<log-dir>/orchestrator.log`` with per-invocation stdout/stderr tee files.

DST-aware build chunking (Task 4 addendum §A). The C++ hive loader applies
ONE ``snapshot_suffix`` uniformly per load call
(atx-vol/src/opra_hive.cpp:144), and that stamp feeds T-to-expiry math
(opra_panel.cpp). An ET-anchored pull (``--snap-et``) lands at 19:55Z on EDT
dates and 20:55Z on EST dates, so a build chunk must hold dates of ONE
snapshot minute only -- ``chunk_sessions`` below groups by (calendar month,
snapshot minute) before cutting to size, and DST transitions (2nd Sunday
March, 1st Sunday November) split a month into two groups even when the
literal chunk-size limit would not have.

Early-close exclusion (addendum §C). A 15:55 ET snapshot falls AFTER the
close on an XNYS early-close session (Black Friday, day-before-July-4th,
Christmas Eve when it is a session), so the pull tool never gets a real
15:55 snapshot for those dates and logs them loudly instead of sidecar-
latching them (``pull_opra_hive.py``'s ``EMPTY-SESSION`` path). This
orchestrator therefore treats "sessions requested" everywhere (pull
planning, build chunk membership, verify's expected-cell math) as the XNYS
calendar MINUS early closes (``trading_sessions_excluding_early_close``),
never the raw session list.

Usage:
  python atx-vol/tools/run_surface_db_backfill.py \\
      --universe atx-vol/data/universe/sp100_2026-07.csv \\
      --hive C:/atx-data/opra-hive --db-prefix C:/atx-data/surface-db/sp100 \\
      --from 2022-01-01 --to 2026-07-31 --phase all \\
      --snap-et 15:55 --rates atx-vol/data/rates/us_3m_monthly.csv \\
      --build-exe build/bin/atx-vol-surface-db-build.exe \\
      --admin-exe build/bin/atx-vol-surface-db.exe \\
      --chunk-sessions 4 --fit-workers 0 --cap 90 --spend-abort 95 \\
      --log-dir C:/atx-data/logs/sp100 [--dry-run] [--index SPY] [--max-symbols N]
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import importlib.util
import math
import pathlib
import re
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Callable, Optional

# ── Reuse Task 3's DST-aware snapshot-minute math and universe reader rather
# than reimplementing them (Task 4 addendum, context notes). tools/ is not an
# importable package, so load by path -- same convention
# test_pull_opra_hive.py already established.
_THIS_DIR = pathlib.Path(__file__).resolve().parent
_PULL_TOOL_PATH = _THIS_DIR / "pull_opra_hive.py"
_pull_spec = importlib.util.spec_from_file_location("pull_opra_hive", _PULL_TOOL_PATH)
_pull_mod = importlib.util.module_from_spec(_pull_spec)
sys.modules.setdefault("pull_opra_hive", _pull_mod)
_pull_spec.loader.exec_module(_pull_mod)

snapshot_minute_utc = _pull_mod.snapshot_minute_utc
read_universe = _pull_mod.read_universe
ET = _pull_mod.ET

DEFAULT_RATES_PATH = "atx-vol/data/rates/us_3m_monthly.csv"


# ── Rates table ──────────────────────────────────────────────────────────────

def load_rates_csv(path) -> dict[str, float]:
    """Parse ``month,r`` from the operator-refinable rates CSV, skipping the
    leading ``#`` header-comment block."""
    path = pathlib.Path(path)
    with open(path, "r", encoding="utf-8-sig", newline="") as f:
        lines = [ln for ln in f if not ln.lstrip().startswith("#")]
    rates: dict[str, float] = {}
    for row in csv.DictReader(lines):
        month = (row.get("month") or "").strip()
        if not month:
            continue
        rates[month] = float(row["r"])
    return rates


def rate_for_date(rates: dict[str, float], date_str: str) -> float:
    """The flat carry rate for ``date_str``'s calendar month. Fails CLOSED
    (ValueError) on a month outside the table -- never a silently-coerced
    default, for the same reason the build CLI's own ``--r`` refuses NaN/inf:
    a wrong rate silently fails every put-call-parity forward downstream."""
    month = date_str[:7]
    try:
        return rates[month]
    except KeyError:
        raise ValueError(
            f"no rate for month {month} (date {date_str}) in the rates table; "
            f"extend {DEFAULT_RATES_PATH} or pass --rates"
        ) from None


# ── Calendar: XNYS sessions, early-close-excluded (addendum §C) ────────────

def _require_exchange_calendars():
    try:
        import exchange_calendars as xcals
    except ImportError as exc:
        raise SystemExit(
            "BLOCKED: exchange_calendars is not installed. The orchestrator "
            "requires the REAL XNYS trading calendar and deliberately does not "
            "fall back to a weekday approximation -- that would silently "
            "miscount holidays across a multi-year backfill. Install "
            "exchange_calendars and retry."
        ) from exc
    return xcals


def trading_sessions_excluding_early_close(start: str, end: str, snap_et: str = "15:55") -> list[str]:
    """XNYS sessions in ``[start, end]`` MINUS early closes whose close is
    before ``snap_et`` ET (e.g. the 13:00 ET closes on Black Friday / the day
    before July 4th) -- those sessions never get a real snapshot at
    ``snap_et`` (the pull tool logs them ``EMPTY-SESSION`` and does not
    sidecar-latch them), so they must not count as expected cells anywhere in
    this orchestrator (pull planning, build chunk membership, verify
    thresholds)."""
    xcals = _require_exchange_calendars()
    cal = xcals.get_calendar("XNYS")
    sessions = cal.sessions_in_range(start, end)
    h, m = (int(x) for x in snap_et.split(":"))
    out: list[str] = []
    for session in sessions:
        close_utc = cal.schedule.loc[session, "close"]
        close_et = close_utc.tz_convert(ET)
        if (close_et.hour, close_et.minute) < (h, m):
            continue  # early close: no 15:55 snapshot ever exists for this date
        out.append(session.strftime("%Y-%m-%d"))
    return out


# ── year_of / year_roots_partition ──────────────────────────────────────────

def year_of(date_str: str) -> int:
    return int(date_str[:4])


def year_roots_partition(dates: list[str]) -> dict[int, list[str]]:
    """Partition ``dates`` (assumed sorted) into per-calendar-year groups,
    preserving order -- one SurfaceDb root per year."""
    out: dict[int, list[str]] = {}
    for d in dates:
        out.setdefault(year_of(d), []).append(d)
    return out


# ── chunk_sessions (addendum §A: month AND snapshot-minute grouping) ────────

def chunk_sessions(sessions: list[str], chunk_size: int, snap_et: str = "15:55") -> list[list[str]]:
    """Group ``sessions`` by (calendar month, snapshot minute) -- both
    constraints -- then cut each group into pieces of at most ``chunk_size``
    sessions, preserving order. A chunk therefore never spans a month
    boundary NOR a DST transition within one month (addendum §A): the C++
    hive loader stamps one uniform ``snapshot_suffix`` per load call, so a
    chunk holding both an EDT date (19:55Z) and an EST date (20:55Z) would
    silently mis-stamp one side of it."""
    groups: list[list[str]] = []
    current_key: Optional[tuple[str, str]] = None
    current: list[str] = []
    for d in sessions:
        key = (d[:7], snapshot_minute_utc(d, snap_et))
        if key != current_key:
            if current:
                groups.append(current)
            current = [d]
            current_key = key
        else:
            current.append(d)
    if current:
        groups.append(current)

    chunks: list[list[str]] = []
    for group in groups:
        for i in range(0, len(group), chunk_size):
            chunks.append(group[i:i + chunk_size])
    return chunks


# ── bisect_chunk ─────────────────────────────────────────────────────────────

def bisect_chunk(chunk: list[str]) -> tuple[list[str], list[str]]:
    """Split ``chunk`` in half. A single-session chunk cannot be split
    further -- that is the retry ladder's base case, not a bug here."""
    if len(chunk) < 2:
        raise ValueError(f"cannot bisect a single-session chunk: {chunk!r}")
    mid = len(chunk) // 2
    return chunk[:mid], chunk[mid:]


# ── verify_thresholds ────────────────────────────────────────────────────────

def verify_thresholds(n_symbols: int, n_sessions: int) -> tuple[int, int]:
    """(min_cells, max_absent) for ``atx-vol-surface-db verify``:
    ``expected = n_symbols * n_sessions``; ``min_cells = floor(0.7 *
    expected)``; ``max_absent = expected - min_cells``. ``n_sessions`` MUST
    already exclude early closes (addendum §C) -- those dates never get a
    cell, so counting them would make every healthy database look like it
    is missing coverage it was never going to have."""
    expected = n_symbols * n_sessions
    min_cells = math.floor(0.7 * expected)
    max_absent = expected - min_cells
    return min_cells, max_absent


# ── SpendLedger ──────────────────────────────────────────────────────────────

@dataclass
class SpendLedger:
    """Appends one CSV row per pull invocation (ts, start, end, estimate,
    cumulative) to ``path`` and raises SystemExit the moment the running
    total exceeds ``abort_threshold`` -- the orchestrator's OWN circuit
    breaker across the WHOLE multi-month/multi-year run, distinct from each
    pull invocation's own per-call ``--cap`` degrade."""

    path: pathlib.Path
    abort_threshold: float
    cumulative: float = 0.0

    def record(self, start: str, end: str, estimate: float) -> None:
        path = pathlib.Path(self.path)
        is_new = not path.exists()
        path.parent.mkdir(parents=True, exist_ok=True)
        self.cumulative += estimate
        with open(path, "a", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            if is_new:
                w.writerow(["ts", "start", "end", "estimate", "cumulative"])
            w.writerow([
                dt.datetime.now(dt.timezone.utc).isoformat(), start, end,
                f"{estimate:.6f}", f"{self.cumulative:.6f}",
            ])
        if self.cumulative > self.abort_threshold:
            raise SystemExit(
                f"SPEND ABORT: cumulative estimate ${self.cumulative:.4f} exceeds "
                f"--spend-abort ${self.abort_threshold:.2f} after {start}..{end} "
                f"(+${estimate:.4f}). See {path} for the full ledger."
            )


# ── month_bounds (phase pull: one subprocess per calendar month) ───────────

def month_bounds(start: str, end: str) -> list[tuple[str, str]]:
    """``[start, end]`` split into per-calendar-month ``(seg_start,
    seg_end)`` pairs, each clipped to the overall window."""
    s = dt.date.fromisoformat(start)
    e = dt.date.fromisoformat(end)
    if s > e:
        raise ValueError(f"--from {start} is after --to {end}")
    out: list[tuple[str, str]] = []
    cur = dt.date(s.year, s.month, 1)
    while cur <= e:
        if cur.month == 12:
            next_month = dt.date(cur.year + 1, 1, 1)
        else:
            next_month = dt.date(cur.year, cur.month + 1, 1)
        month_end = next_month - dt.timedelta(days=1)
        seg_start = max(cur, s)
        seg_end = min(month_end, e)
        out.append((seg_start.isoformat(), seg_end.isoformat()))
        cur = next_month
    return out


# ── stdout/stderr parsers over the pull tool's canonical output (addendum §C/D) ─

_ESTIMATE_RE = re.compile(r"ESTIMATE \(remaining spend\): \$([0-9]*\.?[0-9]+)")
_FAILED_DATE_RE = re.compile(r"^\s*(\S+): FAILED after retries", re.MULTILINE)
_MINUTE_MISMATCH_RE = re.compile(r"^MINUTE-MISMATCH (\S+) have=\S+ want=\S+", re.MULTILINE)


def parse_estimate_line(stdout: str) -> float:
    """Extract the ``$`` amount from the pull tool's own
    ``ESTIMATE (remaining spend): $X.XXXX = ...`` stdout line. A run with
    nothing to pull never prints that line at all (it short-circuits to
    ``ALL boards already on disk`` first) -- that is a genuine $0.00, not a
    parse failure."""
    if "ALL boards already on disk" in stdout:
        return 0.0
    m = _ESTIMATE_RE.search(stdout)
    if not m:
        raise ValueError("no ESTIMATE line found in pull-tool stdout")
    return float(m.group(1))


def parse_failed_dates(stderr: str) -> list[str]:
    """Dates the pull tool logged ``FAILED after retries`` on stderr
    (``pull_opra_hive.py``'s ``pull()`` -- the only place a failed date's
    identity is ever printed; the manifest CSV omits it entirely)."""
    return _FAILED_DATE_RE.findall(stderr)


def parse_minute_mismatch_dates(stderr: str) -> list[str]:
    """Dates the pull tool logged ``MINUTE-MISMATCH ... — repull`` on
    stderr. Tolerates ``have=mixed`` (addendum §D: a mixed-ts date file logs
    the literal string ``mixed`` instead of an ``HH:MM`` pair) -- the regex
    only needs the DATE, so it does not care what ``have=`` says."""
    return _MINUTE_MISMATCH_RE.findall(stderr)


def parse_query_field(stdout: str, field_name: str) -> Optional[float]:
    """One value off the admin CLI's ``query`` output (Task 1's committed
    format: ``<field> <value>``, one per line, no header). ``None`` if the
    field is absent."""
    for line in stdout.splitlines():
        parts = line.split(" ", 1)
        if len(parts) == 2 and parts[0] == field_name:
            try:
                return float(parts[1])
            except ValueError:
                return None
    return None


# ── universe truncation (pilot use) ─────────────────────────────────────────

def truncate_universe(entries: list[tuple[str, float]], max_symbols: Optional[int]) -> list[tuple[str, float]]:
    """``entries[:max_symbols]`` (preserving ``read_universe``'s priority
    order -- index leg first, then weight descending), or every entry when
    ``max_symbols`` is ``None`` or not smaller than the universe."""
    if max_symbols is None:
        return list(entries)
    return list(entries[:max_symbols])


def write_symbols_file(entries: list[tuple[str, float]], path: pathlib.Path) -> None:
    """Write a truncated universe back out as a D1-shaped CSV (``symbol,
    raw_weight``) that ``read_universe`` parses with weights intact -- used
    for ``--max-symbols`` pilot runs, since the pull tool's ``--universe``/
    ``--symbols-file`` are both PATHS and cannot take an inline symbol list."""
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["symbol", "raw_weight"])
        for sym, weight in entries:
            w.writerow([sym, weight])


# ── spot-check symbol / strike selection (verify phase) ────────────────────

def pick_spot_check_symbols(symbols: list[str], index_symbol: str, n: int = 2) -> list[str]:
    """The index leg plus the first ``n`` non-index constituents, in
    universe order -- the brief's "SPY + 2 constituents"."""
    out = [index_symbol] if index_symbol in symbols else []
    out.extend(s for s in symbols if s != index_symbol)
    return out[: (1 if index_symbol in symbols else 0) + n]


def atm_strike_from_forward(forward: float, step: float = 5.0) -> float:
    """Round ``forward`` to the nearest ``step`` -- an "ATM-ish" strike
    derived from a first probe query's own ``forward`` field (Task 1's
    query output), needing no external market-data source. Matches Task 1's
    own committed example: forward 741.148... -> strike 740 at step 5."""
    return round(forward / step) * step


# ── hive_sessions_present (glob date=* dirs) ────────────────────────────────

def hive_sessions_present(hive_root, year: int) -> list[str]:
    """Sorted list of ``YYYY-MM-DD`` dates in ``year`` that have a
    ``date=<date>/data.parquet`` file under ``hive_root`` -- "sessions
    actually present in the hive" per the brief's build-phase chunk
    membership rule."""
    root = pathlib.Path(hive_root)
    out: list[str] = []
    for p in sorted(root.glob(f"date={year}-*")):
        if (p / "data.parquet").exists():
            out.append(p.name.split("=", 1)[1])
    return out


# ── command construction ────────────────────────────────────────────────────

def build_pull_command(*, python_exe: str, pull_tool, universe_path, start: str, end: str,
                       hive, snap_et: str, cap: float, env_file: str, index_symbol: str = "SPY",
                       dry_run: bool = False) -> list[str]:
    argv = [
        str(python_exe), str(pull_tool),
        "--universe", str(universe_path),
        "--start", start, "--end", end,
        "--out", str(hive),
        "--snap-et", snap_et,
        "--cap", f"{cap:g}",
        "--index-symbol", index_symbol,
        "--env-file", str(env_file),
    ]
    if dry_run:
        argv.append("--dry-run")
    return argv


def build_build_command(*, build_exe, hive, db_prefix: str, year: int, chunk: list[str],
                        symbols: list[str], index_symbol: str, rates: dict[str, float],
                        fit_workers: int, report_path, snap_et: str = "15:55") -> list[str]:
    """One build-CLI invocation over ``chunk`` (a uniform-(month, minute)
    session group per ``chunk_sessions``). ``--r`` resolves off the chunk's
    FIRST session's calendar month; ``--snapshot-suffix`` off that same
    session's DST-resolved snapshot minute -- valid for the whole chunk by
    ``chunk_sessions``'s own grouping invariant."""
    c0, c1 = chunk[0], chunk[-1]
    r = rate_for_date(rates, c0)
    minute = snapshot_minute_utc(c0, snap_et)
    suffix = f"T{minute}:00Z"
    return [
        str(build_exe),
        "--db", f"{db_prefix}-{year}",
        "--hive", str(hive),
        "--from", c0, "--to", c1,
        "--symbols", ",".join(symbols),
        "--index", index_symbol,
        "--preset", "populate",
        "--r", f"{r:.6f}",
        "--fit-workers", str(fit_workers),
        "--snapshot-suffix", suffix,
        "--report", str(report_path),
    ]


def build_verify_command(*, admin_exe, db_prefix: str, year: int, min_cells: int, max_absent: int) -> list[str]:
    return [str(admin_exe), "verify", "--db", f"{db_prefix}-{year}",
            "--min-cells", str(min_cells), "--max-absent", str(max_absent)]


def build_info_command(*, admin_exe, db_prefix: str, year: int) -> list[str]:
    return [str(admin_exe), "info", "--db", f"{db_prefix}-{year}"]


def build_query_command(*, admin_exe, db_prefix: str, year: int, key: str, symbol: str,
                        strike, tenor) -> list[str]:
    return [str(admin_exe), "query", "--db", f"{db_prefix}-{year}",
            "--key", key, "--symbol", symbol, "--strike", str(strike), "--tenor", str(tenor)]


# ── bisect-and-retry build policy ───────────────────────────────────────────

def execute_build_chunk_with_retry(executor: Callable[[list[str]], int], chunk: list[str], *,
                                   max_failed_sessions: int, failed_sessions: list[str]) -> str:
    """Run ``executor(chunk) -> exit_code`` (one build-CLI invocation) and
    apply the brief's policy:
      * 0 -> "ok".
      * 3 -> "total_fit_failure" (a real produced-nothing result: an EMPTY
        window is exit 0, so 3 here is genuine) -- ABORTS the year.
      * 5 -> "coverage_regression" (a refused rewrite; needs human eyes) --
        ABORTS the year.
      * any other nonzero (a crash) -> ``bisect_chunk`` and retry each half
        recursively. A single-session chunk that STILL crashes this way is
        appended to ``failed_sessions`` (mutated in place) and treated as
        "ok" for the purpose of continuing the walk -- its failure is
        recorded, not fatal -- unless that pushes ``failed_sessions`` past
        ``max_failed_sessions``, which raises ``RuntimeError`` to abort the
        whole run.
    Returns the first ABORTING status seen while walking the (possibly
    bisected) chunk, or "ok" if none did."""
    code = executor(chunk)
    if code == 0:
        return "ok"
    if code == 3:
        return "total_fit_failure"
    if code == 5:
        return "coverage_regression"
    # Crash-shaped nonzero: bisect and retry.
    if len(chunk) < 2:
        failed_sessions.append(chunk[0])
        if len(failed_sessions) > max_failed_sessions:
            raise RuntimeError(
                f"build phase: {len(failed_sessions)} permanently-failed session(s) "
                f"exceeds --max-failed-sessions ({max_failed_sessions}): {failed_sessions}"
            )
        return "ok"
    left, right = bisect_chunk(chunk)
    status = execute_build_chunk_with_retry(executor, left, max_failed_sessions=max_failed_sessions,
                                            failed_sessions=failed_sessions)
    if status != "ok":
        return status
    return execute_build_chunk_with_retry(executor, right, max_failed_sessions=max_failed_sessions,
                                          failed_sessions=failed_sessions)


# ── subprocess execution + logging seam ─────────────────────────────────────

@dataclass
class SubprocessOutcome:
    argv: list[str]
    exit_code: int
    stdout: str
    stderr: str
    duration_s: float
    dry_run: bool = False


def _log_line(log_dir: pathlib.Path, tag: str, argv: list[str], exit_code: int, duration_s: float,
             dry_run: bool) -> None:
    log_dir = pathlib.Path(log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)
    prefix = "[DRY-RUN] " if dry_run else ""
    line = (f"{dt.datetime.now(dt.timezone.utc).isoformat()} {prefix}tag={tag} exit={exit_code} "
            f"duration_s={duration_s:.3f} cmd={shlex.join(str(a) for a in argv)}\n")
    with open(log_dir / "orchestrator.log", "a", encoding="utf-8") as f:
        f.write(line)


def run_subprocess(argv: list[str], *, log_dir, tag: str, dry_run: bool = False) -> SubprocessOutcome:
    """Run ``argv`` (or, under ``dry_run``, print+log the plan and execute
    NOTHING) -- every invocation logs its full command line, exit code, and
    duration to ``<log_dir>/orchestrator.log``; a real (non-dry-run)
    invocation additionally tees stdout/stderr to ``<log_dir>/<tag>.std{out,
    err}.txt``."""
    log_dir = pathlib.Path(log_dir)
    if dry_run:
        print(f"DRY-RUN would run: {shlex.join(str(a) for a in argv)}")
        _log_line(log_dir, tag, argv, 0, 0.0, dry_run=True)
        return SubprocessOutcome(argv=list(argv), exit_code=0, stdout="", stderr="",
                                 duration_s=0.0, dry_run=True)
    start = time.monotonic()
    proc = subprocess.run([str(a) for a in argv], capture_output=True, text=True)
    duration = time.monotonic() - start
    _log_line(log_dir, tag, argv, proc.returncode, duration, dry_run=False)
    log_dir.mkdir(parents=True, exist_ok=True)
    (log_dir / f"{tag}.stdout.txt").write_text(proc.stdout, encoding="utf-8")
    (log_dir / f"{tag}.stderr.txt").write_text(proc.stderr, encoding="utf-8")
    return SubprocessOutcome(argv=list(argv), exit_code=proc.returncode, stdout=proc.stdout,
                             stderr=proc.stderr, duration_s=duration, dry_run=False)


# ── phase pull ───────────────────────────────────────────────────────────────

def phase_pull(args, rates: dict[str, float], ledger: SpendLedger, log_dir: pathlib.Path) -> int:
    months = month_bounds(args.from_date, args.to_date)
    failures: list[str] = []
    for m0, m1 in months:
        tag = f"pull_{m0}_{m1}"
        real_argv = build_pull_command(
            python_exe=sys.executable, pull_tool=args.pull_tool_path,
            universe_path=args.pull_universe_path, start=m0, end=m1, hive=args.hive,
            snap_et=args.snap_et, cap=args.cap, env_file=args.env_file, index_symbol=args.index,
        )
        if args.dry_run:
            # Global --dry-run: show the plan, spawn NOTHING (not even the free
            # cost probe below) -- so a pilot dry-run needs neither a
            # DATABENTO_API_KEY nor an existing hive.
            run_subprocess(real_argv, log_dir=log_dir, tag=tag, dry_run=True)
            continue

        probe_argv = build_pull_command(
            python_exe=sys.executable, pull_tool=args.pull_tool_path,
            universe_path=args.pull_universe_path, start=m0, end=m1, hive=args.hive,
            snap_et=args.snap_et, cap=args.cap, env_file=args.env_file, index_symbol=args.index,
            dry_run=True,
        )
        probe = run_subprocess(probe_argv, log_dir=log_dir, tag=f"{tag}_estimate", dry_run=False)
        estimate = parse_estimate_line(probe.stdout)
        try:
            ledger.record(m0, m1, estimate)
        except SystemExit as exc:
            print(str(exc), file=sys.stderr)
            return 1

        res = run_subprocess(real_argv, log_dir=log_dir, tag=tag, dry_run=False)
        if res.exit_code == 3:
            print(f"BLOCKED (pull {m0}..{m1}): {res.stderr.strip()}", file=sys.stderr)
            return 3
        if res.exit_code == 5:
            month_failed = parse_failed_dates(res.stderr)
            failures.extend(month_failed)
            print(f"pull {m0}..{m1}: {len(month_failed)} failed date(s): {month_failed}",
                 file=sys.stderr)
        elif res.exit_code != 0:
            failures.append(f"{m0}..{m1} (unexpected exit {res.exit_code})")
            print(f"pull {m0}..{m1}: unexpected exit {res.exit_code}", file=sys.stderr)

    if args.dry_run:
        return 0
    if failures:
        print(f"PULL PHASE: {len(failures)} failed date(s): {failures}", file=sys.stderr)
        return 1
    return 0


# ── phase build ──────────────────────────────────────────────────────────────

def phase_build(args, symbols: list[str], rates: dict[str, float], log_dir: pathlib.Path) -> int:
    requested = trading_sessions_excluding_early_close(args.from_date, args.to_date, args.snap_et)
    roots = year_roots_partition(requested)
    overall = 0
    for year in sorted(roots):
        year_sessions = roots[year]
        if args.dry_run:
            # The pilot hive may not exist yet -- fall back to the requested
            # session set so --dry-run needs no pulled data on disk.
            present = year_sessions
        else:
            on_disk = set(hive_sessions_present(args.hive, year))
            present = [d for d in year_sessions if d in on_disk]
        if not present:
            continue

        chunks = chunk_sessions(present, args.chunk_sessions, snap_et=args.snap_et)
        failed_sessions: list[str] = []
        year_status = "ok"
        for chunk in chunks:
            c0, c1 = chunk[0], chunk[-1]
            report_path = pathlib.Path(log_dir) / f"build_{year}_{c0}_{c1}.csv"
            tag = f"build_{year}_{c0}_{c1}"
            argv = build_build_command(
                build_exe=args.build_exe, hive=args.hive, db_prefix=args.db_prefix, year=year,
                chunk=chunk, symbols=symbols, index_symbol=args.index, rates=rates,
                fit_workers=args.fit_workers, report_path=report_path, snap_et=args.snap_et,
            )
            if args.dry_run:
                run_subprocess(argv, log_dir=log_dir, tag=tag, dry_run=True)
                continue

            def executor(sub_chunk, _year=year, _log_dir=log_dir) -> int:
                sc0, sc1 = sub_chunk[0], sub_chunk[-1]
                sub_report = pathlib.Path(_log_dir) / f"build_{_year}_{sc0}_{sc1}.csv"
                sub_argv = build_build_command(
                    build_exe=args.build_exe, hive=args.hive, db_prefix=args.db_prefix, year=_year,
                    chunk=sub_chunk, symbols=symbols, index_symbol=args.index, rates=rates,
                    fit_workers=args.fit_workers, report_path=sub_report, snap_et=args.snap_et,
                )
                sub_res = run_subprocess(sub_argv, log_dir=_log_dir, tag=f"build_{_year}_{sc0}_{sc1}",
                                         dry_run=False)
                return sub_res.exit_code

            try:
                status = execute_build_chunk_with_retry(
                    executor, chunk, max_failed_sessions=args.max_failed_sessions,
                    failed_sessions=failed_sessions,
                )
            except RuntimeError as exc:
                print(str(exc), file=sys.stderr)
                return 1
            if status != "ok":
                year_status = status
                print(f"BUILD year {year} ABORTED ({status}) on chunk {c0}..{c1}", file=sys.stderr)
                break

        if failed_sessions:
            print(f"BUILD year {year}: permanently-failed session(s): {failed_sessions}",
                 file=sys.stderr)
        if year_status != "ok":
            overall = 1

    return overall


# ── phase verify ─────────────────────────────────────────────────────────────

def phase_verify(args, universe_entries: list[tuple[str, float]], log_dir: pathlib.Path) -> int:
    requested = trading_sessions_excluding_early_close(args.from_date, args.to_date, args.snap_et)
    roots = year_roots_partition(requested)
    symbols = [s for s, _ in universe_entries]
    n_symbols = len(symbols)
    overall = 0

    for year in sorted(roots):
        year_sessions = roots[year]
        min_cells, max_absent = verify_thresholds(n_symbols, len(year_sessions))

        verify_argv = build_verify_command(admin_exe=args.admin_exe, db_prefix=args.db_prefix,
                                           year=year, min_cells=min_cells, max_absent=max_absent)
        verify_res = run_subprocess(verify_argv, log_dir=log_dir, tag=f"verify_{year}",
                                    dry_run=args.dry_run)
        if not args.dry_run and verify_res.exit_code != 0:
            overall = 1
            print(f"VERIFY year {year}: verify exited {verify_res.exit_code}", file=sys.stderr)

        info_argv = build_info_command(admin_exe=args.admin_exe, db_prefix=args.db_prefix, year=year)
        run_subprocess(info_argv, log_dir=log_dir, tag=f"info_{year}", dry_run=args.dry_run)

        key = year_sessions[-1] if year_sessions else f"{year}-01-03"
        for symbol in pick_spot_check_symbols(symbols, args.index, 2):
            probe_argv = build_query_command(admin_exe=args.admin_exe, db_prefix=args.db_prefix,
                                             year=year, key=key, symbol=symbol, strike=100, tenor=0.0833)
            probe = run_subprocess(probe_argv, log_dir=log_dir, tag=f"query_{year}_{symbol}_probe",
                                   dry_run=args.dry_run)
            if args.dry_run:
                continue
            forward = parse_query_field(probe.stdout, "forward")
            if probe.exit_code != 0 or forward is None:
                overall = 1
                print(f"VERIFY year {year}: spot query for {symbol} failed or returned no forward",
                     file=sys.stderr)
                continue
            strike = atm_strike_from_forward(forward)
            final_argv = build_query_command(admin_exe=args.admin_exe, db_prefix=args.db_prefix,
                                             year=year, key=key, symbol=symbol, strike=strike,
                                             tenor=0.0833)
            final = run_subprocess(final_argv, log_dir=log_dir, tag=f"query_{year}_{symbol}",
                                   dry_run=False)
            iv = parse_query_field(final.stdout, "iv")
            if final.exit_code != 0 or iv is None or not math.isfinite(iv):
                overall = 1
                print(f"VERIFY year {year}: spot query for {symbol}@{strike} is "
                     f"non-finite/error (exit {final.exit_code})", file=sys.stderr)

    return overall


# ── CLI ────────────────────────────────────────────────────────────────────

def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--universe", type=pathlib.Path, required=True)
    ap.add_argument("--hive", type=pathlib.Path, required=True)
    ap.add_argument("--db-prefix", required=True)
    ap.add_argument("--from", dest="from_date", required=True)
    ap.add_argument("--to", dest="to_date", required=True)
    ap.add_argument("--phase", choices=["pull", "build", "verify", "all"], default="all")
    ap.add_argument("--snap-et", default="15:55")
    ap.add_argument("--rates", type=pathlib.Path, default=pathlib.Path(DEFAULT_RATES_PATH))
    ap.add_argument("--build-exe", type=pathlib.Path, required=True)
    ap.add_argument("--admin-exe", type=pathlib.Path, required=True)
    ap.add_argument("--pull-tool", type=pathlib.Path, default=None,
                    help="override path to pull_opra_hive.py (default: this script's sibling)")
    ap.add_argument("--chunk-sessions", type=int, default=4)
    ap.add_argument("--fit-workers", type=int, default=0)
    ap.add_argument("--cap", type=float, default=90.0)
    ap.add_argument("--spend-abort", type=float, default=95.0)
    ap.add_argument("--max-failed-sessions", type=int, default=10)
    ap.add_argument("--log-dir", type=pathlib.Path, required=True)
    ap.add_argument("--env-file", default="C:/atx/.env")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--index", default="SPY")
    ap.add_argument("--max-symbols", type=int, default=None)
    return ap


def run(args) -> int:
    log_dir = pathlib.Path(args.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)

    rates = load_rates_csv(args.rates)
    universe_entries = truncate_universe(read_universe(args.universe), args.max_symbols)
    symbols = [s for s, _ in universe_entries]

    pull_universe_path = args.universe
    if args.max_symbols is not None:
        pull_universe_path = log_dir / "_pilot_universe.csv"
        write_symbols_file(universe_entries, pull_universe_path)
    args.pull_universe_path = pull_universe_path
    args.pull_tool_path = args.pull_tool or _PULL_TOOL_PATH

    ledger = SpendLedger(path=log_dir / "spend_ledger.csv", abort_threshold=args.spend_abort)

    phases = ["pull", "build", "verify"] if args.phase == "all" else [args.phase]
    for phase in phases:
        if phase == "pull":
            code = phase_pull(args, rates, ledger, log_dir)
        elif phase == "build":
            code = phase_build(args, symbols, rates, log_dir)
        else:
            code = phase_verify(args, universe_entries, log_dir)
        if code != 0:
            return code
    return 0


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return run(args)
    except ValueError as exc:
        # rate_for_date (and month_bounds) fail closed with a plain ValueError;
        # surface it as a clean one-line operator message instead of a raw
        # traceback, without changing the pure functions' tested contract.
        print(f"run_surface_db_backfill: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
