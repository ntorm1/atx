"""Operator entry point for the point-in-time US-listed universe."""

from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path

from atx_db.clock import utc_today
from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.universe_us_listed import (
    DEFAULT_US_LISTED_UNIVERSE_ID,
    UniverseUsListedOptions,
    refresh_universe_us_listed,
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="build-universe-us-listed")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--universe-id", default=DEFAULT_US_LISTED_UNIVERSE_ID)
    parser.add_argument("--lookback-days", type=int, default=20)
    parser.add_argument("--start-date", type=dt.date.fromisoformat)
    parser.add_argument("--end-date", type=dt.date.fromisoformat)
    parser.add_argument("--run-id")
    args = parser.parse_args(argv)

    options = UniverseUsListedOptions(
        universe_id=args.universe_id,
        lookback_days=args.lookback_days,
        start_date=args.start_date,
        end_date=args.end_date,
        as_of_date=args.end_date or utc_today(),
        run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        rows = refresh_universe_us_listed(store, options)
    print(json.dumps({"universe_id": options.universe_id, "intervals": rows}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
