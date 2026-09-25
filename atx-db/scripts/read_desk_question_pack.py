"""Bounded read-only runner for the five desk SQL contracts; never certification.

Two access modes; every output keeps ``production_qualified=false``.

The governed warehouse is anchored explicitly (PRODUCTION_DB =
C:\\atx\\atx-db\\data\\warehouse.duckdb), never derived from this script's own
location, so a copy of the runner (RX3 pinned export, pool worktree) protects
the same file. Q3 needs schema >= 0327 (A5 identity label columns); on an older
schema it is reported unavailable.

L1 inspection (default): schema copies and small fixture databases only.
Refused before any DuckDB call: the governed warehouse (also through a hard
link), any file inside an ``atx-db/data`` directory of any checkout, export or
worktree (and inside the governed warehouse's directory), the checkout-root
warehouse_template.duckdb, and any database larger than 1 GiB (fixtures and
schema templates are far smaller; warehouses and backups are not). DuckDB
32-512MB, 1-2 threads, no spill, 1-60 s per query.
Bind against a copy of the current test-harness template, e.g. (PowerShell,
from atx-db):
  Copy-Item .pytest_cache/db_schema_templates/<fingerprint>/warehouse_template.duckdb "$env:TEMP/l1-schema.duckdb"
  .venv/Scripts/python.exe scripts/read_desk_question_pack.py `
    --db-path "$env:TEMP/l1-schema.duckdb" --mode explain --output-json "$env:TEMP/l1-bind.json"

Governed production read (--production): the production warehouse only, only
inside the controller memory guard. All of these are required, else the run is
refused before DuckDB is imported:
  * the explicit --production flag and --db-path naming the governed warehouse
    (PRODUCTION_DB, or --governed-warehouse PATH, which must then also appear in
    the guard receipt's command with the same path);
  * ATX_DESK_GUARD_RECEIPT naming the run_memory_guarded.py receipt of THIS
    launch: status "running", child_pid = this process (or its venv launcher),
    rewritten within the last 120 s, job_limit_gb <= 2, command naming this
    script with --production;
  * this process inside a Windows job whose memory limit is <= that cap;
  * >= 3 GiB free beside the output (spill cap 2GB + 1 GiB floor).
The runner may run from an RX3 pinned export (it then executes the export's SQL
files); the receipt records the runner root, its script sha256 and the export
commit when the root sits in exports/<sha>/atx-db.
Connection: read_only, memory_limit 1GB, 1 thread, spill <= 2GB in a private
temporary directory beside the output (removed afterwards), external access
disabled once the spill directory is set, UTC; the effective DuckDB settings are
re-read (access_mode read_only, the private temp_directory, limits as ceilings)
and then locked (lock_configuration=true), so no SQL can lift them. Per-query
deadline 1-1800 s (default 600), row cap 1-1000 plus a truncation sentinel,
transfer byte cap. The guard receipt, effective settings, schema version and
warehouse file metadata before and after are recorded in the output receipt. A
governed read is inspection of production data, not a qualification of it.
Operator sequence (PowerShell, from C:\\atx\\atx-db, no warehouse writer running;
observe headroom first per the controller rules; <runner> is scripts\\ of the
live tree or of the pinned export):
  $env:ATX_DESK_GUARD_RECEIPT = "<dir>\\desk-guard.json"
  .venv\\Scripts\\python.exe ..\\.superpowers\\sdd\\tier1-parity\\run_memory_guarded.py `
    --job-gb 2 --receipt "<dir>\\desk-guard.json" -- `
    .venv\\Scripts\\python.exe <runner>\\read_desk_question_pack.py --production `
    --db-path data\\warehouse.duckdb --mode run --output-json "<dir>\\desk-pack.json"
"""

from __future__ import annotations

import argparse
import contextlib
import datetime as dt
import hashlib
import itertools
import json
import math
import os
import re
import shutil
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parents[1]
SCRIPT_NAME = Path(__file__).name
SQL_FILES = {
    "q1": "desk-q1-cvx-eps.sql",
    "q2": "desk-q2-acceleration.sql",
    "q3": "desk-q3-valuation-profitability.sql",
    "q4": "desk-q4-accrual-leverage.sql",
    "q5": "desk-q5-monthly-factor-portfolios.sql",
}
MAX_RECEIPT_BYTES = 10_000_000
GIB = 1024 ** 3

