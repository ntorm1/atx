#!/usr/bin/env python
"""Build PIT five-year earnings seasonality for predicted announcement months."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.earnings_seasonality import (
    FACTOR_ID,
    EarningsSeasonalityOptions,
    refresh_earnings_seasonality_values,
)


def _date(value: str) -> dt.date:
    return dt.date.fromisoformat(value)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--start-date", type=_date)
    parser.add_argument("--end-date", type=_date)
    parser.add_argument("--minimum-market-cap-usd", type=float, default=100_000_000.0)
    parser.add_argument("--minimum-adv21-usd", type=float, default=1_000_000.0)
    parser.add_argument("--minimum-price-usd", type=float, default=5.0)
    parser.add_argument("--minimum-names-per-date", type=int, default=20)
    parser.add_argument("--winsor-limit", type=float, default=0.01)
    parser.add_argument("--run-id")
    args = parser.parse_args()
    options = EarningsSeasonalityOptions(
        start_date=args.start_date,
        end_date=args.end_date,
        minimum_market_cap_usd=args.minimum_market_cap_usd,
        minimum_adv21_usd=args.minimum_adv21_usd,
        minimum_price_usd=args.minimum_price_usd,
        minimum_names_per_date=args.minimum_names_per_date,
        winsor_limit=args.winsor_limit,
        run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        count = refresh_earnings_seasonality_values(store, options)
        coverage = store.con.execute(
            """
            SELECT count(*),count(DISTINCT security_id),count(DISTINCT as_of_date),
                   min(as_of_date),max(as_of_date),median(names)
            FROM (
                SELECT *,count(DISTINCT security_id) OVER (PARTITION BY as_of_date) AS names
                FROM fundamental_factor_values WHERE factor_id=? AND source=?
            )
            """,
            [FACTOR_ID, options.source],
        ).fetchone()
    print(json.dumps({
        "rows_materialized": count,
        "coverage": {
            "rows": coverage[0], "securities": coverage[1], "dates": coverage[2],
            "first_date": coverage[3], "last_date": coverage[4],
            "median_names_per_date": coverage[5],
        },
    }, default=str, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
