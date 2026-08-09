#!/usr/bin/env python
"""Build the interval-keyed PIT universe used by ``v_factor_panel``.

The older :mod:`scripts.build_universe` command materializes daily legacy
snapshots in ``universe_memberships``.  This command owns the governed
``universe_membership`` contract consumed by the production factor panel.

Examples
--------
Build the complete common-equity universe::

    python scripts/build_governed_universe.py

Build only the securities needed for one or more factor research runs::

    python scripts/build_governed_universe.py \
      --factor-ids profitability_operating_profitability \
      --min-price 0.01 --min-dollar-volume 1000000
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.universe import (
    DEFAULT_UNIVERSE_ID,
    GovernedUniverseMembershipDataset,
    UniverseMembershipOptions,
)


def _date(value: str | None) -> dt.date | None:
    return None if value is None else dt.date.fromisoformat(value)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build the governed PIT universe consumed by v_factor_panel."
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--universe-id", default=DEFAULT_UNIVERSE_ID)
    parser.add_argument("--name", default="US common-equity liquid PIT universe")
    parser.add_argument(
        "--description",
        default=(
            "Interval-keyed, point-in-time common-equity universe screened on "
            "trailing price and dollar-volume history."
        ),
    )
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument("--symbols", nargs="*", default=None)
    scope.add_argument(
        "--factor-ids",
        nargs="+",
        default=None,
        help="Limit the build to securities present in these factor series.",
    )
    parser.add_argument("--start-date", type=_date, default=None)
    parser.add_argument("--end-date", type=_date, default=None)
    parser.add_argument("--lookback-days", type=int, default=20)
    parser.add_argument("--min-history-days", type=int, default=20)
    parser.add_argument("--min-price", type=float, default=5.0)
    parser.add_argument("--min-dollar-volume", type=float, default=10_000_000.0)
    parser.add_argument("--run-id", default=None)
    return parser.parse_args()


def _factor_symbols(store: DuckDBStore, factor_ids: list[str]) -> tuple[str, ...]:
    store.con.register(
        "governed_universe_factor_filter",
        pd.DataFrame({"factor_id": sorted(set(factor_ids))}),
    )
    try:
        rows = store.con.execute(
            """
            SELECT DISTINCT symbol
            FROM fundamental_factor_values f
            JOIN governed_universe_factor_filter x USING (factor_id)
            WHERE f.is_latest_revision
              AND f.symbol IS NOT NULL
            ORDER BY symbol
            """
        ).fetchall()
    finally:
        store.con.unregister("governed_universe_factor_filter")
    return tuple(str(row[0]) for row in rows)


def main() -> int:
    args = parse_args()
    with DuckDBStore(args.db_path) as store:
        store.con.execute("PRAGMA disable_progress_bar")
        symbols = tuple(args.symbols) if args.symbols is not None else None
        if args.factor_ids:
            symbols = _factor_symbols(store, args.factor_ids)
            if not symbols:
                raise SystemExit("No current factor observations matched --factor-ids")

        options = UniverseMembershipOptions(
            universe_id=args.universe_id,
            name=args.name,
            description=args.description,
            symbols=symbols,
            start_date=args.start_date,
            end_date=args.end_date,
            lookback_days=args.lookback_days,
            min_history_days=args.min_history_days,
            min_price=args.min_price,
            min_dollar_volume=args.min_dollar_volume,
            run_id=args.run_id,
        )
        result = GovernedUniverseMembershipDataset().run(store, options)
        coverage = store.con.execute(
            """
            SELECT
                count(*) AS interval_count,
                count(*) FILTER (WHERE is_member) AS member_intervals,
                count(DISTINCT security_id) FILTER (WHERE is_member) AS member_securities,
                min(valid_from) FILTER (WHERE is_member) AS first_member_date,
                max(valid_to) FILTER (WHERE is_member) AS last_member_date
            FROM universe_membership
            WHERE universe_id = ? AND is_latest_revision
            """,
            [args.universe_id],
        ).fetchone()
        if coverage is None:
            raise RuntimeError("governed universe coverage query returned no row")

    print(
        json.dumps(
            {
                "universe_id": args.universe_id,
                "rows_loaded": result.rows_loaded,
                "scoped_symbols": None if symbols is None else len(symbols),
                "interval_count": coverage[0],
                "member_intervals": coverage[1],
                "member_securities": coverage[2],
                "first_member_date": str(coverage[3]),
                "last_member_date": str(coverage[4]),
                "rules": result.details.get("rules"),
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
