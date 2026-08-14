#!/usr/bin/env python
"""Build the governed point-in-time tax-to-book income factor."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.tax_to_book_income import (
    FACTOR_ID,
    TaxToBookIncomeOptions,
    refresh_tax_to_book_income_values,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--start-date", type=dt.date.fromisoformat)
    parser.add_argument("--end-date", type=dt.date.fromisoformat)
    parser.add_argument("--maximum-reporting-age-days", type=int, default=550)
    parser.add_argument("--maximum-raw-value", type=float, default=10.0)
    parser.add_argument("--minimum-names-per-date", type=int, default=20)
    parser.add_argument("--winsor-limit", type=float, default=0.01)
    parser.add_argument("--run-id")
    args = parser.parse_args()
    options = TaxToBookIncomeOptions(
        start_date=args.start_date,end_date=args.end_date,
        maximum_reporting_age_days=args.maximum_reporting_age_days,
        maximum_raw_value=args.maximum_raw_value,
        minimum_names_per_date=args.minimum_names_per_date,
        winsor_limit=args.winsor_limit,run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        count = refresh_tax_to_book_income_values(store,options)
        coverage = store.con.execute(
            """SELECT count(*) AS rows,count(DISTINCT security_id) AS securities,
                      count(DISTINCT as_of_date) AS rebalance_dates,
                      min(as_of_date) AS first_date,max(as_of_date) AS last_date
               FROM fundamental_factor_values WHERE factor_id=? AND source=?""",
            [FACTOR_ID,options.source],
        ).fetchdf()
    print(json.dumps({"rows_materialized":count,"coverage":coverage.to_dict("records")},default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
