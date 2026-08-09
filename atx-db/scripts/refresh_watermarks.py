#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore, refresh_warehouse_watermarks


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Refresh dataset high-water marks from current warehouse tables.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with DuckDBStore(args.db_path) as store:
        result = refresh_warehouse_watermarks(store)
    print(
        json.dumps(
            {
                "db_path": str(args.db_path),
                "rows_upserted": result.rows_upserted,
                "watermarks": result.watermarks,
            },
            indent=2,
            default=str,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
