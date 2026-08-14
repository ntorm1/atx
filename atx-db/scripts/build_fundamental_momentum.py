#!/usr/bin/env python
"""Build PIT SUE residualized against split-adjusted 12-1 price momentum."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.fundamental_momentum import (
    FACTOR_ID,
    FundamentalMomentumOptions,
    refresh_fundamental_momentum_values,
)


def _date(value: str) -> dt.date:
    return dt.date.fromisoformat(value)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--start-date", type=_date)
    parser.add_argument("--end-date", type=_date)
    parser.add_argument("--skip-sessions", type=int, default=21)
    parser.add_argument("--lookback-sessions", type=int, default=252)
    parser.add_argument("--maximum-reference-staleness-days", type=int, default=7)
    parser.add_argument("--minimum-names-per-date", type=int, default=20)
    parser.add_argument("--winsor-limit", type=float, default=0.01)
    parser.add_argument("--run-id")
    args = parser.parse_args()
    options = FundamentalMomentumOptions(
        start_date=args.start_date,
        end_date=args.end_date,
        skip_sessions=args.skip_sessions,
        lookback_sessions=args.lookback_sessions,
        maximum_reference_staleness_days=args.maximum_reference_staleness_days,
        minimum_names_per_date=args.minimum_names_per_date,
        winsor_limit=args.winsor_limit,
        run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        count = refresh_fundamental_momentum_values(store, options)
        coverage = store.con.execute(
            """
            SELECT count(*), count(DISTINCT security_id), count(DISTINCT as_of_date),
                   min(as_of_date), max(as_of_date)
            FROM fundamental_factor_values
            WHERE factor_id=? AND source=?
            """,
            [FACTOR_ID, options.source],
        ).fetchone()
    print(
        json.dumps(
            {
                "rows_materialized": count,
                "coverage": {
                    "rows": coverage[0],
                    "securities": coverage[1],
                    "rebalance_dates": coverage[2],
                    "first_date": coverage[3],
                    "last_date": coverage[4],
                },
            },
            default=str,
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
