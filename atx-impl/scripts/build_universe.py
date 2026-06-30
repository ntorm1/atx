#!/usr/bin/env python
"""Build a point-in-time, survivorship-bias-free investable universe from canonical bars.

Materializes ``universe_memberships`` (one row per (universe_id, security_id, as_of_date)
that passes the trailing-liquidity screen) over ``equity_daily_bars``. Each membership's
``available_at`` is the bar close (as_of_date 22:00), so the universe is point-in-time:
a backtest as of date D sees exactly the names that were liquid on D, with no look-ahead
and no survivorship bias (delisted names remain members on the dates they qualified).

Defaults build the BROAD universe over every security with bars (``--symbols`` restricts
it). Pure derivation from cached bars — no network.

Usage
-----
  python scripts/build_universe.py [--db-path PATH]
         [--universe-id us_broad_liquid_equity_v1] [--symbols AAPL MSFT ...]
         [--min-price 5.0] [--min-dollar-volume 10000000]
         [--lookback-days 20] [--min-history-days 20]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore
from db.universes import UniverseBuildOptions, UniverseMembershipDataset


def parse_date(value: str | None):
    import datetime as dt

    return None if not value else dt.date.fromisoformat(value)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build a PIT liquidity-screened investable universe.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--universe-id", default="us_broad_liquid_equity_v1")
    parser.add_argument("--name", default="US broad liquid equity universe")
    parser.add_argument(
        "--description",
        default="Survivorship-bias-free PIT trailing-liquidity universe over all canonical daily bars.",
    )
    parser.add_argument("--symbols", nargs="*", default=None, help="Restrict to these symbols (default: all securities).")
    parser.add_argument("--start-date", type=parse_date, default=None)
    parser.add_argument("--end-date", type=parse_date, default=None)
    parser.add_argument("--lookback-days", type=int, default=20)
    parser.add_argument("--min-history-days", type=int, default=20)
    parser.add_argument("--min-price", type=float, default=5.0)
    parser.add_argument("--min-dollar-volume", type=float, default=10_000_000.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    options = UniverseBuildOptions(
        universe_id=args.universe_id,
        name=args.name,
        description=args.description,
        symbols=tuple(args.symbols) if args.symbols else None,
        start_date=args.start_date,
        end_date=args.end_date,
        lookback_days=args.lookback_days,
        min_history_days=args.min_history_days,
        min_price=args.min_price,
        min_dollar_volume=args.min_dollar_volume,
    )
    with DuckDBStore(args.db_path) as store:
        store.con.execute("PRAGMA disable_progress_bar")
        result = UniverseMembershipDataset().run(store, options)
        c = store.con
        secs, dmin, dmax = c.execute(
            "SELECT COUNT(DISTINCT security_id), MIN(as_of_date), MAX(as_of_date) "
            "FROM universe_memberships WHERE universe_id = ?",
            [args.universe_id],
        ).fetchone()
    print(
        json.dumps(
            {
                "universe_id": args.universe_id,
                "rows_loaded": result.rows_loaded,
                "distinct_securities": secs,
                "as_of_min": str(dmin),
                "as_of_max": str(dmax),
                "rules": result.details.get("rules"),
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
