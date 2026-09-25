"""Bounded inspection of the five desk SQL contracts; never production certification.

Only schema copies / small fixture databases are permitted in this L1 runner.
The optional checkout-root warehouse_template.duckdb is a stale, untracked
schema (0271); bind against a copy of the current test-harness template under
.pytest_cache/db_schema_templates/<fingerprint>/ instead.
Example (PowerShell, from atx-db):
  Copy-Item .pytest_cache/db_schema_templates/<fingerprint>/warehouse_template.duckdb "$env:TEMP/l1-schema.duckdb"
  .venv/Scripts/python.exe scripts/read_desk_question_pack.py `
    --db-path "$env:TEMP/l1-schema.duckdb" --mode explain `
    --output-json "$env:TEMP/l1-bind.json"
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import os
import re
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SQL_FILES = {
    "q1": "desk-q1-cvx-eps.sql",
    "q2": "desk-q2-acceleration.sql",
    "q3": "desk-q3-valuation-profitability.sql",
    "q4": "desk-q4-accrual-leverage.sql",
    "q5": "desk-q5-monthly-factor-portfolios.sql",
}
MAX_RECEIPT_BYTES = 10_000_000


def utc_cutoff(text: str) -> dt.datetime:
    value = dt.datetime.fromisoformat(text.replace("Z", "+00:00"))
    if value.tzinfo is None or value.utcoffset() is None:
        raise argparse.ArgumentTypeError("cutoff requires an explicit UTC offset")
    return value.astimezone(dt.UTC).replace(tzinfo=None)


def memory_limit(text: str) -> str:
    match = re.fullmatch(r"([0-9]+)MB", text.upper())
    if match is None or not 32 <= int(match[1]) <= 512:
        raise argparse.ArgumentTypeError("memory-limit must be 32MB..512MB")
    return text.upper()


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--db-path", type=Path, required=True)
    p.add_argument("--output-json", type=Path, required=True)
    p.add_argument("--query", choices=("all", *SQL_FILES), default="all")
    p.add_argument("--mode", choices=("explain", "run"), default="explain")
    p.add_argument("--memory-limit", type=memory_limit, default="256MB")
    p.add_argument("--threads", type=int, choices=(1, 2), default=1)
    p.add_argument("--max-rows", type=int, default=1000)
    p.add_argument("--max-result-bytes", type=int, default=1_000_000)
    p.add_argument("--timeout-seconds", type=int, default=30)
    p.add_argument("--cutoff", type=utc_cutoff, default=utc_cutoff("2026-09-20T22:00:00Z"))
    p.add_argument("--cik", default="0000093410")
    p.add_argument("--market-cap-floor", type=float, default=1_000_000_000)
    p.add_argument("--build-run-id", default="fundamental_signals_build1")
    p.add_argument("--evaluation-run-id", default="fundamental_signals_evaluation1")
    p.add_argument("--signal-id", default="eps_growth_yoy")
    p.add_argument("--start-date", type=dt.date.fromisoformat, default=dt.date(2015, 1, 1))
    p.add_argument("--end-date", type=dt.date.fromisoformat, default=dt.date(2026, 12, 31))
    return p


def validate_paths(db_path: Path, output: Path) -> tuple[Path, Path]:
    db_path, output = db_path.resolve(), output.resolve()
    # Resolve links and reject production backups too, before any DuckDB call.
    if db_path.is_relative_to((ROOT / "data").resolve()):
        raise ValueError("L1 forbids databases under atx-db/data; use a schema copy or tiny fixture")
    for protected in (ROOT / "data/warehouse.duckdb", ROOT / "warehouse_template.duckdb"):
        if db_path == protected.resolve() or (
            db_path.exists() and protected.exists() and db_path.samefile(protected)
        ):
            raise ValueError("L1 requires a copy; live warehouse and original template are forbidden")
    if not db_path.is_file():
        raise ValueError("db-path must be an existing schema copy or tiny fixture")
    if output == db_path or output.exists() or output.suffix.lower() != ".json" or not output.parent.is_dir():
        raise ValueError("output-json must be a fresh .json path in an existing directory")
    return db_path, output


def open_read_only(db_path: Path, memory: str, threads: int):
    import duckdb

    return duckdb.connect(str(db_path), read_only=True, config={
        "memory_limit": memory, "threads": str(threads), "max_temp_directory_size": "0B",
        "preserve_insertion_order": "false", "enable_external_access": "false",
    })


def inspect_query(con, name: str, parameters: dict, args) -> dict:
    import duckdb

    sql_path = ROOT / "sql/research" / SQL_FILES[name]
    sql = sql_path.read_text(encoding="utf-8").strip().removesuffix(";")
    bound = {key: parameters[key] for key in set(re.findall(r"\$([a-z_]+)", sql))}
    result = {"query": name, "sql_path": str(sql_path),
              "sql_sha256": hashlib.sha256(sql.encode()).hexdigest(), "parameters": bound,
              "production_qualified": False}
    timer = threading.Timer(args.timeout_seconds, con.interrupt)
    timer.daemon = True
    con.execute("BEGIN TRANSACTION")
    timer.start()
    try:
        plan = con.execute("EXPLAIN " + sql, bound).fetchone()
        result["plan_sha256"] = hashlib.sha256(str(plan).encode()).hexdigest()
        result["bind_status"] = "bound"
        if args.mode == "run":
            bounded = f"SELECT CAST(to_json(t) AS VARCHAR) AS payload FROM ({sql}) t LIMIT {args.max_rows + 1}"
            rows, size = con.execute(
                "SELECT count(*),coalesce(sum(octet_length(encode(payload))),0) "
                f"FROM ({bounded})", bound,
            ).fetchone()
            if size > args.max_result_bytes:
                raise ValueError(f"bounded payload exceeds byte cap: {size}")
            fetched = con.execute(bounded, bound).fetchmany(args.max_rows + 1)
            result.update(rows=[json.loads(row[0]) for row in fetched[:args.max_rows]],
                          returned_rows=min(rows, args.max_rows), truncated=rows > args.max_rows,
                          transferred_bytes_including_sentinel=size)
            result["status"] = "empty" if not rows else "inspection_only"
        else:
            result["status"] = "bound_only"
        con.execute("COMMIT")
    except (duckdb.Error, ValueError) as exc:
        con.execute("ROLLBACK")
        result.update(status="unavailable", error_type=type(exc).__name__, error=str(exc)[:6000])
    finally:
        timer.cancel()
        timer.join()
    return result


def write_receipt(output: Path, receipt: dict) -> None:
    encoded = json.dumps(receipt, default=str, sort_keys=True, indent=2, allow_nan=False) + "\n"
    if len(encoded.encode("utf-8")) > MAX_RECEIPT_BYTES:
        raise ValueError("receipt exceeds 10MB cap")
    fd, temporary = tempfile.mkstemp(prefix=".desk-pack-", suffix=".json", dir=output.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as sink:
            sink.write(encoded)
            sink.flush()
            os.fsync(sink.fileno())
        os.link(temporary, output)
    finally:
        os.unlink(temporary)


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        db_path, output = validate_paths(args.db_path, args.output_json)
        if not 1 <= args.max_rows <= 1000 or not 1024 <= args.max_result_bytes <= 1_000_000:
            raise ValueError("max-rows must be 1..1000 and max-result-bytes 1024..1000000")
        if not 1 <= args.timeout_seconds <= 60:
            raise ValueError("timeout-seconds must be 1..60 per query")
        if not args.cik.isascii() or not args.cik.isdecimal() or not 1 <= len(args.cik) <= 10 or int(args.cik) == 0:
            raise ValueError("cik requires 1..10 digits, nonzero")
        if not math.isfinite(args.market_cap_floor) or args.market_cap_floor <= 0:
            raise ValueError("market-cap-floor must be finite and positive")
        if not dt.date(2015, 1, 1) <= args.start_date <= args.end_date <= dt.date(2026, 12, 31):
            raise ValueError("date range must lie within 2015..2026")
        parameters = {key: getattr(args, key) for key in (
            "cutoff", "market_cap_floor", "build_run_id", "evaluation_run_id", "signal_id", "start_date", "end_date",
        )}
        parameters["cik"] = args.cik.zfill(10)
        receipt = {"contract": "desk-question-pack-l1-v1", "database": str(db_path),
                   "read_only": True, "mode": args.mode, "production_qualified": False,
                   "limits": {key: getattr(args, key) for key in (
                       "memory_limit", "threads", "max_rows", "max_result_bytes", "timeout_seconds",
                   )}, "queries": []}
        try:
            with open_read_only(db_path, args.memory_limit, args.threads) as con:
                con.execute("SET TimeZone='UTC'")
                for name in SQL_FILES if args.query == "all" else (args.query,):
                    receipt["queries"].append(inspect_query(con, name, parameters, args))
            receipt["status"] = "unavailable" if any(
                r["status"] == "unavailable" for r in receipt["queries"]
            ) else "inspection_complete"
        except Exception as exc:
            receipt.update(status="unavailable", error_type=type(exc).__name__, error=str(exc)[:6000])
        write_receipt(output, receipt)
    except Exception as exc:
        # Connection/argument failures occur before any query result exists.
        print(f"desk pack unavailable: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2
    print(str(output))
    return 2 if receipt["status"] == "unavailable" else 0


if __name__ == "__main__":
    raise SystemExit(main())
