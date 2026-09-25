"""Shared read-only guard contract of the governed-read runners (C1 desk pack, P7 PIT screen).

Stdlib only, and importable without the ``atx_db`` package: the runners load
this file by path (:func:`load_contract` is the pattern), because importing the
package imports DuckDB, and every refusal must happen before any DuckDB import.

Contract (extracted from C1 ``c3f9cb80``; behavior unchanged, plus C1-N1):

* The governed warehouse is anchored explicitly (:data:`PRODUCTION_DB`), never
  derived from a runner's location, so a runner copy (RX3 pinned export, pool
  worktree) protects the same file. A ``--governed-warehouse`` override must be
  named in the guard receipt's command.
* L1 inspection refuses, before any DuckDB call: the governed warehouse (also
  through a hard link), any file inside an ``atx-db/data`` directory of any
  checkout, export or worktree (and the runner's own), the checkout-root
  ``warehouse_template.duckdb``, any database larger than 1 GiB, and any database
  file with more than one hard link (a link placed outside ``data/`` to a file
  inside it, C1 re-review N1).
* Governed production reads only the governed warehouse, only inside the
  controller memory guard (:func:`verify_guard_receipt`: receipt of this launch,
  running, fresh, job cap <= 2 GiB, command naming the runner with
  ``--production``, a memory-capped Windows job no larger than the receipt's
  cap). Fixed limits: 1GB, 1 thread, spill <= 2GB.
* Effective DuckDB settings are re-read (read-only access, the private spill
  directory, limits as ceilings, external access off) and then locked
  (``lock_configuration``) and read back, so no later SQL can lift them.
* Receipts record the runner root, its script sha256 and the export commit.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import importlib.util
import itertools
import json
import math
import os
import re
import sys
import tempfile
import time
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import Any, NamedTuple

GIB = 1024 ** 3
MAX_RECEIPT_BYTES = 10_000_000
#: The only database a governed production read may open. Anchored to the live
#: checkout, never to a runner's location (copies run from exports).
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
L1_MEMORY_LIMIT = "256MB"
L1_THREADS = 1
# DuckDB reports settings rounded to one decimal of a binary unit.
SETTING_TOLERANCE = 1.001
SIZE_UNITS = {"bytes": 1, "b": 1, "kib": 1024, "mib": 1024 ** 2, "gib": 1024 ** 3, "tib": 1024 ** 4,
              "kb": 10 ** 3, "mb": 10 ** 6, "gb": 10 ** 9, "tb": 10 ** 12}
SETTINGS_SQL = """
    SELECT name, value FROM duckdb_settings()
    WHERE name IN ('memory_limit','threads','max_temp_directory_size','enable_external_access',
                   'preserve_insertion_order','access_mode','TimeZone','temp_directory','lock_configuration')
"""


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


def load_contract(runner_root: Path) -> Any:
    """This module loaded by path from ``runner_root/src/atx_db`` (no package import, no DuckDB)."""
    path = (Path(runner_root) / "src" / "atx_db" / "governed_read.py").resolve()
    name = "_atx_governed_read_" + hashlib.sha256(str(path).encode("utf-8")).hexdigest()[:12]
    if name not in sys.modules:
        spec = importlib.util.spec_from_file_location(name, path)
        if spec is None or spec.loader is None:
            raise ImportError(f"governed-read contract not found at {path}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    return sys.modules[name]


def memory_limit_arg(text: str) -> str:
    """argparse type for the L1 memory override (32MB..512MB)."""
    import argparse

    match = re.fullmatch(r"([0-9]+)MB", text.upper())
    if match is None or not 32 <= int(match[1]) <= 512:
        raise argparse.ArgumentTypeError("memory-limit must be 32MB..512MB")
    return text.upper()


def same_file(left: Path, right: Path) -> bool:
    return left == right or (left.exists() and right.exists() and left.samefile(right))


def in_data_directory(path: Path, runner_root: Path) -> bool:
    """Inside any ``atx-db/data`` directory (any checkout, export or worktree) or the runner's own."""
    parts = [part.lower() for part in path.parts]
    return (any(pair == ("atx-db", "data") for pair in itertools.pairwise(parts))
            or path.is_relative_to((Path(runner_root) / "data").resolve()))


