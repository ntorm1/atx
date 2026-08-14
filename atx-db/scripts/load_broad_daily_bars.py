#!/usr/bin/env python
"""Load a broad multi-year daily-bar universe from the local tbltickerhistory archive.

Unlike ``bootstrap_research_universe.py`` (which loads a bounded symbol *preset* plus
the full feature/corp-action/universe DAG), this loads **every** symbol in the archive
for an early multi-year window bounded by ``--max-chunks`` (the archive is date-ordered,
so N chunks ≈ the first N*chunk_size rows ≈ the earliest dates). It then runs the
recycled-ticker / share-class collision repair that ``TickerHistoryDataset.load`` applies
automatically, so ``equity_daily_bars`` ends up with one clean row per
``(security_id, trade_date)``.

Pure local file read — no network. After loading, rebuild the derived price surface with
``scripts/build_equity_price_metrics.py`` and refresh quality/watermarks/lake.

Usage
-----
  python scripts/load_broad_daily_bars.py [--db-path PATH] [--max-chunks 16]
         [--zip-path PATH] [--chunk-size 200000]
"""
from __future__ import annotations

import argparse
import json
import logging
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.ticker_history import (
    DEFAULT_TICKER_HISTORY_ZIP,
    TickerHistoryDataset,
    TickerHistoryOptions,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Load the full tbltickerhistory symbol universe (bounded window).")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--zip-path", type=Path, default=DEFAULT_TICKER_HISTORY_ZIP)
    parser.add_argument("--max-chunks", type=int, default=16, help="Chunks to read (None-equivalent: pass a large value).")
    parser.add_argument("--chunk-size", type=int, default=200_000)
    parser.add_argument(
        "--memory-limit",
        default="4GB",
        help="DuckDB working-memory ceiling; overflow may spill to its temp directory.",
    )
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument(
        "--skip-chunks",
        type=int,
        default=0,
        help="Resume after this many archive chunks; max-chunks remains the absolute stop.",
    )
    parser.add_argument(
        "--price-projection-only",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Read only canonical price columns; disable only when raw option fields are required.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    if not args.zip_path.exists():
        raise FileNotFoundError(args.zip_path)
    t0 = time.time()
    with DuckDBStore(args.db_path) as store:
        store.con.execute("PRAGMA disable_progress_bar")
        store.con.execute("SET memory_limit = ?", [args.memory_limit])
        store.con.execute("SET threads = ?", [args.threads])
        result = TickerHistoryDataset().load(
            store,
            TickerHistoryOptions(
                zip_path=args.zip_path,
                symbols=None,
                max_chunks=args.max_chunks,
                skip_chunks=args.skip_chunks,
                chunk_size=args.chunk_size,
                price_projection_only=args.price_projection_only,
                compute_source_hash=False,
                run_id="broad-bars-production-breadth-v1",
            ),
        )
        bars = store.con.execute(
            "SELECT COUNT(*), COUNT(DISTINCT security_id), MIN(trade_date), MAX(trade_date) "
            "FROM equity_daily_bars"
        ).fetchone()
        securities = store.con.execute("SELECT COUNT(*) FROM securities").fetchone()[0]
    print(
        json.dumps(
            {
                "rows_loaded": result.rows_loaded,
                "elapsed_sec": round(time.time() - t0, 1),
                "vendor_collisions_rekeyed": result.details.get("vendor_collisions_rekeyed"),
                "matched_symbol_count": result.details.get("matched_symbol_count"),
                "min_trading_date": str(result.details.get("min_trading_date")),
                "max_trading_date": str(result.details.get("max_trading_date")),
                "equity_daily_bars_rows": bars[0],
                "equity_daily_bars_securities": bars[1],
                "bars_min_date": str(bars[2]),
                "bars_max_date": str(bars[3]),
                "securities": securities,
                "price_projection_only": args.price_projection_only,
                "skip_chunks": args.skip_chunks,
                "memory_limit": args.memory_limit,
                "threads": args.threads,
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
