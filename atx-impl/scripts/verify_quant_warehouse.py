#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore
from db.quality import run_warehouse_quality_checks


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run and record quant warehouse SQL quality checks.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--daily-macro-stale-days", type=int, default=10)
    parser.add_argument("--monthly-macro-stale-days", type=int, default=70)
    parser.add_argument("--no-record", action="store_true", help="Run checks without appending data_quality_checks rows.")
    parser.add_argument("--allow-failures", action="store_true", help="Always exit 0 and report failures in JSON.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with DuckDBStore(args.db_path) as store:
        results = run_warehouse_quality_checks(
            store,
            daily_macro_stale_days=args.daily_macro_stale_days,
            monthly_macro_stale_days=args.monthly_macro_stale_days,
            record=not args.no_record,
        )
    failed = [result for result in results if result.status == "failed"]
    warnings = [result for result in results if result.status == "warning"]
    print(
        json.dumps(
            {
                "db_path": str(args.db_path),
                "passed": len([result for result in results if result.status == "passed"]),
                "warnings": len(warnings),
                "failed": len(failed),
                "results": [asdict(result) for result in results],
            },
            indent=2,
            default=str,
        )
    )
    return 0 if args.allow_failures or not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