#: The only database the governed production mode may read. Anchored to the
#: live checkout, never to this script's location (copies run from exports).
PRODUCTION_DB = Path(r"C:\atx\atx-db\data\warehouse.duckdb")
#: L1 never opens a database larger than this (schema templates are ~60 MB).
L1_MAX_DATABASE_BYTES = GIB
#: Environment variable naming the controller guard receipt of this launch.
GUARD_RECEIPT_ENV = "ATX_DESK_GUARD_RECEIPT"
GUARD_RECEIPT_MAX_BYTES = 1_000_000
#: The guard rewrites its receipt every 30 s while the child runs.
GUARD_RECEIPT_MAX_AGE_SECONDS = 120
#: The guard writes "running" + child_pid just after launching the child.
GUARD_RECEIPT_WAIT_SECONDS = 15.0
PRODUCTION_MAX_JOB_BYTES = 2 * GIB
PRODUCTION_MEMORY_LIMIT = "1GB"
PRODUCTION_MEMORY_BYTES = 10 ** 9
PRODUCTION_THREADS = 1
PRODUCTION_SPILL_LIMIT = "2GB"
PRODUCTION_SPILL_BYTES = 2 * 10 ** 9
PRODUCTION_DISK_FLOOR_BYTES = PRODUCTION_SPILL_BYTES + GIB
PRODUCTION_TIMEOUT_SECONDS = (1, 1800, 600)
L1_TIMEOUT_SECONDS = (1, 60, 30)
L1_MEMORY_LIMIT = "256MB"
L1_THREADS = 1
# DuckDB reports settings rounded to one decimal of a binary unit.
_SETTING_TOLERANCE = 1.001
_SIZE_UNITS = {"bytes": 1, "b": 1, "kib": 1024, "mib": 1024 ** 2, "gib": 1024 ** 3, "tib": 1024 ** 4,
               "kb": 10 ** 3, "mb": 10 ** 6, "gb": 10 ** 9, "tb": 10 ** 12}


class Refused(ValueError):
    """The requested access is not permitted; nothing was opened."""


class Limits(NamedTuple):
    access_mode: str
    memory_limit: str
    memory_bytes: int
    threads: int
    spill_limit: str
    spill_bytes: int
    timeout_seconds: int
    max_rows: int
    max_result_bytes: int


def utc_cutoff(text: str) -> dt.datetime:
    value = dt.datetime.fromisoformat(text.replace("Z", "+00:00"))
    if value.tzinfo is None or value.utcoffset() is None:
        raise argparse.ArgumentTypeError("cutoff requires an explicit UTC offset")
    return value.astimezone(dt.UTC).replace(tzinfo=None)


def memory_limit(text: str) -> str:
    match = re.fullmatch(r"([0-9]+)MB", text.upper())
    if match is None or not 32 <= int(match[1]) <= 512:
        raise argparse.ArgumentTypeError("memory-limit must be 32MB..512MB")
    return text.upper()


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--db-path", type=Path, required=True)
    p.add_argument("--output-json", type=Path, required=True)
    p.add_argument("--production", action="store_true",
                   help="governed read of the governed warehouse inside the controller guard")
    p.add_argument("--governed-warehouse", type=Path, default=None,
                   help="production only: override PRODUCTION_DB; must also appear in the guard receipt's command")
    p.add_argument("--query", choices=("all", *SQL_FILES), default="all")
    p.add_argument("--mode", choices=("explain", "run"), default="explain")
    p.add_argument("--memory-limit", type=memory_limit, default=None, help="L1 only (default 256MB)")
    p.add_argument("--threads", type=int, choices=(1, 2), default=None, help="L1 only (default 1)")
    p.add_argument("--max-rows", type=int, default=1000)
    p.add_argument("--max-result-bytes", type=int, default=1_000_000)
    p.add_argument("--timeout-seconds", type=int, default=None,
                   help="per-query deadline: L1 1..60 (default 30), production 1..1800 (default 600)")
    p.add_argument("--cutoff", type=utc_cutoff, default=utc_cutoff("2026-09-20T22:00:00Z"))
    p.add_argument("--cik", default="0000093410")
    p.add_argument("--market-cap-floor", type=float, default=1_000_000_000)
    p.add_argument("--build-run-id", default="fundamental_signals_build1")
    p.add_argument("--evaluation-run-id", default="fundamental_signals_evaluation1")
    p.add_argument("--signal-id", default="eps_growth_yoy")
    p.add_argument("--start-date", type=dt.date.fromisoformat, default=dt.date(2015, 1, 1))
    p.add_argument("--end-date", type=dt.date.fromisoformat, default=dt.date(2026, 12, 31))
    return p


