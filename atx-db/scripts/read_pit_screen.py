"""Bounded read-only point-in-time universe screen; inspection, never certification.

"Give me field X for every member of universe U as of time T": one row per
(tier-1 universe member active at --as-of, --field) from
atx_db.asof.cross_section (run_cross_section), with value, value_status, lineage
status, clocks, staleness and the identity / universe / availability bases.
Every output keeps production_qualified=false.

Access is the shared governed-read contract (src/atx_db/governed_read.py, the
same module the C1 desk-pack runner uses; loaded by path, so no DuckDB import
precedes the refusals). The governed warehouse is anchored explicitly
(C:\\atx\\atx-db\\data\\warehouse.duckdb), never derived from this script's
location, so a copy of the runner (RX3 pinned export, pool worktree) protects
the same file; the default research store is anchored beside it
(data\\research\\research.duckdb).

L1 inspection (default): schema copies and small fixture databases only.
Refused before any DuckDB call: the governed warehouse (also through a hard
link), any file inside an ``atx-db/data`` directory of any checkout, export or
worktree, the checkout-root warehouse_template.duckdb, any database larger than
1 GiB and any database file with more than one hard link -- for the warehouse
and for --research-db-path alike. DuckDB 32-512MB, 1-2 threads, no spill,
screen deadline 1-300 s.

Governed production read (--production): the governed warehouse only (or
--governed-warehouse PATH, which must also appear in the guard receipt's
command), only inside the controller memory guard: ATX_DESK_GUARD_RECEIPT names
the run_memory_guarded.py receipt of THIS launch (running, this process, <= 120 s
old, job_limit_gb <= 2, command naming this script with --production), this
process sits in a memory-capped Windows job no larger than that, and >= 3 GiB
are free beside the output. A research store is then the anchored default one
or a copy outside every atx-db/data directory. Limits: 1GB, 1 thread, spill <=
2GB, deadline 1-1800 s (default 900).

Connection: a private scratch DuckDB (removed afterwards) in a temporary
directory beside the output, with the warehouse (and the research store for
feature: fields) attached READ_ONLY; spill only into that directory; external
access disabled once attached; configuration locked (lock_configuration). The
effective settings and read-only attachments are re-read and verified. One
watchdog bounds the whole screen. The receipt carries at most --max-rows rows
(plus a truncation flag and a byte cap), the runner root, script sha256 and
export commit, the guard receipt, settings, schema version and warehouse file
metadata before and after; the full screen streams in bounded chunks to a
fresh --output-csv (at most --max-screen-rows rows, <= 1M), never through one
pandas frame.

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
import importlib.util
import json
import shutil
import sys
import threading
import time
from pathlib import Path
from typing import Any

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

PRODUCTION_DB = _gr.PRODUCTION_DB
L1_MAX_DATABASE_BYTES = _gr.L1_MAX_DATABASE_BYTES
GUARD_RECEIPT_ENV = _gr.GUARD_RECEIPT_ENV
GUARD_RECEIPT_WAIT_SECONDS = _gr.GUARD_RECEIPT_WAIT_SECONDS
PRODUCTION_DISK_FLOOR_BYTES = _gr.PRODUCTION_DISK_FLOOR_BYTES
PRODUCTION_TIMEOUT_SECONDS = (1, 1800, 900)
L1_TIMEOUT_SECONDS = (1, 300, 120)
#: Screen rows (members x fields) the CLI accepts; the screen streams to CSV.
MAX_SCREEN_ROWS = 1_000_000
Refused = _gr.Refused
job_memory_limit_bytes = _gr.job_memory_limit_bytes


def utc_timestamp(text: str) -> dt.datetime:
    value = dt.datetime.fromisoformat(text.replace("Z", "+00:00"))
    if value.tzinfo is None or value.utcoffset() is None:
        raise argparse.ArgumentTypeError("--as-of requires an explicit UTC offset")
    return value.astimezone(dt.UTC)


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--db-path", type=Path, required=True)
    p.add_argument("--output-json", type=Path, required=True)
    p.add_argument("--output-csv", type=Path, default=None, help="fresh .csv for the full screen (streamed)")
    p.add_argument("--production", action="store_true",
                   help="governed read of the governed warehouse inside the controller guard")
    p.add_argument("--governed-warehouse", type=Path, default=None,
                   help="production only: override the anchored warehouse; must appear in the guard receipt")
    p.add_argument("--as-of", type=utc_timestamp, required=True, help="knowledge cutoff, ISO with UTC offset")
    p.add_argument("--field", action="append", required=True, help="repeatable; see the module docstring")
    p.add_argument("--basis", choices=("strict", "reconstructed"), default=None)
    p.add_argument("--universe-id", default=None)
    p.add_argument("--research-db-path", type=Path, default=None)
    p.add_argument("--feature-version", default=None)
    p.add_argument("--unverified-vendor-shares", action="store_true",
                   help="allow line_market_cap (vendor share counts, labeled unverified)")
    p.add_argument("--session-lookback-days", type=int, default=10)
    p.add_argument("--memory-limit", type=_gr.memory_limit_arg, default=None, help="L1 only (default 256MB)")
    p.add_argument("--threads", type=int, choices=(1, 2), default=None, help="L1 only (default 1)")
    p.add_argument("--max-rows", type=int, default=1000, help="rows copied into the JSON receipt (1..1000)")
    p.add_argument("--max-result-bytes", type=int, default=1_000_000)
    p.add_argument("--max-screen-rows", type=int, default=200_000,
                   help=f"members x fields bound of the screen itself (1..{MAX_SCREEN_ROWS})")
    p.add_argument("--timeout-seconds", type=int, default=None,
                   help="screen deadline: L1 1..300 (default 120), production 1..1800 (default 900)")
    return p


def validate_paths(db_path: Path, output: Path, csv: Path | None, research: Path | None, *, production: bool,
                   governed: Path | None = None) -> tuple[Path, Path, Path | None, Path | None]:
    """Classify every input and output before any DuckDB call (shared contract)."""
    governed = (governed or PRODUCTION_DB).resolve()
    db_path = _gr.validate_database(db_path, production=production, governed=governed, runner_root=ROOT,
                                    l1_max_bytes=L1_MAX_DATABASE_BYTES)
    if research is not None:
        research = Path(research).resolve()
        if _gr.same_file(research, db_path):
            raise Refused("the research store must be a separate research database")
        anchored = governed.parent / "research" / "research.duckdb"
        if production and _gr.same_file(research, anchored):
            pass  # the governed default research store (RX6), read-only inside the guard
        else:
            # Any other research store is inspected like an L1 database: a copy
            # outside every atx-db/data directory, <= 1 GiB, one hard link.
            research = _gr.validate_database(research, production=False, governed=governed, runner_root=ROOT,
                                             l1_max_bytes=L1_MAX_DATABASE_BYTES)
    inputs = tuple(path for path in (db_path, research) if path is not None)
    output = _gr.validate_output(output, runner_root=ROOT, suffix=".json", inputs=inputs)
    if csv is not None:
        csv = _gr.validate_output(csv, runner_root=ROOT, suffix=".csv", inputs=(*inputs, output),
                                  label="output-csv")
    return db_path, output, csv, research


def resolve_limits(args: argparse.Namespace) -> Any:
    if not 1 <= args.max_screen_rows <= MAX_SCREEN_ROWS:
        raise ValueError(f"max-screen-rows must be 1..{MAX_SCREEN_ROWS}")
    if not 1 <= args.session_lookback_days <= 31:
        raise ValueError("session-lookback-days must be 1..31")
    return _gr.resolve_limits(production=args.production, memory_limit=args.memory_limit, threads=args.threads,
                              timeout_seconds=args.timeout_seconds, max_rows=args.max_rows,
                              max_result_bytes=args.max_result_bytes, l1_timeouts=L1_TIMEOUT_SECONDS,
                              production_timeouts=PRODUCTION_TIMEOUT_SECONDS, timeout_scope="per screen")


def verify_guard_receipt(governed_override: Path | None = None) -> dict[str, Any]:
    return _gr.verify_guard_receipt(SCRIPT_NAME, governed_override, env_name=GUARD_RECEIPT_ENV,
                                    wait_seconds=GUARD_RECEIPT_WAIT_SECONDS, job_memory=job_memory_limit_bytes)


def _bounded_rows(frame: Any, limits: Any) -> tuple[list[dict[str, Any]], int]:
    """At most max_rows rows and max_result_bytes of JSON."""
    records = json.loads(frame.head(limits.max_rows).to_json(orient="records", date_format="iso",
                                                              double_precision=15))
    rows, size = [], 0
    for record in records:
        encoded = len(json.dumps(record, sort_keys=True, allow_nan=False).encode("utf-8"))
        if size + encoded > limits.max_result_bytes:
            break
        rows.append(record)
        size += encoded
    return rows, size


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.governed_warehouse is not None and not args.production:
            raise Refused("--governed-warehouse is a production-only override")
        governed = args.governed_warehouse or PRODUCTION_DB
        db_path, output, csv_path, research = validate_paths(
            args.db_path, args.output_json, args.output_csv, args.research_db_path, production=args.production,
            governed=governed)
        limits = resolve_limits(args)
        guard = None
        if args.production:
            guard = verify_guard_receipt(args.governed_warehouse)
            free = shutil.disk_usage(output.parent).free
            if free < PRODUCTION_DISK_FLOOR_BYTES:
                raise Refused(f"{free} bytes free beside the output; the spill cap needs "
                              f"{PRODUCTION_DISK_FLOOR_BYTES}")
        receipt: dict[str, Any] = {
            "contract": "pit-screen-governed-v2" if args.production else "pit-screen-l1-v2",
            "access_mode": limits.access_mode, "database": str(db_path), "research_database":
                None if research is None else str(research),
            "read_only": True, "production_qualified": False, "limits": {**limits._asdict(),
                                                                         "max_screen_rows": args.max_screen_rows},
            "guard": guard, "governed_warehouse": str(governed),
            "governed_warehouse_override": args.governed_warehouse is not None,
            "runner": _gr.runner_provenance(ROOT, Path(__file__)),
            "request": {"as_of": args.as_of.isoformat(), "fields": list(args.field), "basis": args.basis,
                        "universe_id": args.universe_id, "feature_version": args.feature_version,
                        "unverified_vendor_shares": args.unverified_vendor_shares,
                        "session_lookback_days": args.session_lookback_days},
            "database_file_before": _gr.file_state(db_path),
        }
        # One deadline for the whole screen (import, open, owner bridge, values,
        # CSV): the screen is many statements and an interrupt landing between
        # two of them is a no-op, so the watchdog keeps interrupting until it stops.
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
                    attached = ("wh",) if research is None else ("wh", "rs")
                    receipt["effective_settings"] = _gr.verify_settings(
                        screen.con, limits, screen.spill_directory, read_only_databases=attached)
                    receipt["attachments_read_only"] = dict(screen.con.execute(
                        "SELECT database_name, readonly FROM duckdb_databases() WHERE database_name IN ('wh','rs')"
                    ).fetchall())
                    receipt["schema_version"] = _gr.schema_version(screen.con, "wh.main.schema_migrations")
                    result = run_cross_section(
                        screen, args.as_of, args.field, basis=args.basis, universe_id=args.universe_id,
                        feature_version=args.feature_version, max_rows=args.max_screen_rows,
                        frame_rows=limits.max_rows, session_lookback_days=args.session_lookback_days,
                        unverified_vendor_shares=args.unverified_vendor_shares)
                    if csv_path is not None:
                        def stream(sink: Any) -> None:
                            for index, chunk in enumerate(screen.iter_screen_frames()):
                                chunk.to_csv(sink, index=False, header=index == 0)

                        digest = _gr.publish_text(csv_path, stream, prefix=".pit-screen-", newline="")
                        receipt["csv"] = {"path": str(csv_path), "rows": result.total_rows, "sha256": digest}
                finally:
                    active.pop("con", None)
            if deadline_hit.is_set():
                raise TimeoutError(f"screen deadline of {limits.timeout_seconds} s exceeded")
            rows, size = _bounded_rows(result.rows, limits)
            receipt["screen"] = {
                "query_version": result.query_version, "status": result.status,
                "as_of_ts_utc": result.as_of_ts.isoformat(),
                "screen_date": None if result.screen_date is None else result.screen_date.isoformat(),
                "basis": result.basis, "universe_id": result.universe_id, "identity_basis": result.identity_basis,
                "fields": [item.field_id for item in result.fields], "members": result.members,
                "eligible_members": result.eligible_members, "valid_members": result.valid_members,
                "screen_rows": result.total_rows, "field_digests": result.field_digests,
                "r2a_field_digests": result.r2a_field_digests, "screen_sha256": result.screen_sha256,
                "blockers": list(result.blockers), "diagnostics": result.diagnostics,
            }
            receipt.update(rows=rows, returned_rows=len(rows), truncated=result.total_rows > len(rows),
                           transferred_bytes=size)
            receipt["status"] = "inspection_complete" if result.status == "complete" else result.status
        except Exception as exc:
            receipt.update(status="unavailable", error_type=type(exc).__name__, error=str(exc)[:6000],
                           deadline_exceeded=deadline_hit.is_set())
        finally:
            stop.set()
            guard_thread.join()
        receipt["elapsed_seconds"] = round(time.monotonic() - started, 3)
        receipt["database_file_after"] = _gr.file_state(db_path)
        receipt["database_file_changed"] = receipt["database_file_after"] != receipt["database_file_before"]
        _gr.write_receipt(output, receipt, prefix=".pit-screen-")
    except Exception as exc:
        # Refusals and argument failures occur before any database is opened.
        label = "refused" if isinstance(exc, Refused) else "unavailable"
        print(f"pit screen {label}: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2
    print(str(output))
    return 2 if receipt["status"] == "unavailable" else 0


if __name__ == "__main__":
    raise SystemExit(main())
