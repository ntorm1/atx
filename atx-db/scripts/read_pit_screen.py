"""Bounded read-only point-in-time universe screen; inspection, never certification.

"Give me field X for every member of universe U as of time T": one row per
(tier-1 universe member active at --as-of, --field) from
atx_db.asof.cross_section_asof, with value, value_status, lineage status, clocks,
staleness and the identity / universe / availability bases. Every output keeps
production_qualified=false.

Access follows the desk-question-pack read-only guard contract (C1; implemented
locally here because that helper lives inside its own script):

L1 inspection (default): schema copies and small fixture databases only. The
production warehouse, every database under atx-db/data (also through a hard
link) and the checkout-root warehouse_template.duckdb are refused before any
DuckDB call. DuckDB 32-512MB, 1-2 threads, no spill, screen deadline 1-300 s.

Governed production read (--production): the production warehouse only, only
inside the controller memory guard. All of these are required, else the run is
refused before DuckDB is imported:
  * the explicit --production flag and --db-path naming data/warehouse.duckdb;
  * ATX_DESK_GUARD_RECEIPT naming the run_memory_guarded.py receipt of THIS
    launch: status "running", child_pid = this process (or its venv launcher),
    rewritten within the last 120 s, job_limit_gb <= 2, command naming this
    script with --production;
  * this process inside a Windows job whose memory limit is <= that cap;
  * >= 3 GiB free beside the output (spill cap 2GB + 1 GiB floor).
Connection: a private scratch DuckDB (removed afterwards) in a temporary
directory beside the output, with the warehouse (and an optional research store
for feature: fields) attached READ_ONLY -- verified -- memory_limit 1GB, 1
thread, spill <= 2GB, external access disabled once attached, UTC; effective
settings are re-read and must not exceed the limits. Screen deadline 1-1800 s
(default 900); receipt rows 1-1000 plus a truncation flag and a byte cap; the
full screen optionally to a fresh --output-csv. The guard receipt, settings,
schema version and warehouse file metadata before and after are recorded.

Fields: <metric_code> | derived:<code> | market:<code> | item:<code>:<basis> |
feature:<feature_id>[:<variant>] (feature fields need --research-db-path and
--feature-version). Example (PowerShell, from atx-db, on a fixture copy):
  .venv\\Scripts\\python.exe scripts\\read_pit_screen.py --db-path "$env:TEMP\\fixture.duckdb" `
    --as-of 2024-01-31T22:00:00Z --field roa_q --field market_cap --field item:revenue:quarterly `
    --output-json "$env:TEMP\\screen.json" --output-csv "$env:TEMP\\screen.csv"
"""

from __future__ import annotations

import argparse
import contextlib
import datetime as dt
import hashlib
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
from typing import Any, NamedTuple

ROOT = Path(__file__).resolve().parents[1]
SCRIPT_NAME = Path(__file__).name
MAX_RECEIPT_BYTES = 10_000_000
GIB = 1024 ** 3

#: The only database the governed production mode may read.
PRODUCTION_DB = ROOT / "data" / "warehouse.duckdb"
#: The default research store (RX6) a governed read may attach for feature fields.
PRODUCTION_RESEARCH_DB = ROOT / "data" / "research" / "research.duckdb"
#: Environment variable naming the controller guard receipt of this launch (C1).
GUARD_RECEIPT_ENV = "ATX_DESK_GUARD_RECEIPT"
GUARD_RECEIPT_MAX_BYTES = 1_000_000
GUARD_RECEIPT_MAX_AGE_SECONDS = 120
GUARD_RECEIPT_WAIT_SECONDS = 15.0
PRODUCTION_MAX_JOB_BYTES = 2 * GIB
PRODUCTION_MEMORY_LIMIT = "1GB"
PRODUCTION_MEMORY_BYTES = 10 ** 9
PRODUCTION_THREADS = 1
PRODUCTION_SPILL_LIMIT = "2GB"
PRODUCTION_SPILL_BYTES = 2 * 10 ** 9
PRODUCTION_DISK_FLOOR_BYTES = PRODUCTION_SPILL_BYTES + GIB
PRODUCTION_TIMEOUT_SECONDS = (1, 1800, 900)
L1_TIMEOUT_SECONDS = (1, 300, 120)
L1_MEMORY_LIMIT = "256MB"
L1_THREADS = 1
MAX_SCREEN_ROWS = 1_000_000
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
    max_screen_rows: int


def utc_timestamp(text: str) -> dt.datetime:
    value = dt.datetime.fromisoformat(text.replace("Z", "+00:00"))
    if value.tzinfo is None or value.utcoffset() is None:
        raise argparse.ArgumentTypeError("--as-of requires an explicit UTC offset")
    return value.astimezone(dt.UTC)