def validate_database(db_path: Path, *, production: bool, governed: Path, runner_root: Path,
                      l1_max_bytes: int = L1_MAX_DATABASE_BYTES) -> Path:
    """Resolve links and classify the database before any DuckDB call."""
    db_path, governed = Path(db_path).resolve(), Path(governed).resolve()
    for template in (Path(runner_root) / "warehouse_template.duckdb",
                     governed.parent.parent / "warehouse_template.duckdb"):
        if same_file(db_path, template.resolve()):
            raise Refused("the checkout-root template is forbidden; use a copy")
    is_production = same_file(db_path, governed)
    if production and not is_production:
        raise Refused(f"--production reads only the governed warehouse {governed}")
    if not production:
        if is_production or in_data_directory(db_path, runner_root):
            raise Refused("the governed warehouse and every database in an atx-db/data directory require "
                          f"--production and the {GUARD_RECEIPT_ENV} guard receipt; L1 reads a copy or tiny fixture")
        if db_path.is_file() and db_path.stat().st_size > l1_max_bytes:
            raise Refused(f"L1 opens only fixtures and schema copies up to {l1_max_bytes} bytes")
        if db_path.is_file() and db_path.stat().st_nlink > 1:
            raise Refused("L1 refuses hard-linked database files (a link may reach a file in atx-db/data); "
                          "read a copy")
    if not db_path.is_file():
        raise ValueError("db-path must be an existing database file")
    return db_path


def validate_output(output: Path, *, runner_root: Path, suffix: str = ".json",
                    inputs: Sequence[Path] = (), label: str = "output-json") -> Path:
    """A fresh ``suffix`` path in an existing directory, never an input, never in an atx-db/data directory."""
    output = Path(output).resolve()
    if any(output == Path(item).resolve() for item in inputs) or output.exists() \
            or output.suffix.lower() != suffix or not output.parent.is_dir():
        raise ValueError(f"{label} must be a fresh {suffix} path in an existing directory")
    if in_data_directory(output, runner_root):
        raise ValueError(f"{label} must not be written in an atx-db/data directory")
    return output


def resolve_limits(*, production: bool, memory_limit: str | None, threads: int | None,
                   timeout_seconds: int | None, max_rows: int, max_result_bytes: int,
                   l1_timeouts: tuple[int, int, int], production_timeouts: tuple[int, int, int],
                   timeout_scope: str = "per query") -> Limits:
    if not 1 <= max_rows <= 1000 or not 1024 <= max_result_bytes <= 1_000_000:
        raise ValueError("max-rows must be 1..1000 and max-result-bytes 1024..1000000")
    if production:
        if memory_limit is not None or threads is not None:
            raise Refused("production memory and threads are fixed (1GB, 1 thread); do not pass overrides")
        low, high, default = production_timeouts
        memory, memory_bytes, thread_count = PRODUCTION_MEMORY_LIMIT, PRODUCTION_MEMORY_BYTES, PRODUCTION_THREADS
        spill, spill_bytes, mode = PRODUCTION_SPILL_LIMIT, PRODUCTION_SPILL_BYTES, "governed_production"
    else:
        low, high, default = l1_timeouts
        memory = memory_limit or L1_MEMORY_LIMIT
        memory_bytes = int(memory.removesuffix("MB")) * 10 ** 6
        thread_count, spill, spill_bytes, mode = threads or L1_THREADS, "0B", 0, "l1_inspection"
    timeout = default if timeout_seconds is None else timeout_seconds
    if not low <= timeout <= high:
        raise ValueError(f"timeout-seconds must be {low}..{high} {timeout_scope} in {mode}")
    return Limits(mode, memory, memory_bytes, thread_count, spill, spill_bytes, timeout, max_rows, max_result_bytes)


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