def _same_file(left: Path, right: Path) -> bool:
    return left == right or (left.exists() and right.exists() and left.samefile(right))


def _in_data_directory(path: Path) -> bool:
    """Inside any ``atx-db/data`` directory (any checkout, export or worktree) or this runner's own."""
    parts = [part.lower() for part in path.parts]
    return (any(pair == ("atx-db", "data") for pair in itertools.pairwise(parts))
            or path.is_relative_to((ROOT / "data").resolve()))


def validate_paths(db_path: Path, output: Path, *, production: bool,
                   governed: Path | None = None) -> tuple[Path, Path]:
    """Resolve links and classify the database before any DuckDB call."""
    db_path, output = db_path.resolve(), output.resolve()
    governed = (governed or PRODUCTION_DB).resolve()
    for template in (ROOT / "warehouse_template.duckdb", governed.parent.parent / "warehouse_template.duckdb"):
        if _same_file(db_path, template.resolve()):
            raise Refused("the checkout-root template is forbidden; use a copy")
    is_production = _same_file(db_path, governed)
    if production and not is_production:
        raise Refused(f"--production reads only the governed warehouse {governed}")
    if not production:
        if is_production or _in_data_directory(db_path):
            raise Refused("the governed warehouse and every database in an atx-db/data directory require "
                          f"--production and the {GUARD_RECEIPT_ENV} guard receipt; L1 reads a copy or tiny fixture")
        if db_path.is_file() and db_path.stat().st_size > L1_MAX_DATABASE_BYTES:
            raise Refused(f"L1 opens only fixtures and schema copies up to {L1_MAX_DATABASE_BYTES} bytes")
    if not db_path.is_file():
        raise ValueError("db-path must be an existing database file")
    if output == db_path or output.exists() or output.suffix.lower() != ".json" or not output.parent.is_dir():
        raise ValueError("output-json must be a fresh .json path in an existing directory")
    if _in_data_directory(output):
        raise ValueError("output-json must not be written in an atx-db/data directory")
    return db_path, output


def resolve_limits(args) -> Limits:
    if not 1 <= args.max_rows <= 1000 or not 1024 <= args.max_result_bytes <= 1_000_000:
        raise ValueError("max-rows must be 1..1000 and max-result-bytes 1024..1000000")
    if args.production:
        if args.memory_limit is not None or args.threads is not None:
            raise Refused("production memory and threads are fixed (1GB, 1 thread); do not pass overrides")
        low, high, default = PRODUCTION_TIMEOUT_SECONDS
        memory, memory_bytes, threads = PRODUCTION_MEMORY_LIMIT, PRODUCTION_MEMORY_BYTES, PRODUCTION_THREADS
        spill, spill_bytes, mode = PRODUCTION_SPILL_LIMIT, PRODUCTION_SPILL_BYTES, "governed_production"
    else:
        low, high, default = L1_TIMEOUT_SECONDS
        memory = args.memory_limit or L1_MEMORY_LIMIT
        memory_bytes = int(memory.removesuffix("MB")) * 10 ** 6
        threads, spill, spill_bytes, mode = args.threads or L1_THREADS, "0B", 0, "l1_inspection"
    timeout = default if args.timeout_seconds is None else args.timeout_seconds
    if not low <= timeout <= high:
        raise ValueError(f"timeout-seconds must be {low}..{high} per query in {mode}")
    return Limits(mode, memory, memory_bytes, threads, spill, spill_bytes, timeout,
                  args.max_rows, args.max_result_bytes)


