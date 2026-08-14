#!/usr/bin/env python
"""Build the PIT abnormal receivables-growth factor."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.abnormal_receivables_growth import (
    FACTOR_ID,
    AbnormalReceivablesGrowthOptions,
    refresh_abnormal_receivables_growth_values,
)
from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore


def _date(value: str) -> dt.date:
    return dt.date.fromisoformat(value)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--start-date", type=_date)
    parser.add_argument("--end-date", type=_date)
    parser.add_argument("--maximum-absolute-receivables-growth", type=float, default=10.0)
    parser.add_argument("--maximum-absolute-abnormal-growth", type=float, default=10.0)
    parser.add_argument("--minimum-names-per-date", type=int, default=20)
    parser.add_argument("--winsor-limit", type=float, default=0.01)
    parser.add_argument("--run-id")
    args = parser.parse_args()
    options = AbnormalReceivablesGrowthOptions(
        start_date=args.start_date,
        end_date=args.end_date,
        maximum_absolute_receivables_growth=args.maximum_absolute_receivables_growth,
        maximum_absolute_abnormal_growth=args.maximum_absolute_abnormal_growth,
        minimum_names_per_date=args.minimum_names_per_date,
        winsor_limit=args.winsor_limit,
        run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        rows = refresh_abnormal_receivables_growth_values(store, options)
        coverage = store.con.execute(
            """SELECT count(*),count(DISTINCT security_id),count(DISTINCT as_of_date),
                      min(as_of_date),max(as_of_date)
               FROM fundamental_factor_values WHERE source=? AND factor_id=?""",
            [options.source, FACTOR_ID],
        ).fetchone()
    print(
        json.dumps(
            {
                "rows_materialized": rows,
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
