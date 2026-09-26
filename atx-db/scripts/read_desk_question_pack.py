"""Bounded read-only runner for the five desk SQL contracts; never certification.

Two access modes; every output keeps ``production_qualified=false``.

The access guard is the shared governed-read contract
(src/atx_db/governed_read.py, loaded by path so no DuckDB import precedes the
refusals; the P7 PIT screen runner uses the same contract). The governed
warehouse is anchored explicitly (PRODUCTION_DB =
C:\\atx\\atx-db\\data\\warehouse.duckdb), never derived from this script's own
location, so a copy of the runner (RX3 pinned export, pool worktree) protects
the same file. Q3 needs schema >= 0327 (A5 identity label columns); on an older
schema it is reported unavailable.

L1 inspection (default): schema copies and small fixture databases only.
Refused before any DuckDB call: the governed warehouse (also through a hard
link), any file inside an ``atx-db/data`` directory of any checkout, export or
worktree (and inside the governed warehouse's directory), the checkout-root
warehouse_template.duckdb, any database larger than 1 GiB (fixtures and
schema templates are far smaller; warehouses and backups are not) and any
database file with more than one hard link. DuckDB 32-512MB, 1-2 threads, no
spill, 1-60 s per query.
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
    rewritten within the last 120 s, job_limit_gb <= 2 (a ceiling check; the
    memory guard itself refuses caps above 1.0, ruling C-65), command naming
    this script with --production;
  * this process inside a Windows job whose memory limit is <= that cap;
  * >= 3 GiB free beside the output (spill cap 2GB + 1 GiB floor).
The runner may run from an RX3 pinned export (it then executes the export's SQL
files); the receipt records the runner root, its script sha256 and the export
commit when the root sits in exports/<sha>/atx-db.
Connection: read_only, memory_limit 512MB, 1 thread, spill <= 2GB in a private
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
    --job-gb 1.0 --wait-minutes 30 --receipt "<dir>\\desk-guard.json" -- `
    .venv\\Scripts\\python.exe <runner>\\read_desk_question_pack.py --production `
    --db-path data\\warehouse.duckdb --mode run --output-json "<dir>\\desk-pack.json"
"""

from __future__ import annotations

import argparse
import contextlib
import datetime as dt
import hashlib
import importlib.util
import json
import math
import re
import shutil
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT_NAME = Path(__file__).name