def job_memory_limit_bytes() -> int | None:
    """Memory limit of the Windows job that directly contains this process.

    None off Windows, outside a job, or for a job without a memory limit. The
    controller guard assigns itself to a job with a job-memory limit before
    launching, so its child inherits the cap from creation.
    """
    if os.name != "nt":
        return None
    import ctypes
    from ctypes import wintypes

    class Basic(ctypes.Structure):
        _fields_ = [("per_process_user_time", ctypes.c_longlong), ("per_job_user_time", ctypes.c_longlong),
                    ("flags", wintypes.DWORD), ("minimum_working_set", ctypes.c_size_t),
                    ("maximum_working_set", ctypes.c_size_t), ("active_process_limit", wintypes.DWORD),
                    ("affinity", ctypes.c_size_t), ("priority_class", wintypes.DWORD),
                    ("scheduling_class", wintypes.DWORD)]

    class Io(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in
                    ("read_ops", "write_ops", "other_ops", "read_bytes", "write_bytes", "other_bytes")]

    class Extended(ctypes.Structure):
        _fields_ = [("basic", Basic), ("io", Io), ("process_memory", ctypes.c_size_t),
                    ("job_memory", ctypes.c_size_t), ("peak_process_memory", ctypes.c_size_t),
                    ("peak_job_memory", ctypes.c_size_t)]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.argtypes, kernel.GetCurrentProcess.restype = [], wintypes.HANDLE
    kernel.IsProcessInJob.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)]
    kernel.IsProcessInJob.restype = wintypes.BOOL
    kernel.QueryInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                                 wintypes.DWORD, ctypes.c_void_p]
    kernel.QueryInformationJobObject.restype = wintypes.BOOL
    in_job = wintypes.BOOL(False)
    if not kernel.IsProcessInJob(kernel.GetCurrentProcess(), None, ctypes.byref(in_job)) or not in_job.value:
        return None
    limits = Extended()
    # A NULL job handle queries the job that directly contains the caller (9 =
    # JobObjectExtendedLimitInformation).
    if not kernel.QueryInformationJobObject(None, 9, ctypes.byref(limits), ctypes.sizeof(limits), None):
        return None
    caps = [limit for flag, limit in ((0x200, limits.job_memory), (0x100, limits.process_memory))
            if limits.basic.flags & flag and limit > 0]
    return min(caps) if caps else None


def _read_guard_receipt(path: Path) -> dict | None:
    """The parsed receipt, or None while it is absent or being rewritten."""
    try:
        if path.stat().st_size > GUARD_RECEIPT_MAX_BYTES:
            return None
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return value if isinstance(value, dict) else None


def verify_guard_receipt(governed_override: Path | None = None) -> dict:
    """Prove this process runs inside the controller guard's memory-capped job.

    ``governed_override`` (--governed-warehouse) must be named in the receipt's
    command with the same resolved path, so the override is part of the guard record.
    """
    named = os.environ.get(GUARD_RECEIPT_ENV)
    if not named:
        raise Refused(f"{GUARD_RECEIPT_ENV} is not set: production reads run only under run_memory_guarded.py")
    path = Path(named).resolve()
    if path.suffix.lower() != ".json":
        raise Refused("guard receipt must be a .json file")
    own = {os.getpid(), os.getppid()}
    deadline = time.monotonic() + GUARD_RECEIPT_WAIT_SECONDS
    while True:
        receipt = _read_guard_receipt(path)
        if receipt is not None and receipt.get("child_pid") in own:
            break
        if time.monotonic() >= deadline:
            raise Refused("guard receipt is absent or does not name this process as the guarded child")
        time.sleep(0.25)
    if receipt.get("status") != "running":
        raise Refused(f"guard receipt status is {receipt.get('status')!r}, not 'running'")
    age = time.time() - path.stat().st_mtime
    if not 0 <= age <= GUARD_RECEIPT_MAX_AGE_SECONDS:
        raise Refused(f"guard receipt is stale ({age:.0f} s since its last write)")
    job_gb = receipt.get("job_limit_gb")
    if isinstance(job_gb, bool) or not isinstance(job_gb, (int, float)) or not math.isfinite(job_gb) \
            or not 0 < job_gb * GIB <= PRODUCTION_MAX_JOB_BYTES:
        raise Refused(f"guard job cap must be positive and at most {PRODUCTION_MAX_JOB_BYTES // GIB} GiB")
    command = receipt.get("command")
    if not isinstance(command, list) or not all(isinstance(part, str) for part in command) \
            or not any(Path(part).name == SCRIPT_NAME for part in command) or "--production" not in command:
        raise Refused(f"guard receipt command must launch {SCRIPT_NAME} with --production")
    if governed_override is not None:
        flag = "--governed-warehouse"
        named = [Path(command[i + 1]).resolve() for i, part in enumerate(command[:-1]) if part == flag]
        if named != [governed_override.resolve()]:
            raise Refused(f"{flag} must appear once in the guard receipt's command with the same path")
    job_bytes = job_memory_limit_bytes()
    if job_bytes is None:
        raise Refused("this process is not inside a memory-capped job object")
    if job_bytes > job_gb * GIB + 2 ** 20:
        raise Refused(f"job memory cap {job_bytes} exceeds the guard receipt's {job_gb} GiB")
    return {"receipt_path": str(path), "receipt_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "child_pid": receipt["child_pid"], "job_limit_gb": job_gb, "job_memory_limit_bytes": job_bytes,
            "guard_headroom": receipt.get("headroom"), "verified_at_utc": dt.datetime.now(dt.UTC).isoformat()}


