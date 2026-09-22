"""Execute a reviewed acceptance SELECT with bounded read-only output."""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path

import duckdb

parser = argparse.ArgumentParser()
parser.add_argument("--sql", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--max-rows", type=int, default=100)
args = parser.parse_args()
if args.output.exists():
    raise SystemExit("Refusing evidence overwrite")
if not 1 <= args.max_rows <= 1000:
    raise SystemExit("max-rows must be between1 and1000")
sql_bytes = args.sql.read_bytes()
result = {
    "observed_at_utc": dt.datetime.now(dt.UTC).isoformat(),
    "read_only": True,
    "sql_file": str(args.sql),
    "sql_sha256": hashlib.sha256(sql_bytes).hexdigest(),
    "duckdb_version": duckdb.__version__,
}
with duckdb.connect("C:/atx/atx-db/data/warehouse.duckdb", read_only=True,
                    config={"memory_limit": "1GB", "threads": "1", "preserve_insertion_order": "false"}) as con:
    con.execute("SET TimeZone='UTC'")
    con.execute("SET max_temp_directory_size='2GB'")
    cur = con.execute(sql_bytes.decode("utf-8-sig"))
    rows = cur.fetchmany(args.max_rows + 1)
    if len(rows) > args.max_rows:
        raise RuntimeError("Query output exceeded declared row budget")
    result["columns"] = [d[0] for d in cur.description]
    result["rows"] = rows
with args.output.open("x", encoding="utf-8") as handle:
    json.dump(result, handle, indent=2, default=str)
print(json.dumps({"output": str(args.output), "rows": len(rows),
                  "sql_sha256": result["sql_sha256"]}))
