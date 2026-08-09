#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore, ThirteenFDataSet, ThirteenFOptions
from atx_db.thirteenf import AAPL_CUSIP, normalize_cusip


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Load SEC Form 13F bulk data into the atx-db DuckDB store.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--dataset-url", help="SEC Form 13F ZIP URL. Defaults to latest discovered SEC ZIP.")
    parser.add_argument("--cache-dir", type=Path, default=ThirteenFOptions().cache_dir)
    parser.add_argument(
        "--cusip",
        action="append",
        default=None,
        help="CUSIP filter. Repeat for multiple. Defaults to AAPL; use --full-holdings for all rows.",
    )
    parser.add_argument("--full-holdings", action="store_true", help="Import every INFOTABLE holding row.")
    parser.add_argument("--chunk-size", type=int, default=200_000)
    parser.add_argument("--request-timeout", type=int, default=180)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.full_holdings:
        cusips = None
    else:
        cusips = tuple(normalize_cusip(value) for value in (args.cusip or [AAPL_CUSIP]))
    options = ThirteenFOptions(
        dataset_url=args.dataset_url,
        cache_dir=args.cache_dir,
        cusips=cusips,
        chunk_size=args.chunk_size,
        request_timeout=args.request_timeout,
    )
    with DuckDBStore(args.db_path) as store:
        result = ThirteenFDataSet().run(store, options)
    print(json.dumps(result.details | {"rows_loaded": result.rows_loaded, "source": result.source}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