def memory_limit(text: str) -> str:
    match = re.fullmatch(r"([0-9]+)MB", text.upper())
    if match is None or not 32 <= int(match[1]) <= 512:
        raise argparse.ArgumentTypeError("memory-limit must be 32MB..512MB")
    return text.upper()


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--db-path", type=Path, required=True)
    p.add_argument("--output-json", type=Path, required=True)
    p.add_argument("--output-csv", type=Path, default=None, help="fresh .csv for the full screen")
    p.add_argument("--production", action="store_true",
                   help="governed read of data/warehouse.duckdb inside the controller guard")
    p.add_argument("--as-of", type=utc_timestamp, required=True, help="knowledge cutoff, ISO with UTC offset")
    p.add_argument("--field", action="append", required=True, help="repeatable; see the module docstring")
    p.add_argument("--basis", choices=("strict", "reconstructed"), default=None)
    p.add_argument("--universe-id", default=None)
    p.add_argument("--research-db-path", type=Path, default=None)
    p.add_argument("--feature-version", default=None)
    p.add_argument("--unverified-vendor-shares", action="store_true",
                   help="allow line_market_cap (vendor share counts, labeled unverified)")
    p.add_argument("--session-lookback-days", type=int, default=10)
    p.add_argument("--memory-limit", type=memory_limit, default=None, help="L1 only (default 256MB)")
    p.add_argument("--threads", type=int, choices=(1, 2), default=None, help="L1 only (default 1)")
    p.add_argument("--max-rows", type=int, default=1000, help="rows copied into the JSON receipt (1..1000)")
    p.add_argument("--max-result-bytes", type=int, default=1_000_000)
    p.add_argument("--max-screen-rows", type=int, default=200_000,
                   help=f"members x fields bound of the screen itself (1..{MAX_SCREEN_ROWS})")
    p.add_argument("--timeout-seconds", type=int, default=None,
                   help="screen deadline: L1 1..300 (default 120), production 1..1800 (default 900)")
    return p


def _same_file(left: Path, right: Path) -> bool:
    return left == right or (left.exists() and right.exists() and left.samefile(right))


def _fresh(path: Path, suffix: str, data_root: Path, *others: Path) -> Path:
    path = path.resolve()
    if path.exists() or path.suffix.lower() != suffix or not path.parent.is_dir():
        raise ValueError(f"{path.name}: outputs must be fresh {suffix} paths in an existing directory")
    if path.is_relative_to(data_root) or any(path == other for other in others):
        raise ValueError("outputs must not be written under atx-db/data or over an input")
    return path


def validate_paths(db_path: Path, output: Path, csv: Path | None, research: Path | None, *,
                   production: bool) -> tuple[Path, Path, Path | None, Path | None]:
    """Resolve links and classify every input before any DuckDB call."""
    db_path = db_path.resolve()
    data_root = (ROOT / "data").resolve()
    if _same_file(db_path, (ROOT / "warehouse_template.duckdb").resolve()):
        raise Refused("the checkout-root template is forbidden; use a copy")
    is_production = _same_file(db_path, PRODUCTION_DB.resolve())
    if production and not is_production:
        raise Refused(f"--production reads only the governed warehouse {PRODUCTION_DB}")
    if not production and (is_production or db_path.is_relative_to(data_root)):
        raise Refused("the production warehouse and every database under atx-db/data require --production "
                      f"and the {GUARD_RECEIPT_ENV} guard receipt; L1 inspection reads a copy or tiny fixture")
    if not db_path.is_file():
        raise ValueError("db-path must be an existing database file")
    if research is not None:
        research = research.resolve()
        if _same_file(research, db_path) or _same_file(research, (ROOT / "warehouse_template.duckdb").resolve()):
            raise Refused("the research store must be a separate research database")
        under_data = research.is_relative_to(data_root)
        if under_data and not (production and _same_file(research, PRODUCTION_RESEARCH_DB.resolve())):
            raise Refused("under atx-db/data only the default research store, and only with --production")
        if not research.is_file():
            raise ValueError("research-db-path must be an existing database file")
    output = _fresh(output, ".json", data_root, db_path)
    csv = None if csv is None else _fresh(csv, ".csv", data_root, db_path, output)
    return db_path, output, csv, research


def resolve_limits(args: argparse.Namespace) -> Limits:
    if not 1 <= args.max_rows <= 1000 or not 1024 <= args.max_result_bytes <= 1_000_000:
        raise ValueError("max-rows must be 1..1000 and max-result-bytes 1024..1000000")
    if not 1 <= args.max_screen_rows <= MAX_SCREEN_ROWS:
        raise ValueError(f"max-screen-rows must be 1..{MAX_SCREEN_ROWS}")
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
        raise ValueError(f"timeout-seconds must be {low}..{high} in {mode}")
    return Limits(mode, memory, memory_bytes, threads, spill, spill_bytes, timeout, args.max_rows,
                  args.max_result_bytes, args.max_screen_rows)


