#!/usr/bin/env python
"""Publish the reconciliation serving table in bounded symbol shards.

A single full-universe materialization of ``v_fundamental_reconciliation_contextual``
can exceed available memory because the whole contextual result must stage at
once. Scoped publishes are the governed alternative: each shard upserts its
security scope and prunes stale rows only inside that scope, so sequential
shards compose into exactly one full publication without ever holding the
complete result in memory.

Usage
-----
  python scripts/refresh_reconciliation_sharded.py --shards 16
  python scripts/refresh_reconciliation_sharded.py --shards 16 --start-shard 7   # resume
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.cli import _configure_analytical_session
from atx_db.fundamental_reconciliation import (
    FundamentalReconciliationRefreshOptions,
    refresh_fundamental_reconciliation_serving,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Sequential symbol-sharded reconciliation serving publish."
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--shards", type=int, default=16)
    parser.add_argument("--start-shard", type=int, default=1)
    parser.add_argument("--memory-limit", default="8GB")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--run-id-prefix", default="recon-sharded")
    return parser.parse_args()


def _symbol_shards(db_path: Path, shard_count: int) -> list[list[str]]:
    with DuckDBStore(db_path, read_only=True) as store:
        symbols = [
            row[0]
            for row in store.con.execute(
                """
                SELECT DISTINCT upper(trim(symbol))
                FROM fundamental_standardized
                WHERE symbol IS NOT NULL AND trim(symbol) <> ''
                ORDER BY 1
                """
            ).fetchall()
        ]
    if not symbols:
        raise SystemExit("no symbols found in fundamental_standardized")
    size = -(-len(symbols) // shard_count)
    return [symbols[i : i + size] for i in range(0, len(symbols), size)]


def main() -> int:
    args = parse_args()
    if args.shards < 1:
        raise SystemExit("--shards must be positive")
    shards = _symbol_shards(args.db_path, args.shards)
    print(
        json.dumps(
            {
                "step": "plan",
                "symbol_count": sum(len(s) for s in shards),
                "shard_count": len(shards),
                "start_shard": args.start_shard,
            }
        ),
        flush=True,
    )
    for index, shard in enumerate(shards, start=1):
        if index < args.start_shard:
            continue
        begun = time.monotonic()
        with DuckDBStore(args.db_path) as store:
            store.con.execute("PRAGMA disable_progress_bar")
            _configure_analytical_session(
                store, memory_limit=args.memory_limit, threads=args.threads
            )
            result = refresh_fundamental_reconciliation_serving(
                store,
                FundamentalReconciliationRefreshOptions(
                    symbols=tuple(shard),
                    run_id=f"{args.run_id_prefix}-s{index:02d}",
                ),
            )
        print(
            json.dumps(
                {
                    "step": "shard",
                    "shard": index,
                    "of": len(shards),
                    "symbols": len(shard),
                    "build_id": result.build_id,
                    "serving_rows_in_scope": result.row_count,
                    "seconds": round(time.monotonic() - begun, 1),
                }
            ),
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
