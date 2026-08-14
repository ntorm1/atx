#!/usr/bin/env python
"""Atomically publish the complete local ticker-history archive with DuckDB."""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.ticker_history_bulk import BulkTickerHistoryOptions, publish_bulk_ticker_history


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--tsv-path",
        type=Path,
        default=Path("data/staging/broad-bars/tbltickerhistory3_10y.txt"),
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--memory-limit", default="4GB")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--run-id", default="broad-bars-native-bulk-v1")
    args = parser.parse_args()
    with DuckDBStore(args.db_path) as store:
        result = publish_bulk_ticker_history(
            store,
            BulkTickerHistoryOptions(
                tsv_path=args.tsv_path.resolve(),
                memory_limit=args.memory_limit,
                threads=args.threads,
                run_id=args.run_id,
            ),
        )
    print(json.dumps(asdict(result), default=str, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
