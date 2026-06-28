#!/usr/bin/env python
from __future__ import annotations

import argparse
import datetime as dt
import json
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore, FinraShortInterestDataset, FinraShortInterestOptions
from db.finra import parse_date, subtract_years


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Load FINRA consolidated short interest into the atx-impl DuckDB store."
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--symbol", help="Optional symbol filter, e.g. AAPL. Omit for date-window full-market loads.")
    parser.add_argument("--start-date", type=parse_date, default=None)
    parser.add_argument("--end-date", type=parse_date, default=None)
    parser.add_argument("--api-url", default=FinraShortInterestOptions.api_url)
    parser.add_argument("--limit", type=int, default=5000)
    parser.add_argument("--request-timeout", type=int, default=120)
    parser.add_argument("--max-retries", type=int, default=5)
    parser.add_argument("--retry-sleep", type=float, default=1.0)
    parser.add_argument("--limit-dates", type=int)
    parser.add_argument(
        "--date-order",
        choices=("asc", "desc"),
        default=FinraShortInterestOptions.date_order,
        help="Settlement-date fetch order when --symbol is omitted; use desc with --limit-dates for latest-date refreshes.",
    )
    parser.add_argument("--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    logging.basicConfig(level=getattr(logging, args.log_level), format="%(asctime)s %(levelname)s %(message)s")
    options = FinraShortInterestOptions(
        api_url=args.api_url,
        symbol=args.symbol,
        start_date=args.start_date if args.start_date else (None if args.symbol else subtract_years(dt.date.today(), 5)),
        end_date=args.end_date if args.end_date else (None if args.symbol else dt.date.today()),
        limit=args.limit,
        request_timeout=args.request_timeout,
        max_retries=args.max_retries,
        retry_sleep=args.retry_sleep,
        limit_dates=args.limit_dates,
        date_order=args.date_order,
    )
    with DuckDBStore(args.db_path) as store:
        result = FinraShortInterestDataset().run(store, options)
    print(json.dumps(result.details | {"rows_loaded": result.rows_loaded, "source": result.source}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
