#!/usr/bin/env python
"""Build first-filed standardized unexpected quarterly revenue."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.revenue_surprise import (
    FACTOR_ID,
    RevenueSurpriseOptions,
    refresh_revenue_surprise_values,
)


def _date(value: str) -> dt.date:
    return dt.date.fromisoformat(value)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--start-date", type=_date)
    parser.add_argument("--end-date", type=_date)
    parser.add_argument("--minimum-names-per-date", type=int, default=20)
    parser.add_argument("--run-id")
    args = parser.parse_args()
    options = RevenueSurpriseOptions(
        start_date=args.start_date,
        end_date=args.end_date,
        minimum_names_per_date=args.minimum_names_per_date,
        run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        rows = refresh_revenue_surprise_values(store, options)
        stats = store.con.execute(
            """SELECT count(*),count(DISTINCT security_id),count(DISTINCT as_of_date),
                      min(as_of_date),max(as_of_date)
               FROM fundamental_factor_values WHERE source=? AND factor_id=?""",
            [options.source, FACTOR_ID],
        ).fetchone()
    print(
        json.dumps(
            {
                "factor_id": FACTOR_ID,
                "rows_materialized": rows,
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
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
