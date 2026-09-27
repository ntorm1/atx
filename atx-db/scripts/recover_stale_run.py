"""Recover stale ``running`` ledger rows left by a killed warehouse writer (tier-1 v2 1.3).

    run_memory_guarded.py --job-gb 0.6 --wait-minutes 30 -- \\
      python scripts/recover_stale_run.py --db <warehouse> [--receipts-dir D ...] [--sweep-spill]

1. Refuses (exit 3) while a writer may be alive: any guard receipt under ``--receipts-dir``
   (default: the tier-1 v2 receipts tree and the guard's default receipt directory) whose command
   names this database and whose status is still ``preflight``/``queued``/``running`` while its
   guard job, guard PID or command PID is alive (this process's own guard excepted); the newest
   such receipt is reported either way. DuckDB's own file lock is the second check: the exclusive
   open fails while another process holds the file.
2. Opens the file exclusively with ``bounded_config("256MB")`` (the WAL of the killed writer is
   replayed inside that budget) and a private spill directory.
3. Marks every ``activation_stage_runs`` / ``dataset_runs`` row still ``running`` as ``failed``,
   with explicit timestamps and the reason, by small keyed UPDATEs of only those rows in one
   transaction (neither table has a DEFAULT now() column; the CHECKPOINT follows at once).
   Open ``build_runs`` are left alone: their batches resume from the ledger.
4. ``CHECKPOINT``; asserts ``verify_schema(conn) == ()``; closes and asserts no WAL is left.
5. ``--sweep-spill`` deletes only dead-process spill directories carrying this exact database's
   path digest under the DuckDB spill root. Other databases' directories are never listed or
   deleted. Without the flag, matching directories are only listed.

Prints one JSON receipt. Exit codes: 0 recovered (or nothing to recover), 3 refused (live
writer), 1 failure (schema drift, WAL left, error).
"""

from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import json
import os
import re
import shutil
import sys
import time
from pathlib import Path
from typing import Any

_SCRIPT = Path(__file__).resolve()
_SRC = _SCRIPT.parents[1] / "src"
if str(_SRC) not in sys.path:
    sys.path.insert(0, str(_SRC))  # an export runs its own code, not the editable install

import duckdb  # noqa: E402

from atx_db.connection import bounded_config, private_temp_directory, spill_root  # noqa: E402

EXIT_REFUSED = 3
GUARD_JOB_ENV = "ATX_GUARD_JOB"
DEFAULT_RECEIPT_DIRS = (
    Path(r"C:\atx\.superpowers\sdd\tier1-v2\receipts"),
    Path(os.environ.get("LOCALAPPDATA") or Path.home()) / "atx-memory-guard" / "receipts",
)
_LIVE_STATUSES = ("preflight", "queued", "running")
_STALE_TABLES = (
    # table, key columns, finished column, error column
    ("activation_stage_runs", ("stage", "run_id"), "finished_at", "error"),
    ("dataset_runs", ("run_id",), "finished_at", "error_message"),
)


def _utc_now() -> dt.datetime:
    return dt.datetime.now(dt.UTC).replace(tzinfo=None)


# ---------------------------------------------------------------------------
# Liveness (Windows): a guard's named job with a process in it, or a live PID
# ---------------------------------------------------------------------------

class _Accounting(ctypes.Structure):  # JOBOBJECT_BASIC_ACCOUNTING_INFORMATION
    _fields_ = [(name, ctypes.c_longlong) for name in ("user", "kernel", "period_user", "period_kernel")] + [
        (name, ctypes.c_ulong) for name in ("page_faults", "total_processes", "active_processes", "terminated")]


def _kernel() -> Any:
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    kernel.OpenJobObjectW.restype = ctypes.c_void_p
    kernel.OpenJobObjectW.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_wchar_p]
    kernel.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
    kernel.GetProcessTimes.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(ctypes.c_ulonglong)] * 4
    kernel.QueryInformationJobObject.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_ulong,
                                                 ctypes.c_void_p]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    return kernel


def pid_alive(pid: object, created: int | None = None) -> bool:
    if os.name != "nt" or not isinstance(pid, int) or pid <= 0:
        return False
    kernel = _kernel()
    handle = kernel.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return ctypes.get_last_error() == 5  # access denied is not evidence of a dead process
    try:
        code = ctypes.c_ulong()
        if created is not None:
            times = [ctypes.c_ulonglong() for _ in range(4)]
            if not kernel.GetProcessTimes(handle, *(ctypes.byref(t) for t in times)):
                return True
            if times[0].value != created:
                return False
        return bool(kernel.GetExitCodeProcess(handle, ctypes.byref(code))) and int(code.value) == 259  # STILL_ACTIVE
    finally:
        kernel.CloseHandle(handle)