def open_read_only(db_path: Path, memory: str, threads: int, spill_directory: str | None = None,
                   spill_limit: str = "0B"):
    import duckdb

    config = {"memory_limit": memory, "threads": str(threads), "preserve_insertion_order": "false",
              "max_temp_directory_size": spill_limit}
    if spill_directory is None:
        config["enable_external_access"] = "false"
        return duckdb.connect(str(db_path), read_only=True, config=config)
    # DuckDB rejects temp_directory once external access is disabled: set the
    # private spill directory first, then lock external access.
    con = duckdb.connect(str(db_path), read_only=True, config={**config, "temp_directory": spill_directory})
    try:
        con.execute("SET enable_external_access = false")
    except BaseException:
        con.close()
        raise
    return con


def _setting_bytes(text: str) -> float | None:
    match = re.fullmatch(r"\s*([0-9]+(?:\.[0-9]+)?)\s*([A-Za-z]+)\s*", text or "")
    if match is None or match[2].lower() not in _SIZE_UNITS:
        return None
    return float(match[1]) * _SIZE_UNITS[match[2].lower()]


_SETTINGS_SQL = """
    SELECT name, value FROM duckdb_settings()
    WHERE name IN ('memory_limit','threads','max_temp_directory_size','enable_external_access',
                   'preserve_insertion_order','access_mode','TimeZone','temp_directory','lock_configuration')
"""


def verify_settings(con, limits: Limits, spill_directory: str | None = None) -> dict:
    """Re-read DuckDB's effective settings, enforce them, then lock the configuration.

    Limits are ceilings; access must be read_only; a governed spill must use the
    private directory. After ``lock_configuration`` no SET/RESET/PRAGMA can lift them.
    """
    settings = dict(con.execute(_SETTINGS_SQL).fetchall())
    memory = _setting_bytes(settings.get("memory_limit"))
    spill = _setting_bytes(settings.get("max_temp_directory_size"))
    problems = []
    if settings.get("access_mode") != "read_only":
        problems.append(f"access_mode {settings.get('access_mode')}")
    if spill_directory is not None and not _same_file(Path(str(settings.get("temp_directory"))).resolve(),
                                                      Path(spill_directory).resolve()):
        problems.append(f"temp_directory {settings.get('temp_directory')}")
    if memory is None or memory > limits.memory_bytes * _SETTING_TOLERANCE:
        problems.append(f"memory_limit {settings.get('memory_limit')}")
    if spill is None or spill > limits.spill_bytes * _SETTING_TOLERANCE:
        problems.append(f"max_temp_directory_size {settings.get('max_temp_directory_size')}")
    if not str(settings.get("threads", "")).isdigit() or not 1 <= int(settings["threads"]) <= limits.threads:
        problems.append(f"threads {settings.get('threads')}")
    if str(settings.get("enable_external_access")).lower() != "false":
        problems.append("external access enabled")
    if problems:
        raise RuntimeError("effective DuckDB settings exceed the declared limits: " + "; ".join(problems))
    con.execute("SET lock_configuration = true")
    locked = dict(con.execute(_SETTINGS_SQL).fetchall())
    if str(locked.get("lock_configuration")).lower() != "true" or locked != {**settings, "lock_configuration":
                                                                              locked.get("lock_configuration")}:
        raise RuntimeError("DuckDB configuration did not lock with the verified settings")
    return locked


