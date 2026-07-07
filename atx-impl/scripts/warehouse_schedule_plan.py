"""PF4-S2 S2-3: read-only dry-run maintenance-cadence planner.

Reads ``maintenance_schedule`` (migration 0183) and PLANS -- never executes --
the operator archive: for each enabled dataset it composes the
``run_backfill``/``run_maintenance`` mode the ``DatasetOrchestrator`` *would*
run, based on the schedule's declared backfill-window shape. This script
performs zero writes and opens the live DB ``read_only=True``; the live
archive itself remains an operator-run step outside this sprint's scope.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from db.connection import DEFAULT_DB_PATH, connect


def plan_maintenance(store) -> list[dict]:
    """Return the dry-run maintenance plan; performs no writes."""
    rows = store.con.execute(
        "SELECT dataset_id, cadence, cadence_trigger, backfill_window_json, chunk, enabled "
        "FROM maintenance_schedule WHERE enabled ORDER BY dataset_id"
    ).fetchall()
    plan = []
    for dataset_id, cadence, trigger, window_json, chunk, enabled in rows:
        window = json.loads(window_json or "{}")
        mode = "backfill" if window.get("shape") in ("windowed", "rebuild") else "maintenance"
        plan.append({
            "dataset_id": str(dataset_id), "cadence": str(cadence),
            "trigger": None if trigger is None else str(trigger),
            "planned_mode": mode, "chunk": None if chunk is None else str(chunk),
            "backfill_window": window, "executes": False, "dry_run": True,
        })
    return plan


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Plan (dry-run) the warehouse maintenance archive.")
    parser.add_argument("--db-path", default=str(DEFAULT_DB_PATH))
    args = parser.parse_args(argv)
    with connect(Path(args.db_path), read_only=True) as store:  # read-only: never mutates live data
        plan = plan_maintenance(store)
    sys.stdout.write(json.dumps({"plan": plan, "executed": False}, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
