#!/usr/bin/env python
"""Build monthly PIT gross-profitability, value, and quality/value factors."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.fundamental_signals import FundamentalSignalOptions, refresh_fundamental_signal_values


def _date(value: str) -> dt.date:
    return dt.date.fromisoformat(value)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--start-date", type=_date)
    parser.add_argument("--end-date", type=_date)
    parser.add_argument("--minimum-market-cap-usd", type=float, default=100_000_000.0)
    parser.add_argument("--minimum-adv21-usd", type=float, default=1_000_000.0)
    parser.add_argument("--maximum-fundamental-age-days", type=int, default=550)
    parser.add_argument("--minimum-names-per-date", type=int, default=20)
    parser.add_argument("--winsor-limit", type=float, default=0.01)
    parser.add_argument("--run-id")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    options = FundamentalSignalOptions(
        start_date=args.start_date,
        end_date=args.end_date,
        minimum_market_cap_usd=args.minimum_market_cap_usd,
        minimum_adv21_usd=args.minimum_adv21_usd,
        maximum_fundamental_age_days=args.maximum_fundamental_age_days,
        minimum_names_per_date=args.minimum_names_per_date,
        winsor_limit=args.winsor_limit,
        run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        rows = refresh_fundamental_signal_values(store, options)
        stats = store.con.execute(
            """
            SELECT
                factor_id,
                count(*) AS rows,
                count(DISTINCT security_id) AS securities,
                count(DISTINCT as_of_date) AS rebalance_dates,
                min(as_of_date) AS first_date,
                max(as_of_date) AS last_date
            FROM fundamental_factor_values
            WHERE source = ?
            GROUP BY factor_id
            ORDER BY factor_id
            """,
            [options.source],
        ).fetchdf()
    print(
        json.dumps(
            {
                "rows_materialized": rows,
                "source": options.source,
                "factors": stats.to_dict("records"),
            },
            default=str,
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