def schema_version(con) -> str | None:
    import duckdb

    try:
        row = con.execute("SELECT max(version) FROM schema_migrations WHERE version ~ '^[0-9]+$'").fetchone()
    except duckdb.Error:
        return None
    return None if row is None else row[0]


def inspect_query(con, name: str, parameters: dict, limits: Limits, mode: str) -> dict:
    import duckdb

    sql_path = ROOT / "sql/research" / SQL_FILES[name]
    sql = sql_path.read_text(encoding="utf-8").strip().removesuffix(";")
    bound = {key: parameters[key] for key in set(re.findall(r"\$([a-z_]+)", sql))}
    result = {"query": name, "sql_path": str(sql_path),
              "sql_sha256": hashlib.sha256(sql.encode()).hexdigest(), "parameters": bound,
              "production_qualified": False}
    deadline_hit = threading.Event()

    def interrupt() -> None:
        deadline_hit.set()
        con.interrupt()

    timer = threading.Timer(limits.timeout_seconds, interrupt)
    timer.daemon = True
    started = time.monotonic()
    con.execute("BEGIN TRANSACTION")
    timer.start()
    try:
        plan = con.execute("EXPLAIN " + sql, bound).fetchone()
        result["plan_sha256"] = hashlib.sha256(str(plan).encode()).hexdigest()
        result["bind_status"] = "bound"
        if mode == "run":
            # One execution: DuckDB materializes at most max_rows + 1 rows (the
            # sentinel) and rows cross into Python one at a time under the cap.
            cursor = con.execute(
                "SELECT payload, octet_length(encode(payload)) FROM ("
                f"SELECT CAST(to_json(t) AS VARCHAR) AS payload FROM ({sql}) t LIMIT {limits.max_rows + 1})",
                bound,
            )
            payloads, size = [], 0
            while (row := cursor.fetchone()) is not None:
                size += row[1]
                if size > limits.max_result_bytes:
                    raise ValueError(f"bounded payload exceeds byte cap: more than {limits.max_result_bytes}")
                payloads.append(row[0])
            rows = len(payloads)
            result.update(rows=[json.loads(payload) for payload in payloads[:limits.max_rows]],
                          returned_rows=min(rows, limits.max_rows), truncated=rows > limits.max_rows,
                          transferred_bytes_including_sentinel=size)
            result["status"] = "empty" if not rows else "inspection_only"
        else:
            result["status"] = "bound_only"
        con.execute("COMMIT")
    except (duckdb.Error, ValueError) as exc:
        con.execute("ROLLBACK")
        result.update(status="unavailable", error_type=type(exc).__name__, error=str(exc)[:6000],
                      deadline_exceeded=deadline_hit.is_set())
    finally:
        timer.cancel()
        timer.join()
        result["elapsed_seconds"] = round(time.monotonic() - started, 3)
    return result


