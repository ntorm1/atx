"""OPS-only bounded, read-only identity input exports (tier-1 v2 node 3.3).

Run from a committed export under the memory guard with --heavy. Each invocation
exports one table/hash slice; missing tables receive an explicit receipt. Never
copies the warehouse or selects internal CUSIP. The manifest contains exact output
bytes, sha256, rows and schema and records the source file's stat and DuckDB version.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path

TABLE_KEYS = {
    "security_identity_evidence": ("evidence_id", "source_loaded_at"),
    "security_identifier_history": ("security_id", "id_type", "valid_from", "available_at"),
    "securities": ("security_id",),
    "exchange_listings": ("security_id", "valid_from", "available_at"),
    "sec_company_tickers": ("cik", "ticker"),
    "nasdaq_symbol_directory": ("as_of_date", "symbol"),
}
MAX_ROWS = 2_000_000


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def atomic_json(path: Path, value: object) -> None:
    temp = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    temp.write_text(json.dumps(value, sort_keys=True, indent=2, default=str) + "\n", encoding="utf-8")
    os.replace(temp, path)


def quoted(name: str) -> str:
    return '"' + name.replace('"', '""') + '"'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--table", required=True, choices=tuple(TABLE_KEYS))
    parser.add_argument("--slice", type=int, default=0, dest="slice_id")
    parser.add_argument("--slices", type=int, default=1)
    parser.add_argument("--inspect", action="store_true", help="counts/schema only, no output files")
    args = parser.parse_args()
    if not 1 <= args.slices <= 4096 or not 0 <= args.slice_id < args.slices:
        parser.error("require 0 <= --slice < --slices <= 4096")
    import duckdb
    import pyarrow.parquet as pq

    source = args.db.resolve(strict=True)
    before = source.stat()
    con = duckdb.connect(str(source), read_only=True,
                         config={"memory_limit": "256MB", "threads": "1",
                                 "preserve_insertion_order": "false"})
    try:
        con.execute("SET TimeZone='UTC'")
        columns = con.execute("SELECT column_name,data_type,is_nullable FROM information_schema.columns "
                              "WHERE table_catalog=current_database() AND table_schema='main' AND table_name=? "
                              "ORDER BY ordinal_position", [args.table]).fetchall()
        # Projection is explicit: exclusion works even when old schemas lack the internal field.
        columns = [row for row in columns if str(row[0]).lower() != "internal_cusip"]
        entry = {"table": args.table, "slice": args.slice_id, "slices": args.slices,
                 "duckdb": duckdb.__version__, "source": {"path": str(source), "bytes": before.st_size,
                 "mtime_ns": before.st_mtime_ns}, "schema": [list(row) for row in columns],
                 "read_only": True, "memory_limit": "256MB", "threads": 1,
                 "exporter_sha256": sha256(Path(__file__)), "status": "missing"}
        if columns:
            names = {str(row[0]) for row in columns}
            keys = [key for key in TABLE_KEYS[args.table] if key in names]
            if not keys:
                raise RuntimeError(f"{args.table}: none of the deterministic slice keys exists")
            key_sql = ", ".join(quoted(key) for key in keys)
            scope = "id_type IN ('CIK','LEI','FIGI')" if args.table == "security_identifier_history" else "true"
            predicate = f"({scope}) AND hash({key_sql}) % {args.slices} = {args.slice_id}"
            table = quoted(args.table)
            rows = int(con.execute(f"SELECT count(*) FROM {table} WHERE {predicate}").fetchone()[0])
            total = int(con.execute(f"SELECT count(*) FROM {table}").fetchone()[0])
            scope_rows = int(con.execute(f"SELECT count(*) FROM {table} WHERE {scope}").fetchone()[0])
            query = f"SELECT {', '.join(quoted(str(c[0])) for c in columns)} FROM {table} WHERE {predicate} ORDER BY {key_sql}"
            entry.update(status="empty" if rows == 0 else "present", rows=rows, table_rows=total,
                         scope_rows=scope_rows, query=query, slice_keys=keys)
            if rows > MAX_ROWS:
                raise RuntimeError(f"{args.table} slice has {rows:,} rows > {MAX_ROWS:,}; increase --slices")
        else:
            entry.update(rows=0, table_rows=0, scope_rows=0)
        if args.inspect:
            print(json.dumps(entry, sort_keys=True, default=str))
            return 0
        args.out.mkdir(parents=True, exist_ok=True)
        stem = f"{args.table}.slice-{args.slice_id:04d}-of-{args.slices:04d}"
        receipt = args.out / f"{stem}.json"
        if receipt.exists():
            prior = json.loads(receipt.read_text(encoding="utf-8"))
            if any(prior.get(k) != v for k, v in entry.items()):
                raise RuntimeError(f"input or schema changed for {receipt}; choose a new output build directory")
            if prior.get("file") and sha256(args.out / prior["file"]) != prior["sha256"]:
                raise RuntimeError(f"output hash mismatch for {receipt}")
            print(json.dumps({"reused": True, **prior}, sort_keys=True, default=str))
            return 0
        if columns:
            target = args.out / f"{stem}.parquet"
            if target.exists():
                raise RuntimeError(f"unreceipted output exists: {target}; inspect it before resuming")
            temp = args.out / f".{stem}.{os.getpid()}.parquet.tmp"
            try:
                escaped = str(temp.resolve()).replace("'", "''")
                con.execute(f"COPY ({query}) TO '{escaped}' (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 65536)")
                metadata = pq.read_metadata(temp)
                if metadata.num_rows != entry["rows"]:
                    raise RuntimeError("Parquet row count differs from source slice")
                entry.update(file=target.name, sha256=sha256(temp), bytes=temp.stat().st_size,
                             arrow_schema=str(pq.read_schema(temp)))
                after = source.stat()
                if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
                    raise RuntimeError("warehouse changed during export; retry from a stable read window")
                os.replace(temp, target)
            finally:
                temp.unlink(missing_ok=True)
        entry["exported_at"] = dt.datetime.now(dt.UTC).isoformat()
        atomic_json(receipt, entry)
        # One OPS writer, one table/slice invocation. Other owners do not write this directory.
        entries = [json.loads(path.read_text(encoding="utf-8")) for path in sorted(args.out.glob("*.slice-*.json"))]
        atomic_json(args.out / "manifest.json", {"schema": "identity-input-export-v1", "read_only": True,
                    "source": entry["source"], "exports": entries})
        print(json.dumps(entry, sort_keys=True, default=str))
        return 0
    finally:
        con.close()


if __name__ == "__main__":
    raise SystemExit(main())