def job_memory_limit_bytes() -> int | None:
    """Memory limit of the Windows job that directly contains this process (None: no capped job)."""
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
    # A NULL job handle queries the job that directly contains the caller
    # (9 = JobObjectExtendedLimitInformation).
    if not kernel.QueryInformationJobObject(None, 9, ctypes.byref(limits), ctypes.sizeof(limits), None):
        return None
    caps = [limit for flag, limit in ((0x200, limits.job_memory), (0x100, limits.process_memory))
            if limits.basic.flags & flag and limit > 0]
    return min(caps) if caps else None


def _read_guard_receipt(path: Path) -> dict[str, Any] | None:
    try:
        if path.stat().st_size > GUARD_RECEIPT_MAX_BYTES:
            return None
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return value if isinstance(value, dict) else None


def verify_guard_receipt() -> dict[str, Any]:
    """Prove this process runs inside the controller guard's memory-capped job."""
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
    job_bytes = job_memory_limit_bytes()
    if job_bytes is None:
        raise Refused("this process is not inside a memory-capped job object")
    if job_bytes > job_gb * GIB + 2 ** 20:
        raise Refused(f"job memory cap {job_bytes} exceeds the guard receipt's {job_gb} GiB")
    return {"receipt_path": str(path), "receipt_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "child_pid": receipt["child_pid"], "job_limit_gb": job_gb, "job_memory_limit_bytes": job_bytes,
            "guard_headroom": receipt.get("headroom"), "verified_at_utc": dt.datetime.now(dt.UTC).isoformat()}


def _setting_bytes(text: str | None) -> float | None:
    match = re.fullmatch(r"\s*([0-9]+(?:\.[0-9]+)?)\s*([A-Za-z]+)\s*", text or "")
    if match is None or match[2].lower() not in _SIZE_UNITS:
        return None
    return float(match[1]) * _SIZE_UNITS[match[2].lower()]


def verify_settings(settings: dict[str, str], limits: Limits) -> dict[str, str]:
    """A DuckDB setting above the declared limit aborts the screen."""
    memory = _setting_bytes(settings.get("memory_limit"))
    spill = _setting_bytes(settings.get("max_temp_directory_size"))
    problems = []
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
    return settings


def schema_version(con: Any) -> str | None:
    try:
        row = con.execute("SELECT max(version) FROM wh.main.schema_migrations WHERE version ~ '^[0-9]+$'").fetchone()
    except Exception:  # a fixture without the migration ledger has no version
        return None
    return None if row is None else row[0]


def _file_state(path: Path) -> dict[str, int]:
    stat = path.stat()
    return {"bytes": stat.st_size, "mtime_ns": stat.st_mtime_ns}


