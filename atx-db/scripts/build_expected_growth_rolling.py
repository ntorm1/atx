#!/usr/bin/env python
"""Build point-in-time q5 rolling-WLS slopes and expected-growth forecasts."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.expected_growth_rolling import (
    FACTOR_ID,
    MODEL_ID,
    SOURCE_NAME,
    RollingExpectedGrowthOptions,
    refresh_rolling_expected_growth_values,
)


def _date(value: str) -> dt.date:
    return dt.date.fromisoformat(value)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--start-date", type=_date)
    parser.add_argument("--end-date", type=_date)
    parser.add_argument("--minimum-regression-names", type=int, default=100)
    parser.add_argument("--maximum-condition-number", type=float, default=1_000.0)
    parser.add_argument("--slope-window-months", type=int, default=120)
    parser.add_argument("--minimum-slope-months", type=int, default=30)
    parser.add_argument("--winsor-limit", type=float, default=0.01)
    parser.add_argument("--run-id")
    args = parser.parse_args()
    options = RollingExpectedGrowthOptions(
        start_date=args.start_date,
        end_date=args.end_date,
        minimum_regression_names=args.minimum_regression_names,
        maximum_condition_number=args.maximum_condition_number,
        slope_window_months=args.slope_window_months,
        minimum_slope_months=args.minimum_slope_months,
        winsor_limit=args.winsor_limit,
        run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        counts = refresh_rolling_expected_growth_values(store, options)
        coverage = store.con.execute(
            """SELECT count(*) AS rows,
                      count(DISTINCT security_id) AS securities,
                      count(DISTINCT as_of_date) AS rebalance_dates,
                      min(as_of_date) AS first_date,max(as_of_date) AS last_date
               FROM fundamental_factor_values
               WHERE source=? AND factor_id=?""",
            [SOURCE_NAME, FACTOR_ID],
        ).fetchdf()
        slope_coverage = store.con.execute(
            """SELECT count(*) AS rows,min(as_of_date) AS first_date,
                      max(as_of_date) AS last_date,min(n_obs) AS minimum_names,
                      max(condition_number) AS maximum_condition_number
               FROM expected_growth_model_slopes
               WHERE source=? AND model_id=?""",
            [SOURCE_NAME, MODEL_ID],
        ).fetchdf()
    print(
        json.dumps(
            {
                "materialized": counts,
                "factor_coverage": coverage.to_dict(orient="records"),
                "slope_coverage": slope_coverage.to_dict(orient="records"),
            },
            default=str,
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
