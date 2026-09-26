#!/usr/bin/env python
"""Build P8 corporate-action events in a scratch DuckDB (bounded), outside the warehouse.

Reads loader-shaped bars either from the retained TickerHistory3 parquet (materialized once into
``--work-db`` as ``equity_daily_bars``: adjusted_close = close x cumulReturnFactor, vendor shares
x1000, a 22:00 bar clock, one source and run) or from another DuckDB's ``equity_daily_bars``
(ATTACHed read-only). Writes ``corporate_action_events`` (the ``corporate_actions`` columns) into
``--work-db`` and prints the counts by type and reason plus the per-line factor tie-out as JSON.
The production write is the ``corporate_actions`` dataset job; this script never opens the
warehouse.

Usage
-----
  python scripts/build_corporate_action_events.py --work-db SCRATCH.duckdb --parquet TickerHistory3.parquet
  python scripts/build_corporate_action_events.py --work-db SCRATCH.duckdb --bars-db BARS.duckdb
         [--memory-limit 320MB] [--threads 2] [--summary OUT.json]
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import duckdb

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db import DEFAULT_DB_PATH
from atx_db.corporate_actions import build_corporate_action_events

TABLE = "corporate_action_events"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build P8 corporate-action events in a scratch DuckDB.")
    parser.add_argument("--work-db", type=Path, required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--parquet", type=Path, help="Retained TickerHistory3.parquet (materialized once).")
    source.add_argument("--bars-db", type=Path, help="A DuckDB holding equity_daily_bars (read-only).")
    # 320MB keeps the full retained build under 0.6 GiB (measured: 607 MB private at 320MB, 670 MB at 384MB).
    parser.add_argument("--memory-limit", default="320MB")
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--summary", type=Path)
    return parser.parse_args()


def _refuse_warehouse(*paths: Path | None) -> None:
    warehouse = Path(DEFAULT_DB_PATH).resolve()
    for path in paths:
        if path is not None and path.resolve() == warehouse:
            raise SystemExit(f"refusing to open the warehouse {warehouse}: use the corporate_actions dataset job")


def main() -> int:
    args = parse_args()
    _refuse_warehouse(args.work_db, args.bars_db)
    con = duckdb.connect(str(args.work_db))
    con.execute(f"SET memory_limit='{args.memory_limit}'")
    con.execute(f"SET threads={int(args.threads)}")
    con.execute("SET preserve_insertion_order=false")
    con.execute(f"SET temp_directory='{args.work_db}.tmp'")
    started = time.perf_counter()
    if args.bars_db is not None:
        con.execute(f"ATTACH '{args.bars_db}' AS bars_src (READ_ONLY)")
        con.execute("CREATE OR REPLACE VIEW equity_daily_bars AS SELECT * FROM bars_src.equity_daily_bars")
    elif not con.execute("SELECT count(*) FROM duckdb_tables() WHERE table_name = 'equity_daily_bars'").fetchone()[0]:
        con.execute(f"""
            CREATE TABLE equity_daily_bars AS
            SELECT 'tickerhistory3_retained' AS source, CAST(securityID AS VARCHAR) AS security_id,
                   coalesce(ticker_tk, todayTicker) AS symbol, tradingDate AS trade_date,
                   CAST(close AS DOUBLE) AS close,
                   CASE WHEN close > 0 AND cumulReturnFactor > 0 AND isfinite(cumulReturnFactor)
                        THEN CAST(close AS DOUBLE) * cumulReturnFactor END AS adjusted_close,
                   CASE WHEN shares > 0 THEN shares * 1000 END AS shares_outstanding,
                   tradingDate::TIMESTAMP + INTERVAL 22 HOUR AS available_at, 'retained-parquet' AS run_id
            FROM read_parquet('{args.parquet}')
            WHERE securityID > 0 AND tradingDate IS NOT NULL AND close > 0
        """)
    loaded = time.perf_counter()
    con.execute(f"DROP TABLE IF EXISTS {TABLE}")
    summary = build_corporate_action_events(con, TABLE, run_id="scratch-build", kind="")
    summary["bars"] = con.execute("SELECT count(*) FROM equity_daily_bars").fetchone()[0]
    # Latest revision per step, then the superseded (earlier-clock) revisions.
    for key, latest in (("by_reason", "is_latest_revision"), ("superseded_by_reason", "NOT is_latest_revision")):
        summary[key] = [list(row) for row in con.execute(f"""
            SELECT action_type, coalesce(json_extract_string(details_json, '$.reason'), '') AS reason,
                   coalesce(json_extract_string(details_json, '$.evidence_basis'), '') AS evidence_basis,
                   count(*) AS events, count(DISTINCT security_id) AS lines
            FROM {TABLE} WHERE {latest} GROUP BY ALL ORDER BY 1, 2, 3""").fetchall()]
    summary["load_seconds"] = round(loaded - started, 1)
    summary["build_seconds"] = round(time.perf_counter() - loaded, 1)
    text = json.dumps(summary, indent=2, default=str)
    print(text, flush=True)
    if args.summary is not None:
        args.summary.write_text(text + "\n", encoding="utf-8")
    con.close()
    return 0 if summary["tie_out_lines_over"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