def job_alive(name: object) -> bool:
    if os.name != "nt" or not isinstance(name, str) or not name:
        return False
    kernel = _kernel()
    handle = kernel.OpenJobObjectW(0x0004, False, name)  # JOB_OBJECT_QUERY
    if not handle:
        return ctypes.get_last_error() == 5
    try:
        info = _Accounting()
        if not kernel.QueryInformationJobObject(handle, 1, ctypes.byref(info), ctypes.sizeof(info), None):
            return True  # exists, unreadable: treat as live
        return info.active_processes > 0
    finally:
        kernel.CloseHandle(handle)


# ---------------------------------------------------------------------------
# Guard receipts naming this database
# ---------------------------------------------------------------------------

def _norm(path: str) -> str:
    return os.path.normcase(os.path.normpath(path))


def _names_db(command: object, db: Path) -> bool:
    """True when an argument of the guarded command is this database (absolute, or ending in parent/name)."""
    if not isinstance(command, list):
        return False
    absolute, tail = _norm(str(db.resolve())), _norm(os.path.join(db.resolve().parent.name, db.name))
    for part in command:
        if not isinstance(part, str) or db.name.lower() not in part.lower():
            continue
        text = _norm(part)
        if text == absolute or text.endswith(tail) or _norm(os.path.abspath(part)) == absolute:
            return True
    return False


def guard_receipts(db: Path, dirs: list[Path]) -> list[dict[str, Any]]:
    """Guard receipts whose command names ``db``, newest first, each with its liveness evidence."""
    own_job = os.environ.get(GUARD_JOB_ENV)
    found: list[dict[str, Any]] = []
    for directory in dirs:
        if not directory.is_dir():
            continue
        for path in directory.rglob("*.json"):
            try:
                if path.stat().st_size > 2_000_000:
                    continue
                receipt = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            if not isinstance(receipt, dict) or "guard_version" not in receipt:
                continue
            if not _names_db(receipt.get("command"), db):
                continue
            job = (receipt.get("child_env_forced") or {}).get(GUARD_JOB_ENV)
            own = bool(own_job) and job == own_job
            status = receipt.get("status")
            identity = re.fullmatch(r"Local\\atx-memory-guard-(\d+)-(\d+)", str(job))
            created = int(identity[2]) if identity and int(identity[1]) == receipt.get("guard_pid") else None
            evidence = {"job_alive": job_alive(job),
                        "guard_pid_alive": pid_alive(receipt.get("guard_pid"), created),
                        "child_pid_alive": pid_alive(receipt.get("child_pid")) if identity is None else False}
            found.append({"path": str(path), "mtime": path.stat().st_mtime, "status": status,
                          "guard_pid": receipt.get("guard_pid"), "child_pid": receipt.get("child_pid"),
                          "job_name": job, "own": own, **evidence,
                          "live_writer": (not own) and status in _LIVE_STATUSES and any(evidence.values())})
    found.sort(key=lambda item: item["mtime"], reverse=True)
    return found


# ---------------------------------------------------------------------------
# Spill directories of dead processes
# ---------------------------------------------------------------------------

def spill_dirs_of_dead_processes(db: Path) -> list[dict[str, Any]]:
    root = spill_root()
    dead: list[dict[str, Any]] = []
    if not root.is_dir():
        return dead
    suffix = private_temp_directory(db).name.split("-", 2)[2]
    for path in root.iterdir():
        match = re.fullmatch(rf"conn-(\d+)-{re.escape(suffix)}", path.name)
        if (not path.is_dir() or not match or path.is_symlink()
                or not path.resolve().is_relative_to(root.resolve()) or pid_alive(int(match.group(1)))):
            continue
        size = sum(f.stat().st_size for f in path.rglob("*") if f.is_file())
        dead.append({"path": str(path), "pid": int(match.group(1)), "bytes": size})
    return dead


# ---------------------------------------------------------------------------
# Recovery
# ---------------------------------------------------------------------------

def _table_exists(conn: duckdb.DuckDBPyConnection, table: str) -> bool:
    row = conn.execute("SELECT count(*) FROM duckdb_tables() WHERE schema_name = 'main' AND table_name = ?",
                       [table]).fetchone()
    return bool(row and row[0])