def _publish(output: Path, write: Any, prefix: str) -> str:
    """Write through a private temporary file and link it into place (never overwrite)."""
    fd, temporary = tempfile.mkstemp(prefix=prefix, suffix=output.suffix, dir=output.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as sink:
            write(sink)
            sink.flush()
            os.fsync(sink.fileno())
        digest = hashlib.sha256(Path(temporary).read_bytes()).hexdigest()
        os.link(temporary, output)
    finally:
        os.unlink(temporary)
    return digest


def write_receipt(output: Path, receipt: dict[str, Any]) -> None:
    encoded = json.dumps(receipt, default=str, sort_keys=True, indent=2, allow_nan=False) + "\n"
    if len(encoded.encode("utf-8")) > MAX_RECEIPT_BYTES:
        raise ValueError("receipt exceeds 10MB cap")
    _publish(output, lambda sink: sink.write(encoded), ".pit-screen-")


def _bounded_rows(frame: Any, limits: Limits) -> tuple[list[dict[str, Any]], int, bool]:
    """At most max_rows rows and max_result_bytes of JSON; a sentinel flags truncation."""
    head = frame.head(limits.max_rows + 1)
    records = json.loads(head.to_json(orient="records", date_format="iso", double_precision=15))
    rows, size = [], 0
    for record in records[:limits.max_rows]:
        encoded = len(json.dumps(record, sort_keys=True, allow_nan=False).encode("utf-8"))
        if size + encoded > limits.max_result_bytes:
            break
        rows.append(record)
        size += encoded
    return rows, size, len(frame) > len(rows)


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        db_path, output, csv_path, research = validate_paths(
            args.db_path, args.output_json, args.output_csv, args.research_db_path, production=args.production)
        limits = resolve_limits(args)
        if not 1 <= args.session_lookback_days <= 31:
            raise ValueError("session-lookback-days must be 1..31")
        guard = None
        if args.production:
            guard = verify_guard_receipt()
            free = shutil.disk_usage(output.parent).free
            if free < PRODUCTION_DISK_FLOOR_BYTES:
                raise Refused(f"{free} bytes free beside the output; the spill cap needs "
                              f"{PRODUCTION_DISK_FLOOR_BYTES}")
        receipt: dict[str, Any] = {
            "contract": "pit-screen-governed-v1" if args.production else "pit-screen-l1-v1",
            "access_mode": limits.access_mode, "database": str(db_path), "research_database":
                None if research is None else str(research),
            "read_only": True, "production_qualified": False, "limits": limits._asdict(), "guard": guard,
            "request": {"as_of": args.as_of.isoformat(), "fields": list(args.field), "basis": args.basis,
                        "universe_id": args.universe_id, "feature_version": args.feature_version,
                        "unverified_vendor_shares": args.unverified_vendor_shares,
                        "session_lookback_days": args.session_lookback_days},
            "database_file_before": _file_state(db_path),
        }
        # One deadline for the whole screen (import, open, owner bridge, values):
        # the screen is many statements and an interrupt landing between two of
        # them is a no-op, so the watchdog keeps interrupting until it stops.
        deadline_hit, stop, active = threading.Event(), threading.Event(), {}

        def watchdog() -> None:
            if stop.wait(limits.timeout_seconds):
                return
            deadline_hit.set()
            while not stop.is_set():
                con = active.get("con")
                if con is not None:
                    # The connection may close between the read and the call.
                    with contextlib.suppress(Exception):
                        con.interrupt()
                stop.wait(0.25)

        guard_thread = threading.Thread(target=watchdog, daemon=True)
        started = time.monotonic()
        guard_thread.start()
        try:
            # Imported only after every refusal: nothing above touches DuckDB.
            from atx_db.asof.cross_section import ScreenStore, run_cross_section

            with ScreenStore(db_path, scratch_dir=output.parent, memory_limit=limits.memory_limit,
                             threads=limits.threads, spill_limit=limits.spill_limit,
                             research_db_path=research) as screen:
                active["con"] = screen.con
                try:
                    receipt["effective_settings"] = verify_settings(screen.settings(), limits)
                    receipt["attachments_read_only"] = dict(screen.con.execute(
                        "SELECT database_name, readonly FROM duckdb_databases() WHERE database_name IN ('wh','rs')"
                    ).fetchall())
                    receipt["schema_version"] = schema_version(screen.con)
                    result = run_cross_section(
                        screen, args.as_of, args.field, basis=args.basis, universe_id=args.universe_id,
                        feature_version=args.feature_version, max_rows=limits.max_screen_rows,
                        session_lookback_days=args.session_lookback_days,
                        unverified_vendor_shares=args.unverified_vendor_shares)
                finally:
                    active.pop("con", None)
            if deadline_hit.is_set():
                raise TimeoutError(f"screen deadline of {limits.timeout_seconds} s exceeded")
            rows, size, truncated = _bounded_rows(result.rows, limits)
            receipt["screen"] = {
                "query_version": result.query_version, "status": result.status,
                "as_of_ts_utc": result.as_of_ts.isoformat(),
                "screen_date": None if result.screen_date is None else result.screen_date.isoformat(),
                "basis": result.basis, "universe_id": result.universe_id, "identity_basis": result.identity_basis,
                "fields": [item.field_id for item in result.fields], "members": result.members,
                "eligible_members": result.eligible_members, "valid_members": result.valid_members,
                "screen_rows": len(result.rows), "field_digests": result.field_digests,
                "r2a_field_digests": result.r2a_field_digests, "screen_sha256": result.screen_sha256,
                "blockers": list(result.blockers), "diagnostics": result.diagnostics,
            }
            receipt.update(rows=rows, returned_rows=len(rows), truncated=truncated,
                           transferred_bytes=size)
            if csv_path is not None:
                digest = _publish(csv_path, lambda sink: result.rows.to_csv(sink, index=False), ".pit-screen-")
                receipt["csv"] = {"path": str(csv_path), "rows": len(result.rows), "sha256": digest}
            receipt["status"] = "inspection_complete" if result.status == "complete" else result.status
        except Exception as exc:
            receipt.update(status="unavailable", error_type=type(exc).__name__, error=str(exc)[:6000],
                           deadline_exceeded=deadline_hit.is_set())
        finally:
            stop.set()
            guard_thread.join()
        receipt["elapsed_seconds"] = round(time.monotonic() - started, 3)
        receipt["database_file_after"] = _file_state(db_path)
        receipt["database_file_changed"] = receipt["database_file_after"] != receipt["database_file_before"]
        write_receipt(output, receipt)
    except Exception as exc:
        # Refusals and argument failures occur before any database is opened.
        label = "refused" if isinstance(exc, Refused) else "unavailable"
        print(f"pit screen {label}: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2
    print(str(output))
    return 2 if receipt["status"] == "unavailable" else 0


if __name__ == "__main__":
    raise SystemExit(main())