def read_guard_receipt(path: Path) -> dict[str, Any] | None:
    """The parsed receipt, or None while it is absent or being rewritten."""
    try:
        if path.stat().st_size > GUARD_RECEIPT_MAX_BYTES:
            return None
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return value if isinstance(value, dict) else None


def verify_guard_receipt(script_name: str, governed_override: Path | None = None, *,
                         env_name: str = GUARD_RECEIPT_ENV, wait_seconds: float = GUARD_RECEIPT_WAIT_SECONDS,
                         job_memory: Callable[[], int | None] = job_memory_limit_bytes) -> dict[str, Any]:
    """Prove this process runs inside the controller guard's memory-capped job.

    ``governed_override`` (--governed-warehouse) must be named in the receipt's
    command with the same resolved path, so the override is part of the guard record.
    """
    named = os.environ.get(env_name)
    if not named:
        raise Refused(f"{env_name} is not set: production reads run only under run_memory_guarded.py")
    path = Path(named).resolve()
    if path.suffix.lower() != ".json":
        raise Refused("guard receipt must be a .json file")
    own = {os.getpid(), os.getppid()}
    deadline = time.monotonic() + wait_seconds
    while True:
        receipt = read_guard_receipt(path)
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
            or not any(Path(part).name == script_name for part in command) or "--production" not in command:
        raise Refused(f"guard receipt command must launch {script_name} with --production")
    if governed_override is not None:
        flag = "--governed-warehouse"
        listed = [Path(command[i + 1]).resolve() for i, part in enumerate(command[:-1]) if part == flag]
        if listed != [Path(governed_override).resolve()]:
            raise Refused(f"{flag} must appear once in the guard receipt's command with the same path")
    job_bytes = job_memory()
    if job_bytes is None:
        raise Refused("this process is not inside a memory-capped job object")
    if job_bytes > job_gb * GIB + 2 ** 20:
        raise Refused(f"job memory cap {job_bytes} exceeds the guard receipt's {job_gb} GiB")
    return {"receipt_path": str(path), "receipt_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "child_pid": receipt["child_pid"], "job_limit_gb": job_gb, "job_memory_limit_bytes": job_bytes,
            "guard_headroom": receipt.get("headroom"), "verified_at_utc": dt.datetime.now(dt.UTC).isoformat()}


def open_read_only(db_path: Path, memory: str, threads: int, spill_directory: str | None = None,
                   spill_limit: str = "0B") -> Any:
    """A DuckDB connection opened ``read_only`` with the declared limits (external access off)."""
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


def setting_bytes(text: str | None) -> float | None:
    match = re.fullmatch(r"\s*([0-9]+(?:\.[0-9]+)?)\s*([A-Za-z]+)\s*", text or "")
    if match is None or match[2].lower() not in SIZE_UNITS:
        return None
    return float(match[1]) * SIZE_UNITS[match[2].lower()]


