"""B0 gate for migration 0327: read-only TickerHistory share-unit inventory (A9 I1).

Opens the warehouse READ-ONLY and reports, per TickerHistory ``run_id``, the unit evidence
and the action migration 0327 would take: ``scale_x1000``, ``none``, ``abort`` (0327 would
raise and roll back; a manual unit ruling is needed) or ``already_ledgered``. B0 records the
JSON in ``run5-readiness.json`` and applies 0327 only when no run is ``abort``.

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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db-path", required=True, type=Path)
    parser.add_argument("--output", type=Path, help="write the JSON here as well (must not exist)")
    parser.add_argument("--memory-limit", default="512MB")
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
        runs = ticker_history_unit_inventory(con)
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
                    for action in ("scale_x1000", "none", "abort", "already_ledgered")},
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
