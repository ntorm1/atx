#!/usr/bin/env python
"""Promote the fundamentals provider chain across the live warehouse universe.

Runs the governed post-migration ladder in dependency order, one stage at a
time, against a single-writer warehouse:

1. ``standardized``   - revision-complete standardized fundamentals build
2. ``reconciliation`` - indexed accounting-identity serving publish
3. ``submissions``    - bulk SEC submissions ingest from submissions.zip
4. ``queue``          - filing-context backfill queue rebuild
5. ``coverage``       - provider coverage refresh

Each stage prints one JSON line and stops the ladder on failure, so a rerun
with ``--start-stage`` resumes where the previous run stopped. Stages reuse
the exact CLI entry points; this script only sequences them.

Usage
-----
  python scripts/promote_fundamentals_universe.py --submissions-zip data\\cache\\submissions.zip
  python scripts/promote_fundamentals_universe.py --start-stage queue
  python scripts/promote_fundamentals_universe.py --skip-stage submissions
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, cli

STAGES = ("standardized", "reconciliation", "submissions", "queue", "coverage")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the fundamentals provider promotion ladder in order."
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--submissions-zip", type=Path, default=None)
    parser.add_argument("--memory-limit", default="8GB")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--run-id", default=None)
    parser.add_argument("--start-stage", choices=STAGES, default=STAGES[0])
    parser.add_argument(
        "--skip-stage",
        action="append",
        choices=STAGES,
        default=None,
        help="Repeatable; skipped stages are reported as skipped, not silent.",
    )
    return parser.parse_args()


def _stage_argv(stage: str, args: argparse.Namespace) -> list[str] | None:
    db = ["--db-path", str(args.db_path)]
    run_id = [] if args.run_id is None else ["--run-id", f"{args.run_id}-{stage}"]
    if stage == "standardized":
        return (
            ["refresh-standardized-fundamentals"]
            + db
            + ["--memory-limit", args.memory_limit, "--threads", str(args.threads)]
            + run_id
        )
    if stage == "reconciliation":
        return (
            ["refresh-fundamental-reconciliation"]
            + db
            + ["--memory-limit", args.memory_limit, "--threads", str(args.threads)]
            + run_id
        )
    if stage == "submissions":
        if args.submissions_zip is None:
            return None
        return (
            ["load-sec-submissions-bulk"]
            + db
            + ["--zip-path", str(args.submissions_zip)]
            + run_id
        )
    if stage == "queue":
        return ["refresh-filing-context-backfill-queue"] + db + run_id
    if stage == "coverage":
        return ["refresh-provider-coverage"] + db + run_id
    raise ValueError(stage)


def main() -> int:
    args = parse_args()
    skipped = set(args.skip_stage or ())
    started = False
    for stage in STAGES:
        if stage == args.start_stage:
            started = True
        if not started:
            continue
        if stage in skipped:
            print(json.dumps({"stage": stage, "status": "skipped"}))
            continue
        argv = _stage_argv(stage, args)
        if argv is None:
            print(
                json.dumps(
                    {
                        "stage": stage,
                        "status": "skipped",
                        "reason": "no --submissions-zip supplied",
                    }
                )
            )
            continue
        begun = time.monotonic()
        print(json.dumps({"stage": stage, "status": "running", "argv": argv}))
        code = cli.main(argv)
        elapsed = round(time.monotonic() - begun, 1)
        if code != 0:
            print(
                json.dumps(
                    {"stage": stage, "status": "failed", "exit_code": code, "seconds": elapsed}
                )
            )
            return code
        print(json.dumps({"stage": stage, "status": "completed", "seconds": elapsed}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
