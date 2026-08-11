#!/usr/bin/env python
"""Build the PIT operating-profitability / net-issuance conditional factor."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.conditional_router import (
    FACTOR_ID,
    ConditionalRouterOptions,
    refresh_conditional_router_values,
)
from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore


def _date(value: str) -> dt.date:
    return dt.date.fromisoformat(value)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--start-date", type=_date)
    parser.add_argument("--end-date", type=_date)
    parser.add_argument("--minimum-names-per-date", type=int, default=20)
    parser.add_argument("--universe-id", default="us_common_equity_liquid_v1")
    parser.add_argument("--run-id")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    options = ConditionalRouterOptions(
        start_date=args.start_date,
        end_date=args.end_date,
        minimum_names_per_date=args.minimum_names_per_date,
        universe_id=args.universe_id,
        run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        rows = refresh_conditional_router_values(store, options)
        stats = store.con.execute(
            """
            SELECT
                count(*) AS rows,
                count(DISTINCT security_id) AS securities,
                count(DISTINCT as_of_date) AS rebalance_dates,
                min(as_of_date) AS first_date,
                max(as_of_date) AS last_date
            FROM fundamental_factor_values
            WHERE source = ? AND factor_id = ?
            """,
            [options.source, FACTOR_ID],
        ).fetchone()
    print(
        json.dumps(
            {
                "factor_id": FACTOR_ID,
                "rows_materialized": rows,
                "source": options.source,
                "coverage": {
                    "rows": stats[0],
                    "securities": stats[1],
                    "rebalance_dates": stats[2],
                    "first_date": stats[3],
                    "last_date": stats[4],
                },
            },
            default=str,
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