def runner_provenance() -> dict:
    """Which runner and SQL tree ran: the live checkout or an RX3 pinned export (exports/<sha>/atx-db)."""
    commit = ROOT.parent.name if re.fullmatch(r"[0-9a-f]{40}", ROOT.parent.name) else None
    return {"root": str(ROOT), "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "export_commit": commit}


def _file_state(path: Path) -> dict:
    stat = path.stat()
    return {"bytes": stat.st_size, "mtime_ns": stat.st_mtime_ns}


def write_receipt(output: Path, receipt: dict) -> None:
    encoded = json.dumps(receipt, default=str, sort_keys=True, indent=2, allow_nan=False) + "\n"
    if len(encoded.encode("utf-8")) > MAX_RECEIPT_BYTES:
        raise ValueError("receipt exceeds 10MB cap")
    fd, temporary = tempfile.mkstemp(prefix=".desk-pack-", suffix=".json", dir=output.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as sink:
            sink.write(encoded)
            sink.flush()
            os.fsync(sink.fileno())
        os.link(temporary, output)
    finally:
        os.unlink(temporary)


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.governed_warehouse is not None and not args.production:
            raise Refused("--governed-warehouse is a production-only override")
        governed = args.governed_warehouse or PRODUCTION_DB
        db_path, output = validate_paths(args.db_path, args.output_json, production=args.production,
                                         governed=governed)
        limits = resolve_limits(args)
        if not args.cik.isascii() or not args.cik.isdecimal() or not 1 <= len(args.cik) <= 10 or int(args.cik) == 0:
            raise ValueError("cik requires 1..10 digits, nonzero")
        if not math.isfinite(args.market_cap_floor) or args.market_cap_floor <= 0:
            raise ValueError("market-cap-floor must be finite and positive")
        if not dt.date(2015, 1, 1) <= args.start_date <= args.end_date <= dt.date(2026, 12, 31):
            raise ValueError("date range must lie within 2015..2026")
        guard = None
        if args.production:
            guard = verify_guard_receipt(args.governed_warehouse)
            free = shutil.disk_usage(output.parent).free
            if free < PRODUCTION_DISK_FLOOR_BYTES:
                raise Refused(f"{free} bytes free beside the output; the spill cap needs "
                              f"{PRODUCTION_DISK_FLOOR_BYTES}")
        parameters = {key: getattr(args, key) for key in (
            "cutoff", "market_cap_floor", "build_run_id", "evaluation_run_id", "signal_id", "start_date", "end_date",
        )}
        parameters["cik"] = args.cik.zfill(10)
        receipt = {"contract": "desk-question-pack-governed-v1" if args.production else "desk-question-pack-l1-v1",
                   "access_mode": limits.access_mode, "database": str(db_path),
                   "read_only": True, "mode": args.mode, "production_qualified": False,
                   "limits": limits._asdict(), "guard": guard, "database_file_before": _file_state(db_path),
                   "governed_warehouse": str(governed), "governed_warehouse_override": args.governed_warehouse
                   is not None, "runner": runner_provenance(), "queries": []}
        try:
            spill_scope = (tempfile.TemporaryDirectory(prefix=".desk-pack-spill-", dir=output.parent,
                                                       ignore_cleanup_errors=True)
                           if args.production else contextlib.nullcontext(None))
            with spill_scope as spill, open_read_only(db_path, limits.memory_limit, limits.threads, spill,
                                                      limits.spill_limit) as con:
                receipt["spill_directory"] = spill
                con.execute("SET TimeZone='UTC'")
                receipt["effective_settings"] = verify_settings(con, limits, spill)
                receipt["schema_version"] = schema_version(con)
                for name in SQL_FILES if args.query == "all" else (args.query,):
                    receipt["queries"].append(inspect_query(con, name, parameters, limits, args.mode))
            receipt["status"] = "unavailable" if any(
                r["status"] == "unavailable" for r in receipt["queries"]
            ) else "inspection_complete"
        except Exception as exc:
            receipt.update(status="unavailable", error_type=type(exc).__name__, error=str(exc)[:6000])
        if receipt.get("spill_directory"):
            # Cleanup errors are ignored (a lingering DuckDB handle must not fail a
            # completed read); a leftover directory is recorded instead.
            receipt["spill_directory_removed"] = not Path(receipt["spill_directory"]).exists()
        receipt["database_file_after"] = _file_state(db_path)
        receipt["database_file_changed"] = receipt["database_file_after"] != receipt["database_file_before"]
        write_receipt(output, receipt)
    except Exception as exc:
        # Refusals and argument failures occur before any database is opened.
        label = "refused" if isinstance(exc, Refused) else "unavailable"
        print(f"desk pack {label}: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2
    print(str(output))
    return 2 if receipt["status"] == "unavailable" else 0


if __name__ == "__main__":
    raise SystemExit(main())
