"""B0 gate for migration 0327: read-only TickerHistory share-unit inventory (A9 I1).

Opens the warehouse READ-ONLY and reports, per TickerHistory ``run_id``, the unit evidence
and the action migration 0327 would take: ``scale_x1000``, ``none`` or ``abort``.
Runs proven corrected by a published bars_unit_correction ledger are explicitly
``already_corrected``; their original loader metadata no longer describes stored units.
The raw inventory and whole-line suspect counts remain in the report.

    OPENBLAS_NUM_THREADS=1 .venv/Scripts/python.exe scripts/ticker_history_unit_inventory.py \
        --db-path <warehouse.duckdb> --output <readiness-fragment.json>

Exit code 0 = every run decided, 3 = at least one run would abort 0327.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import time
from pathlib import Path

import duckdb

from atx_db.migrations.bodies_0327 import ticker_history_unit_inventory


def inventory_with_corrections(con):
    """Keep raw evidence while avoiding a false mixed-unit abort after publication."""
    runs = ticker_history_unit_inventory(con)
    tables = {row[0] for row in con.execute("SELECT table_name FROM duckdb_tables() WHERE schema_name='main'").fetchall()}
    corrected = set()
    if {"equity_bar_unit_corrections", "build_runs"} <= tables:
        corrected = {row[0] for row in con.execute("""
            SELECT DISTINCT json_extract_string(c.evidence, '$.run_id')
            FROM equity_bar_unit_corrections c JOIN build_runs b
              ON b.stage = 'bars_unit_correction'
             AND b.run_key = json_extract_string(c.evidence, '$.stage_run_key')
             AND b.status = 'published'
            WHERE c.unit_basis IN ('thousands', 'shares_unit_suspect')
        """).fetchall()}
    for run in runs:
        if run["run_id"] in corrected:
            run["raw_inventory_action"] = run["action"]
            run["raw_inventory_reason"] = run["reason"]
            run["action"] = "already_corrected"
            run["reason"] = "published bars_unit_correction ledger; raw loader-unit inference no longer applies"
    return runs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db-path", required=True, type=Path)
    parser.add_argument("--output", type=Path, help="write the JSON here as well (must not exist)")
    parser.add_argument("--memory-limit", default="256MB")
    parser.add_argument("--threads", type=int, default=1)
    args = parser.parse_args()
    if args.output is not None and args.output.exists():
        raise FileExistsError(args.output)
    started = time.perf_counter()
    con = duckdb.connect(
        str(args.db_path), read_only=True,
        config={"memory_limit": args.memory_limit, "threads": args.threads},
    )
    try:
        version = con.execute(
            "SELECT max(CAST(version AS INTEGER)) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
        ).fetchone()
        runs = inventory_with_corrections(con)
    finally:
        con.close()
    report = {
        "check": "ticker_history_share_unit_inventory",
        "migration": "0327",
        "db_path": str(args.db_path.resolve()),
        "read_only": True,
        "schema_version": None if version is None else version[0],
        "generated_at": dt.datetime.now(dt.UTC).isoformat(),
        "elapsed_seconds": round(time.perf_counter() - started, 1),
        "would_abort": any(run["action"] == "abort" for run in runs),
        "actions": {action: sum(run["action"] == action for run in runs)
                    for action in ("scale_x1000", "none", "abort", "already_corrected")},
        "pending_suspect_line_count": sum(run["shares_unit_suspect_line_count"] for run in runs
                                          if run["action"] != "already_corrected"),
        "pending_suspect_rows": sum(run["shares_unit_suspect_rows"] for run in runs
                                    if run["action"] != "already_corrected"),
        "runs": runs,
    }
    text = json.dumps(report, indent=2, default=str)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x", encoding="utf-8") as handle:
            handle.write(text + "\n")
    print(text)
    return 3 if report["would_abort"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
