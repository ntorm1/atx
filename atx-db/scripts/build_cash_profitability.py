#!/usr/bin/env python
"""Materialize Ball-style cash operating profitability factors."""
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.cash_profitability import (
    CashProfitabilityOptions,
    refresh_cash_profitability_values,
)


def _date(value: str | None) -> dt.date | None:
    return None if value is None else dt.date.fromisoformat(value)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build PIT cash profitability and working-capital accrual factors."
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--start-date", type=_date, default=None)
    parser.add_argument("--end-date", type=_date, default=None)
    parser.add_argument("--min-market-cap", type=float, default=100_000_000.0)
    parser.add_argument("--min-adv21", type=float, default=1_000_000.0)
    parser.add_argument("--max-fundamental-age-days", type=int, default=550)
    parser.add_argument("--min-names", type=int, default=20)
    parser.add_argument("--winsor-limit", type=float, default=0.01)
    parser.add_argument("--run-id", default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    options = CashProfitabilityOptions(
        start_date=args.start_date,
        end_date=args.end_date,
        minimum_market_cap_usd=args.min_market_cap,
        minimum_adv21_usd=args.min_adv21,
        maximum_fundamental_age_days=args.max_fundamental_age_days,
        minimum_names_per_date=args.min_names,
        winsor_limit=args.winsor_limit,
        run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        rows = refresh_cash_profitability_values(store, options)
        coverage = store.con.execute(
            """
            SELECT
                factor_id,
                count(*) AS rows,
                count(DISTINCT security_id) AS securities,
                count(DISTINCT as_of_date) AS dates,
                min(as_of_date) AS first_date,
                max(as_of_date) AS last_date
            FROM fundamental_factor_values
            WHERE source = ? AND is_latest_revision
            GROUP BY factor_id
            ORDER BY factor_id
            """,
            [options.source],
        ).df()
    print(
        json.dumps(
            {
                "rows_loaded": rows,
                "source": options.source,
                "run_id": options.run_id,
                "coverage": coverage.to_dict(orient="records"),
            },
            indent=2,
            default=str,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
