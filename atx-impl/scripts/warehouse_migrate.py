#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore, verify_migration_checksums


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Apply governed warehouse migrations.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with DuckDBStore(args.db_path) as store:
        verify_migration_checksums(store.con)
        versions = [
            row[0]
            for row in store.con.execute(
                """
                SELECT CAST(version AS INTEGER)
                FROM schema_migrations
                WHERE version ~ '^[0-9]+$'
                ORDER BY CAST(version AS INTEGER)
                """
            ).fetchall()
        ]

    print(
        json.dumps(
            {
                "db_path": str(args.db_path),
                "schema_version": max(versions) if versions else None,
                "migration_count": len(versions),
                "checksum_verified": True,
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
