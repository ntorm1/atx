#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH
from atx_db.lake import DEFAULT_EXPORT_OBJECTS, DEFAULT_LAKE_ROOT, LakehouseExporter


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Export curated DuckDB warehouse objects to Parquet lake folders.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--lake-root", type=Path, default=DEFAULT_LAKE_ROOT)
    parser.add_argument("--objects", help="Comma-separated table/view list. Defaults to curated objects.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    objects = tuple(part.strip() for part in args.objects.split(",") if part.strip()) if args.objects else DEFAULT_EXPORT_OBJECTS
    results = LakehouseExporter(args.db_path, args.lake_root).export_objects(objects)
    print(
        json.dumps(
            [
                {
                    "export_run_id": result.export_run_id,
                    "object_name": result.object_name,
                    "rows": result.rows,
                    "output_path": str(result.output_path),
                    "manifest_path": str(result.manifest_path),
                    "byte_count": result.byte_count,
                    "sha256": result.sha256,
                }
                for result in results
            ],
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
