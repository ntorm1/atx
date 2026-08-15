#!/usr/bin/env python3
"""Validate an oracle Parquet store from metadata only.

This command never reads column values.  It compares Parquet footer row counts
and schema fingerprints with the aggregate ingest manifest and emits only an
aggregate JSON receipt.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

import pyarrow.parquet as pq


def _fail(reason: str) -> int:
    print(json.dumps({"schema_version": 1, "status": "INGEST_REQUIRED", "reason": reason}, separators=(",", ":")))
    return 0


def _manifest(path: Path) -> dict[str, Any] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    expected = {"trading_date", "source_tsv_bytes", "total_rows", "buckets", "top_underliers_by_rows", "ingested_at"}
    return value if isinstance(value, dict) and set(value) == expected else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-root", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    args = parser.parse_args()

    manifest = _manifest(args.manifest)
    if manifest is None:
        return _fail("manifest_schema")
    trading_date = manifest.get("trading_date")
    buckets = manifest.get("buckets")
    if not isinstance(trading_date, str) or not isinstance(buckets, dict) or not buckets:
        return _fail("manifest_values")
    try:
        expected = {str(name): int(count) for name, count in buckets.items()}
        total_rows = int(manifest["total_rows"])
    except (TypeError, ValueError):
        return _fail("manifest_counts")
    if any(count <= 0 for count in expected.values()) or sum(expected.values()) != total_rows:
        return _fail("manifest_counts")

    date_root = args.data_root / f"date={trading_date}"
    if not date_root.is_dir():
        return _fail("store_date")
    actual_dirs = {path.name.split("=", 1)[1]: path for path in date_root.glob("bucket_et=*") if path.is_dir()}
    if set(actual_dirs) != set(expected):
        return _fail("store_buckets")

    actual: dict[str, int] = {}
    schema_fingerprints: set[str] = set()
    parquet_files = 0
    try:
        for bucket, directory in actual_dirs.items():
            files = sorted(directory.rglob("*.parquet"))
            if not files:
                return _fail("store_files")
            rows = 0
            for path in files:
                if path.stat().st_size <= 0:
                    return _fail("store_files")
                metadata = pq.ParquetFile(path).metadata
                if metadata is None or metadata.num_rows <= 0 or metadata.num_columns <= 0:
                    return _fail("store_metadata")
                rows += metadata.num_rows
                parquet_files += 1
                arrow_schema = metadata.schema.to_arrow_schema().remove_metadata()
                schema_fingerprints.add(hashlib.sha256(str(arrow_schema).encode("utf-8")).hexdigest())
            actual[bucket] = rows
    except (OSError, ValueError, TypeError):
        return _fail("store_metadata")

    if actual != expected or sum(actual.values()) != total_rows:
        return _fail("store_counts")
    if len(schema_fingerprints) != 1:
        return _fail("store_schema")

    manifest_sha256 = hashlib.sha256(args.manifest.read_bytes()).hexdigest()
    result = {
        "schema_version": 1,
        "status": "PASS",
        "manifest_sha256": manifest_sha256,
        "total_rows": total_rows,
        "bucket_count": len(actual),
        "parquet_files": parquet_files,
        "schema_sha256": next(iter(schema_fingerprints)),
    }
    print(json.dumps(result, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
