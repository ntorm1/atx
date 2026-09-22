"""Operator entry point for the daily market panel and its PIT fundamental join."""

from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path

from atx_db.connection import DuckDBStore, resolve_data_dir
from atx_db.market_daily import (
    MarketDailyOptions,
    refresh_market_daily_metrics,
    shares_reconciliation_report,
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=None)
    parser.add_argument("--security-id", action="append", default=None)
    parser.add_argument("--batch-size", type=int, default=200)
    parser.add_argument("--start-date", type=dt.date.fromisoformat, default=None)
    parser.add_argument("--end-date", type=dt.date.fromisoformat, default=None)
    parser.add_argument("--run-id", default=None)
    args = parser.parse_args(argv)

    db_path = args.db_path or (resolve_data_dir() / "warehouse.duckdb")
    with DuckDBStore(db_path) as store:
        store.initialize()
        options = MarketDailyOptions(
            security_ids=tuple(args.security_id) if args.security_id else None,
            batch_size=args.batch_size,
            start_date=args.start_date,
            end_date=args.end_date,
            run_id=args.run_id,
        )
        rows = refresh_market_daily_metrics(store, options)
        report = shares_reconciliation_report(store, tolerance=options.shares_tolerance)
    print(json.dumps({"rows": rows, **report}, sort_keys=True, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
