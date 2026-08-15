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
import subprocess
from pathlib import Path
from typing import Any

import pyarrow.compute as pc
import pyarrow.parquet as pq


# Exact physical schema produced by oracle_ingest.py for the SpiderRock drop.
# Extra columns are permitted, but every field consumed by cohort selection,
# pricing, fitting, convention checks, and aggregate reporting is pinned here.
REQUIRED_SCHEMA = {
    "okey_at": "large_string",
    "okey_ts": "large_string",
    "okey_tk": "large_string",
    "okey_yr": "int64",
    "okey_mn": "int64",
    "okey_dy": "int64",
    "okey_xx": "double",
    "okey_cp": "large_string",
    "date": "large_string",
    "date_us": "int64",
    "tradingDate": "large_string",
    "tradingSession": "large_string",
    "undSecKey_at": "large_string",
    "undSecKey_ts": "large_string",
    "undSecKey_tk": "large_string",
    "undSecKey_yr": "int64",
    "undSecKey_mn": "int64",
    "undSecKey_dy": "int64",
    "undSecType": "large_string",
    "securityID": "large_string",
    "uBid": "double",
    "uAsk": "double",
    "uPrc": "double",
    "bidPrc": "double",
    "askPrc": "double",
    "bidSz": "int64",
    "askSz": "int64",
    "bidIV": "double",
    "askIV": "double",
    "srPrc": "double",
    "srVol": "double",
    "de": "double",
    "ga": "double",
    "th": "double",
    "ve": "double",
    "rh": "double",
    "ph": "double",
    "vo": "double",
    "va": "double",
    "deDecay": "double",
    "sdiv": "double",
    "ddiv": "double",
    "rate": "double",
    "years": "double",
    "error": "double",
    "prtVolume": "int64",
    "timestamp": "large_string",
    "timestamp_us": "int64",
    "date_cst": "large_string",
    "timestamp_cst": "large_string",
    "cumBidSize": "int64",
    "cumAskSize": "int64",
    "bidExch": "large_string",
    "askExch": "large_string",
    "bidMask": "int64",
    "askMask": "int64",
    "bidPrice2": "double",
    "askPrice2": "double",
    "cumBidSize2": "int64",
    "cumAskSize2": "int64",
    "atmVol": "double",
    "bucket": "timestamp[us]",
    "bucket_et": "large_string",
    "moneyness": "double",
}


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


def _cohort_underliers(repo_root: Path, commit: str) -> set[str] | None:
    underliers: set[str] = set()
    for name in ("smoke", "tune", "holdout"):
        path = f"atx-vol/bench/oracle/cohorts/{name}.json"
        try:
            raw = subprocess.run(
                ["git", "-C", str(repo_root), "show", f"{commit}:{path}"],
                check=True,
                capture_output=True,
                text=True,
                encoding="utf-8",
            ).stdout
            value = json.loads(raw)
        except (OSError, subprocess.SubprocessError, UnicodeError, json.JSONDecodeError):
            return None
        values = value.get("underliers") if isinstance(value, dict) else None
        if not isinstance(values, list) or not values or any(not isinstance(item, str) for item in values):
            return None
        underliers.update(values)
    return underliers


def _text_stat(value: Any) -> str | None:
    if isinstance(value, bytes):
        try:
            return value.decode("utf-8")
        except UnicodeDecodeError:
            return None
    return value if isinstance(value, str) else None


def _column_index(schema: Any, name: str) -> int:
    for index in range(len(schema)):
        if schema[index].name == name:
            return index
    return -1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-root", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--commit", required=True)
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
    wanted_underliers = _cohort_underliers(args.repo_root, args.commit)
    if not wanted_underliers:
        return _fail("cohort_underliers")

    date_root = args.data_root / f"date={trading_date}"
    if not date_root.is_dir():
        return _fail("store_date")
    actual_dirs = {path.name.split("=", 1)[1]: path for path in date_root.glob("bucket_et=*") if path.is_dir()}
    if set(actual_dirs) != set(expected):
        return _fail("store_buckets")

    actual: dict[str, int] = {}
    schema_fingerprints: set[str] = set()
    found_underliers: set[str] = set()
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
                parquet_file = pq.ParquetFile(path)
                metadata = parquet_file.metadata
                if metadata is None or metadata.num_rows <= 0 or metadata.num_columns <= 0:
                    return _fail("store_metadata")
                arrow_schema = parquet_file.schema_arrow.remove_metadata()
                actual_schema = {field.name: str(field.type) for field in arrow_schema}
                if any(actual_schema.get(name) != expected_type for name, expected_type in REQUIRED_SCHEMA.items()):
                    return _fail("store_schema")
                rows += metadata.num_rows
                parquet_files += 1
                required_text = "\n".join(f"{name}:{REQUIRED_SCHEMA[name]}" for name in sorted(REQUIRED_SCHEMA))
                schema_fingerprints.add(hashlib.sha256(required_text.encode("utf-8")).hexdigest())

                parquet_schema = metadata.schema
                bucket_index = _column_index(parquet_schema, "bucket_et")
                date_index = _column_index(parquet_schema, "tradingDate")
                underlier_index = _column_index(parquet_schema, "undSecKey_tk")
                if min(bucket_index, date_index, underlier_index) < 0:
                    return _fail("store_schema")
                for row_group_index in range(metadata.num_row_groups):
                    row_group = metadata.row_group(row_group_index)
                    bucket_stats = row_group.column(bucket_index).statistics
                    date_stats = row_group.column(date_index).statistics
                    if (
                        bucket_stats is None
                        or not bucket_stats.has_min_max
                        or _text_stat(bucket_stats.min) != bucket
                        or _text_stat(bucket_stats.max) != bucket
                        or date_stats is None
                        or not date_stats.has_min_max
                        or _text_stat(date_stats.min) != trading_date
                        or _text_stat(date_stats.max) != trading_date
                    ):
                        return _fail("store_partition_stats")
                    missing = wanted_underliers - found_underliers
                    if not missing:
                        continue
                    underlier_stats = row_group.column(underlier_index).statistics
                    candidates = missing
                    if underlier_stats is not None and underlier_stats.has_min_max:
                        lower = _text_stat(underlier_stats.min)
                        upper = _text_stat(underlier_stats.max)
                        if lower is not None and upper is not None:
                            candidates = {item for item in missing if lower <= item <= upper}
                    if candidates:
                        values = parquet_file.read_row_group(row_group_index, columns=["undSecKey_tk"]).column(0)
                        for chunk in values.chunks:
                            found_underliers.update(item for item in pc.unique(chunk).to_pylist() if item in candidates)
            actual[bucket] = rows
    except (OSError, ValueError, TypeError):
        return _fail("store_metadata")

    if actual != expected or sum(actual.values()) != total_rows:
        return _fail("store_counts")
    if len(schema_fingerprints) != 1:
        return _fail("store_schema")
    if found_underliers != wanted_underliers:
        return _fail("cohort_underliers")

    manifest_sha256 = hashlib.sha256(args.manifest.read_bytes()).hexdigest()
    result = {
        "schema_version": 1,
        "status": "PASS",
        "manifest_sha256": manifest_sha256,
        "total_rows": total_rows,
        "bucket_count": len(actual),
        "parquet_files": parquet_files,
        "schema_sha256": next(iter(schema_fingerprints)),
        "cohort_underlier_count": len(wanted_underliers),
    }
    print(json.dumps(result, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