def verify_settings(con: Any, limits: Limits, spill_directory: str | None = None, *,
                    read_only_databases: Sequence[str] | None = None) -> dict[str, Any]:
    """Re-read DuckDB's effective settings, enforce them, then lock the configuration.

    Limits are ceilings; a governed spill must use the private directory. Access
    must be ``read_only`` -- or, for a runner whose writable scratch catalog
    attaches its inputs, every database named in ``read_only_databases`` must be
    attached read-only. After ``lock_configuration`` no SET/RESET/PRAGMA can lift
    the limits; an already locked connection is verified as is.
    """
    settings = dict(con.execute(SETTINGS_SQL).fetchall())
    memory = setting_bytes(settings.get("memory_limit"))
    spill = setting_bytes(settings.get("max_temp_directory_size"))
    problems = []
    if read_only_databases is None:
        if settings.get("access_mode") != "read_only":
            problems.append(f"access_mode {settings.get('access_mode')}")
    else:
        attached = dict(con.execute("SELECT database_name, readonly FROM duckdb_databases()").fetchall())
        problems += [f"{name} not attached read-only" for name in read_only_databases if attached.get(name) is not True]
    if spill_directory is not None and not same_file(Path(str(settings.get("temp_directory"))).resolve(),
                                                     Path(spill_directory).resolve()):
        problems.append(f"temp_directory {settings.get('temp_directory')}")
    if memory is None or memory > limits.memory_bytes * SETTING_TOLERANCE:
        problems.append(f"memory_limit {settings.get('memory_limit')}")
    if spill is None or spill > limits.spill_bytes * SETTING_TOLERANCE:
        problems.append(f"max_temp_directory_size {settings.get('max_temp_directory_size')}")
    if not str(settings.get("threads", "")).isdigit() or not 1 <= int(settings["threads"]) <= limits.threads:
        problems.append(f"threads {settings.get('threads')}")
    if str(settings.get("enable_external_access")).lower() != "false":
        problems.append("external access enabled")
    if problems:
        raise RuntimeError("effective DuckDB settings exceed the declared limits: " + "; ".join(problems))
    if str(settings.get("lock_configuration")).lower() != "true":
        con.execute("SET lock_configuration = true")
    locked = dict(con.execute(SETTINGS_SQL).fetchall())
    if str(locked.get("lock_configuration")).lower() != "true" or locked != {**settings, "lock_configuration":
                                                                              locked.get("lock_configuration")}:
        raise RuntimeError("DuckDB configuration did not lock with the verified settings")
    return locked


def schema_version(con: Any, table: str = "schema_migrations") -> str | None:
    import duckdb

    if not re.fullmatch(r"[a-z_]+(\.[a-z_]+){0,2}", table):
        raise ValueError("unsupported schema ledger name")
    try:
        row = con.execute(f"SELECT max(version) FROM {table} WHERE version ~ '^[0-9]+$'").fetchone()
    except duckdb.Error:
        return None
    return None if row is None else row[0]


def runner_provenance(runner_root: Path, script: Path) -> dict[str, Any]:
    """Which runner and tree ran: the live checkout or an RX3 pinned export (exports/<sha>/atx-db)."""
    root = Path(runner_root)
    commit = root.parent.name if re.fullmatch(r"[0-9a-f]{40}", root.parent.name) else None
    return {"root": str(root), "script_sha256": hashlib.sha256(Path(script).read_bytes()).hexdigest(),
            "export_commit": commit}


def file_state(path: Path) -> dict[str, int]:
    stat = Path(path).stat()
    return {"bytes": stat.st_size, "mtime_ns": stat.st_mtime_ns}


def publish_text(output: Path, write: Callable[[Any], None], *, prefix: str, newline: str | None = None) -> str:
    """Write through a private temporary file, link it into place (never overwrite); return its sha256."""
    fd, temporary = tempfile.mkstemp(prefix=prefix, suffix=Path(output).suffix, dir=Path(output).parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline=newline) as sink:
            write(sink)
            sink.flush()
            os.fsync(sink.fileno())
        digest = hashlib.sha256(Path(temporary).read_bytes()).hexdigest()
        os.link(temporary, output)
    finally:
        os.unlink(temporary)
    return digest


def write_receipt(output: Path, receipt: dict[str, Any], *, prefix: str) -> str:
    encoded = json.dumps(receipt, default=str, sort_keys=True, indent=2, allow_nan=False) + "\n"
    if len(encoded.encode("utf-8")) > MAX_RECEIPT_BYTES:
        raise ValueError("receipt exceeds 10MB cap")
    return publish_text(output, lambda sink: sink.write(encoded), prefix=prefix)


__all__ = [
    "GIB",
    "GUARD_RECEIPT_ENV",
    "L1_MAX_DATABASE_BYTES",
    "PRODUCTION_DB",
    "PRODUCTION_DISK_FLOOR_BYTES",
    "Limits",
    "Refused",
    "file_state",
    "in_data_directory",
    "job_memory_limit_bytes",
    "load_contract",
    "open_read_only",
    "publish_text",
    "resolve_limits",
    "runner_provenance",
    "same_file",
    "schema_version",
    "setting_bytes",
    "validate_database",
    "validate_output",
    "verify_guard_receipt",
    "verify_settings",
    "write_receipt",
]