def recover(conn: duckdb.DuckDBPyConnection, reason: str) -> list[dict[str, Any]]:
    """Mark every ``running`` row of the stale-run tables ``failed`` (one transaction); returns the rows."""
    stamp = _utc_now()
    recovered: list[dict[str, Any]] = []
    conn.execute("BEGIN TRANSACTION")
    try:
        for table, keys, finished, error in _STALE_TABLES:
            if not _table_exists(conn, table):
                continue
            rows = conn.execute(
                f"SELECT {', '.join(keys)}, started_at FROM {table} WHERE status = 'running' ORDER BY started_at"
            ).fetchall()
            for row in rows:
                key = dict(zip(keys, row[:len(keys)], strict=True))
                where = " AND ".join(f"{name} = ?" for name in keys)
                changed = conn.execute(
                    f"UPDATE {table} SET status = 'failed', {finished} = ?, {error} = ? "
                    f"WHERE {where} AND status = 'running'",
                    [stamp, reason, *key.values()],
                ).fetchone()
                if not changed or int(changed[0]) != 1:
                    raise RuntimeError(f"{table} {key}: expected to update exactly one running row")
                recovered.append({"table": table, **key, "started_at": str(row[-1]), "finished_at": str(stamp)})
        conn.execute("COMMIT")
    except BaseException:
        conn.execute("ROLLBACK")
        raise
    return recovered


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db", type=Path, required=True)
    parser.add_argument("--receipts-dir", type=Path, action="append",
                        help="directory tree of guard receipts to check (repeatable; default: tier1-v2 receipts "
                             "and the guard's default receipt directory)")
    parser.add_argument("--memory-limit", default="256MB")
    parser.add_argument("--sweep-spill", action="store_true", help="delete spill dirs of dead processes")
    args = parser.parse_args(argv)
    if not os.environ.get(GUARD_JOB_ENV):
        raise SystemExit("recovery runs only under run_memory_guarded.py")
    db = args.db.resolve()
    began = time.monotonic()
    report: dict[str, Any] = {"db": str(db), "started_utc": _utc_now().isoformat()}
    receipts = guard_receipts(db, args.receipts_dir or list(DEFAULT_RECEIPT_DIRS))
    report["guard_receipts_naming_db"] = len(receipts)
    report["newest_receipt"] = receipts[0] if receipts else None
    live = [item for item in receipts if item["live_writer"]]
    if live:
        report.update(status="refused_live_writer", live_writers=live)
        print(json.dumps(report, indent=1, default=str))
        return EXIT_REFUSED
    if not db.is_file():
        raise SystemExit(f"no database at {db}")
    wal = Path(str(db) + ".wal")
    report["wal_before_open"] = wal.stat().st_size if wal.exists() else None
    try:
        conn = duckdb.connect(str(db), config=bounded_config(args.memory_limit, 1,
                                                             temp_directory=private_temp_directory(db)))
    except duckdb.IOException as exc:
        if "lock" in str(exc).lower():
            report.update(status="refused_file_locked", error=str(exc)[:1000])
            print(json.dumps(report, indent=1, default=str))
            return EXIT_REFUSED
        raise
    code = 0
    try:
        conn.execute("SET TimeZone = 'UTC'")
        reason = (f"stale run recovered by scripts/recover_stale_run.py at {_utc_now().isoformat()}Z: its writer was "
                  "gone (no live guard receipt or file lock); the row had stayed 'running'")
        report["recovered"] = recover(conn, reason)
        conn.execute("CHECKPOINT")
        if _table_exists(conn, "build_runs"):
            report["open_build_runs"] = [
                {"stage": stage, "run_key": key, "created_at": str(created)}
                for stage, key, created in conn.execute(
                    "SELECT stage, run_key, created_at FROM build_runs WHERE status = 'open' ORDER BY created_at"
                ).fetchall()]
        from atx_db.migration_admin import SchemaVerificationError, verify_schema

        try:
            report["verify_schema"] = list(verify_schema(conn))
        except SchemaVerificationError as exc:
            report["verify_schema"] = [str(row) for row in exc.drift] or [str(exc)]
            code = 1
        remaining = sum(
            int((conn.execute(f"SELECT count(*) FROM {table} WHERE status = 'running'").fetchone() or (0,))[0])
            for table, *_ in _STALE_TABLES if _table_exists(conn, table))
        report["running_rows_after"] = remaining
        code = code or (1 if remaining else 0)
    finally:
        conn.close()
    report["wal_after_close"] = wal.stat().st_size if wal.exists() else None
    if report["wal_after_close"]:
        code = 1
    dead = spill_dirs_of_dead_processes(db)
    report["dead_spill_dirs"] = dead
    if args.sweep_spill:
        for item in dead:
            shutil.rmtree(item["path"])
        report["swept_spill_bytes"] = sum(item["bytes"] for item in dead)
    report.update(status="recovered" if code == 0 else "failed", seconds=round(time.monotonic() - began, 1))
    print(json.dumps(report, indent=1, default=str))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