def _load_contract():
    """The shared governed-read contract, loaded by path (stdlib only; no package or DuckDB import).

    Same module name as ``governed_read.load_contract``, so every runner of one tree shares one copy.
    """
    path = (ROOT / "src" / "atx_db" / "governed_read.py").resolve()
    name = "_atx_governed_read_" + hashlib.sha256(str(path).encode("utf-8")).hexdigest()[:12]
    if name not in sys.modules:
        spec = importlib.util.spec_from_file_location(name, path)
        if spec is None or spec.loader is None:
            raise ImportError(f"governed-read contract not found at {path}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    return sys.modules[name]


_gr = _load_contract()

SQL_FILES = {
    "q1": "desk-q1-cvx-eps.sql",
    "q2": "desk-q2-acceleration.sql",
    "q3": "desk-q3-valuation-profitability.sql",
    "q4": "desk-q4-accrual-leverage.sql",
    "q5": "desk-q5-monthly-factor-portfolios.sql",
}
MAX_RECEIPT_BYTES = _gr.MAX_RECEIPT_BYTES
GIB = _gr.GIB

#: The only database the governed production mode may read. Anchored to the
#: live checkout, never to this script's location (copies run from exports).
PRODUCTION_DB = _gr.PRODUCTION_DB
#: L1 never opens a database larger than this (schema templates are ~60 MB).
L1_MAX_DATABASE_BYTES = _gr.L1_MAX_DATABASE_BYTES
#: Environment variable naming the controller guard receipt of this launch.
GUARD_RECEIPT_ENV = _gr.GUARD_RECEIPT_ENV
GUARD_RECEIPT_MAX_BYTES = _gr.GUARD_RECEIPT_MAX_BYTES
GUARD_RECEIPT_MAX_AGE_SECONDS = _gr.GUARD_RECEIPT_MAX_AGE_SECONDS
GUARD_RECEIPT_WAIT_SECONDS = _gr.GUARD_RECEIPT_WAIT_SECONDS
PRODUCTION_MAX_JOB_BYTES = _gr.PRODUCTION_MAX_JOB_BYTES
#: DuckDB budget inside the 1.0 GiB guard job (index §4 budget table, set-based pass): the
#: shared contract's 1GB needs a 2 GiB job, which the guard refuses since ruling C-65.
PRODUCTION_MEMORY_LIMIT = "512MB"
PRODUCTION_MEMORY_BYTES = 512 * 10 ** 6
PRODUCTION_THREADS = _gr.PRODUCTION_THREADS
PRODUCTION_SPILL_LIMIT = _gr.PRODUCTION_SPILL_LIMIT
PRODUCTION_SPILL_BYTES = _gr.PRODUCTION_SPILL_BYTES
PRODUCTION_DISK_FLOOR_BYTES = _gr.PRODUCTION_DISK_FLOOR_BYTES
PRODUCTION_TIMEOUT_SECONDS = (1, 1800, 600)
L1_TIMEOUT_SECONDS = (1, 60, 30)
L1_MEMORY_LIMIT = _gr.L1_MEMORY_LIMIT
L1_THREADS = _gr.L1_THREADS

Refused = _gr.Refused
Limits = _gr.Limits
memory_limit = _gr.memory_limit_arg
_same_file = _gr.same_file
_setting_bytes = _gr.setting_bytes
_SETTINGS_SQL = _gr.SETTINGS_SQL
_read_guard_receipt = _gr.read_guard_receipt
job_memory_limit_bytes = _gr.job_memory_limit_bytes
open_read_only = _gr.open_read_only
schema_version = _gr.schema_version
_file_state = _gr.file_state


def utc_cutoff(text: str) -> dt.datetime:
    value = dt.datetime.fromisoformat(text.replace("Z", "+00:00"))
    if value.tzinfo is None or value.utcoffset() is None:
        raise argparse.ArgumentTypeError("cutoff requires an explicit UTC offset")
    return value.astimezone(dt.UTC).replace(tzinfo=None)


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


def _in_data_directory(path: Path) -> bool:
    """Inside any ``atx-db/data`` directory (any checkout, export or worktree) or this runner's own."""
    return _gr.in_data_directory(path, ROOT)


def validate_paths(db_path: Path, output: Path, *, production: bool,
                   governed: Path | None = None) -> tuple[Path, Path]:
    """Resolve links and classify the database before any DuckDB call (shared contract)."""
    db_path = _gr.validate_database(db_path, production=production, governed=governed or PRODUCTION_DB,
                                    runner_root=ROOT, l1_max_bytes=L1_MAX_DATABASE_BYTES)
    return db_path, _gr.validate_output(output, runner_root=ROOT, suffix=".json", inputs=(db_path,))


def resolve_limits(args) -> Limits:
    limits = _gr.resolve_limits(production=args.production, memory_limit=args.memory_limit, threads=args.threads,
                                timeout_seconds=args.timeout_seconds, max_rows=args.max_rows,
                                max_result_bytes=args.max_result_bytes, l1_timeouts=L1_TIMEOUT_SECONDS,
                                production_timeouts=PRODUCTION_TIMEOUT_SECONDS, timeout_scope="per query")
    if args.production:
        limits = limits._replace(memory_limit=PRODUCTION_MEMORY_LIMIT, memory_bytes=PRODUCTION_MEMORY_BYTES)
    return limits


def verify_guard_receipt(governed_override: Path | None = None) -> dict:
    """Prove this process runs inside the controller guard's memory-capped job (shared contract)."""
    return _gr.verify_guard_receipt(SCRIPT_NAME, governed_override, env_name=GUARD_RECEIPT_ENV,
                                    wait_seconds=GUARD_RECEIPT_WAIT_SECONDS, job_memory=job_memory_limit_bytes)


def verify_settings(con, limits: Limits, spill_directory: str | None = None) -> dict:
    """Re-read, enforce (access_mode read_only) and lock DuckDB's effective settings (shared contract)."""
    return _gr.verify_settings(con, limits, spill_directory)


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
    return _gr.runner_provenance(ROOT, Path(__file__))


def write_receipt(output: Path, receipt: dict) -> None:
    _gr.write_receipt(output, receipt, prefix=".desk-pack-")


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
