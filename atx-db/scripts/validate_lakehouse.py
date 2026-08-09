#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, validate_lake_export


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate audited Parquet lake exports.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--export-run-id", help="Defaults to the latest succeeded export run.")
    parser.add_argument("--allow-failures", action="store_true", help="Always exit 0 and report problems in JSON.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    summary = validate_lake_export(args.db_path, export_run_id=args.export_run_id)
    payload = asdict(summary)
    payload["db_path"] = str(args.db_path)
    payload["problem_count"] = len(summary.problems)
    print(json.dumps(payload, indent=2, default=str))
    return 0 if args.allow_failures or not summary.problems else 1


if __name__ == "__main__":
    raise SystemExit(main())
