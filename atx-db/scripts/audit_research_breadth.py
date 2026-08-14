#!/usr/bin/env python
"""Publish the current research-universe breadth funnel and factor coverage."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore

DEFAULT_FACTORS = (
    "profitability_quarterly_gross_profitability_lagged_assets",
    "profitability_quarterly_gross_profitability_trend_8q",
    "efficiency_quarterly_asset_turnover_trend_8q",
    "profitability_quarterly_gross_margin_trend_8q",
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--factor-ids", nargs="+", default=DEFAULT_FACTORS)
    parser.add_argument("--institutional-target", type=int, default=1_000)
    parser.add_argument(
        "--output", type=Path, default=Path("research/research-breadth-funnel.json")
    )
    args = parser.parse_args()
    with DuckDBStore(args.db_path, read_only=True) as store:
        latest_date = store.con.execute(
            "SELECT max(trade_date) FROM equity_daily_bars"
        ).fetchone()[0]
        price = store.con.execute(
            """
            SELECT
                count(DISTINCT security_id) FILTER (WHERE trade_date=?) AS exact_date,
                count(DISTINCT security_id) FILTER (WHERE trade_date>=?-30) AS trailing_30d,
                count(DISTINCT security_id) AS history
            FROM equity_daily_bars
            """,
            [latest_date, latest_date],
        ).fetchone()
        statements = store.con.execute(
            """
            SELECT
                count(DISTINCT security_id) FILTER (WHERE period_end>=?-450),
                count(DISTINCT security_id)
            FROM fundamental_statement_points
            """,
            [latest_date],
        ).fetchone()
        overlap = store.con.execute(
            """
            SELECT count(DISTINCT b.security_id)
            FROM equity_daily_bars b
            SEMI JOIN fundamental_statement_points f
              ON f.security_id=b.security_id AND f.period_end>=?-450
            WHERE b.trade_date>=?-30
            """,
            [latest_date, latest_date],
        ).fetchone()[0]
        universe_rows = store.con.execute(
            """
            SELECT reason,is_member,count(DISTINCT security_id)
            FROM universe_membership
            WHERE universe_id='us_common_equity_liquid_v1'
              AND valid_from<=?
              AND (valid_to IS NULL OR valid_to>=?)
              AND is_latest_revision
            GROUP BY reason,is_member ORDER BY 3 DESC
            """,
            [latest_date, latest_date],
        ).fetchall()
        factors: list[dict[str, object]] = []
        for factor_id in args.factor_ids:
            row = store.con.execute(
                """
                WITH counts AS (
                    SELECT as_of_date,count(DISTINCT security_id) AS names
                    FROM fundamental_factor_values
                    WHERE factor_id=? AND is_latest_revision AND value IS NOT NULL
                    GROUP BY as_of_date
                )
                SELECT max(as_of_date),arg_max(names,as_of_date),median(names),
                       avg(names),min(names),max(names)
                FROM counts
                """,
                [factor_id],
            ).fetchone()
            median_names = None if row[2] is None else float(row[2])
            factors.append(
                {
                    "factor_id": factor_id,
                    "latest_date": row[0],
                    "latest_names": row[1],
                    "median_names": median_names,
                    "mean_names": row[3],
                    "minimum_names": row[4],
                    "maximum_names": row[5],
                    "median_target_fraction": (
                        None
                        if median_names is None
                        else median_names / args.institutional_target
                    ),
                }
            )
    reasons = [
        {"reason": reason, "is_member": member, "securities": count}
        for reason, member, count in universe_rows
    ]
    payload = {
        "as_of_date": latest_date,
        "institutional_target_names": args.institutional_target,
        "price_securities": {
            "exact_date": price[0],
            "trailing_30d": price[1],
            "all_history": price[2],
        },
        "fundamental_statement_securities": {
            "recent_450d": statements[0],
            "all_history": statements[1],
        },
        "recent_price_statement_overlap": overlap,
        "governed_universe": reasons,
        "factor_coverage": factors,
        "primary_bottleneck": (
            "recent_daily_bar_coverage"
            if price[1] < statements[0]
            else "recent_fundamental_statement_coverage"
        ),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(payload, indent=2, sort_keys=True, default=str) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(payload, indent=2, sort_keys=True, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
