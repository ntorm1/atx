"""Archive the tier-1 v2 warehouse catalog tables to Parquet before the DuckDB file is retired (plan v3, D1/D7).

The five raw reload tables are not archived: they are reproducible from sources that stay on disk, whose SHA-256
the manifest records (``companyfacts.zip``, ``submissions.zip``, TickerHistory3). Everything else that holds rows
(seed catalogs, rule registries, identity evidence, derived-DSL definitions) is written one Parquet file per table.

Run under the memory guard::

    python scripts/archive_warehouse_v2.py [--db data/warehouse.duckdb] [--out data/archive/warehouse-v2]
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path

import duckdb

RAW_RELOAD_TABLES = {
    "fundamental_points": "data/cache/companyfacts.zip",
    "sec_company_facts": "data/cache/companyfacts.zip",
    "sec_submissions": "data/cache/submissions.zip",
    "equity_daily_bars": "TickerHistory3.parquet (sha256 0ed96b26...abbae)",
    "custom_features_daily": "derived from equity_daily_bars by custom_feature_definitions",
}


def _sha(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", type=Path, default=Path("data/warehouse.duckdb"))
    ap.add_argument("--out", type=Path, default=Path("data/archive/warehouse-v2"))
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    con = duckdb.connect(str(args.db), read_only=True, config={"memory_limit": "400MB", "threads": "1"})
    tables = con.execute("""SELECT schema_name, table_name, estimated_size FROM duckdb_tables()
                            ORDER BY table_name""").fetchall()
    files: dict[str, dict] = {}
    empty: list[str] = []
    for schema, name, est in tables:
        if name in RAW_RELOAD_TABLES:
            continue
        rows = con.execute(f'SELECT count(*) FROM "{schema}"."{name}"').fetchone()[0]
        if not rows:
            empty.append(name)
            continue
        dest = args.out / f"{name}.parquet"
        con.execute(f"""COPY (SELECT * FROM "{schema}"."{name}") TO '{dest.as_posix()}'
                        (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 32768)""")
        files[dest.name] = {"table": f"{schema}.{name}", "rows": rows, "bytes": dest.stat().st_size,
                            "sha256": _sha(dest)}
        print(name, rows, flush=True)
    ddl = con.execute("SELECT sql FROM duckdb_tables() WHERE sql IS NOT NULL ORDER BY table_name").fetchall()
    (args.out / "schema.sql").write_text(";\n".join(r[0] for r in ddl) + ";\n", encoding="utf-8")
    con.close()
    manifest = {
        "archived_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "source_db": str(args.db),
        "source_db_bytes": args.db.stat().st_size,
        "reason": "plan v3 D1/D7: lake is the system of record; warehouse retired, catalog tables kept",
        "raw_reload_tables_not_archived": RAW_RELOAD_TABLES,
        "empty_tables": empty,
        "files": files,
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    print(json.dumps({"tables": len(files), "empty": len(empty),
                      "bytes": sum(f["bytes"] for f in files.values())}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
