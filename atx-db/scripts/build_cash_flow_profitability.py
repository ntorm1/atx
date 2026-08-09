#!/usr/bin/env python
"""Build TTM cash-flow profitability and low-total-accrual factors."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.cash_flow_profitability import (
    FACTOR_IDS,
    CashFlowProfitabilityOptions,
    refresh_cash_flow_profitability_values,
)
from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore


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
    options = CashFlowProfitabilityOptions(
        start_date=args.start_date,
        end_date=args.end_date,
        minimum_names_per_date=args.minimum_names_per_date,
        run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        rows = refresh_cash_flow_profitability_values(store, options)
        coverage = store.con.execute(
            """SELECT factor_id,count(*),count(DISTINCT security_id),
                      count(DISTINCT as_of_date),min(as_of_date),max(as_of_date)
               FROM fundamental_factor_values
               WHERE source=? AND factor_id IN (?,?)
               GROUP BY factor_id ORDER BY factor_id""",
            [options.source, *FACTOR_IDS],
        ).fetchall()
    print(
        json.dumps(
            {
                "factor_ids": FACTOR_IDS,
                "rows_materialized": rows,
                "coverage": [
                    {
                        "factor_id": row[0],
                        "rows": row[1],
                        "securities": row[2],
                        "rebalance_dates": row[3],
                        "first_date": row[4],
                        "last_date": row[5],
                    }
                    for row in coverage
                ],
            },
            default=str,
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
