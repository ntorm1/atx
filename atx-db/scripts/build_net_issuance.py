#!/usr/bin/env python
"""Build the monthly PIT, split-adjusted net share issuance factor."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.net_issuance import (
    FACTOR_ID,
    NetIssuanceOptions,
    refresh_net_issuance_values,
)


def _date(value: str) -> dt.date:
    return dt.date.fromisoformat(value)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--start-date", type=_date)
    parser.add_argument("--end-date", type=_date)
    parser.add_argument("--minimum-market-cap-usd", type=float, default=100_000_000.0)
    parser.add_argument("--minimum-adv21-usd", type=float, default=1_000_000.0)
    parser.add_argument("--maximum-observation-age-days", type=int, default=550)
    parser.add_argument("--minimum-names-per-date", type=int, default=20)
    parser.add_argument("--winsor-limit", type=float, default=0.01)
    parser.add_argument("--maximum-absolute-log-change", type=float, default=3.0)
    parser.add_argument("--run-id")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    options = NetIssuanceOptions(
        start_date=args.start_date,
        end_date=args.end_date,
        minimum_market_cap_usd=args.minimum_market_cap_usd,
        minimum_adv21_usd=args.minimum_adv21_usd,
        maximum_observation_age_days=args.maximum_observation_age_days,
        minimum_names_per_date=args.minimum_names_per_date,
        winsor_limit=args.winsor_limit,
        maximum_absolute_log_change=args.maximum_absolute_log_change,
        run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        rows = refresh_net_issuance_values(store, options)
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
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
