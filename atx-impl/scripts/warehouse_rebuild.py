from __future__ import annotations

import argparse
import json
from pathlib import Path

from db.connection import DEFAULT_DB_PATH, connect
from db.rebuild import run_warehouse_rebuild


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a deterministic gated warehouse rebuild.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--since")
    parser.add_argument("--until")
    parser.add_argument("--run-id")
    parser.add_argument("--rebuild-run-id")
    parser.add_argument("--git-sha")
    parser.add_argument("--no-gate", action="store_true")
    args = parser.parse_args()

    with connect(args.db_path, read_only=False) as store:
        result = run_warehouse_rebuild(
            store,
            since=args.since,
            until=args.until,
            rebuild_run_id=args.rebuild_run_id,
            orchestrator_run_id=args.run_id,
            git_sha=args.git_sha,
            gate=not args.no_gate,
        )
    print(
        json.dumps(
            {
                "rebuild_run_id": result.rebuild_run_id,
                "orchestrator_run_id": result.orchestrator_run_id,
                "status": result.status,
                "git_sha": result.git_sha,
                "dataset_counts": result.dataset_counts,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
