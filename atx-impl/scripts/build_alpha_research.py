#!/usr/bin/env python
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore
from db.alpha_research import AlphaResearchDataset, AlphaResearchOptions
from db.warehouse import symbol_key


def parse_date(value: str | None) -> dt.date | None:
    if value in (None, ""):
        return None
    return dt.date.fromisoformat(value)


def split_symbols(value: str | None) -> tuple[str, ...] | None:
    if not value:
        return None
    symbols = sorted({symbol_key(part) for part in value.replace(" ", ",").split(",") if symbol_key(part)})
    return tuple(symbols) or None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build alpha expression registry, PIT signals, and backtest manifests.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--symbols", help="Optional comma/space-separated symbol filter. Defaults to all loaded feature symbols.")
    parser.add_argument("--feature-set", default=AlphaResearchOptions.feature_set)
    parser.add_argument("--universe-id", default=AlphaResearchOptions.universe_id)
    parser.add_argument("--start-date", type=parse_date)
    parser.add_argument("--end-date", type=parse_date)
    parser.add_argument("--horizon-days", type=int, default=AlphaResearchOptions.horizon_days)
    parser.add_argument("--top-quantile", type=float, default=AlphaResearchOptions.top_quantile)
    parser.add_argument("--bottom-quantile", type=float, default=AlphaResearchOptions.bottom_quantile)
    parser.add_argument("--min-cross-section", type=int, default=AlphaResearchOptions.min_cross_section)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    options = AlphaResearchOptions(
        symbols=split_symbols(args.symbols),
        feature_set=args.feature_set,
        universe_id=args.universe_id,
        start_date=args.start_date,
        end_date=args.end_date,
        horizon_days=args.horizon_days,
        top_quantile=args.top_quantile,
        bottom_quantile=args.bottom_quantile,
        min_cross_section=args.min_cross_section,
    )
    with DuckDBStore(args.db_path) as store:
        result = AlphaResearchDataset().run(store, options)
    print(json.dumps({"run_id": result.run_id, "rows_loaded": result.rows_loaded, "details": result.details}, indent=2, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
