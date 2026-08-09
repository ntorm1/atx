#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, run_governed_migrations


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Apply governed warehouse migrations.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--backup-dir", type=Path, default=None)
    parser.add_argument("--label", default="pre-migrate")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    result = run_governed_migrations(
        args.db_path,
        label=args.label,
        backup_dir=args.backup_dir,
    )
    backup = result.backup

    print(
        json.dumps(
            {
                "db_path": str(args.db_path),
                "run_id": result.run_id,
                "schema_version": max(result.versions_after) if result.versions_after else None,
                "migration_count": len(result.versions_after),
                "applied_versions": list(result.applied_versions),
                "backup_path": str(backup.backup_path) if backup is not None else None,
                "wal_backup_path": str(backup.wal_backup_path) if backup and backup.wal_backup_path else None,
                "backup_sha256": backup.sha256 if backup is not None else None,
                "backup_bytes": backup.byte_size if backup is not None else None,
                "checksum_verified": True,
                "schema_verified": True,
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
