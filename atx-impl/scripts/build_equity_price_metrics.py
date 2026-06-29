#!/usr/bin/env python
"""Materialize the derived ``equity_price_metrics`` surface from cached daily bars.

Computes per-security daily price analytics (adjusted daily/log return, overnight gap,
trailing realized volatility, momentum, distance from the trailing 252-day high, dollar
volume) from the cached ``equity_daily_bars`` feed. Pure derivation — no network access.

Usage
-----
  python scripts/build_equity_price_metrics.py [--db-path PATH]
         [--symbols AAPL MSFT ...] [--source TAG]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore
from db.equity_price_metrics import (
    DEFAULT_SOURCE,
    EquityPriceMetricsDataset,
    EquityPriceMetricsOptions,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build derived daily price analytics.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument(
        "--symbols",
        nargs="*",
        default=None,
        help="Restrict metrics to these symbols (default: all securities with daily bars).",
    )
    parser.add_argument("--source", default=DEFAULT_SOURCE, help="Output source tag for the metric rows.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    symbols = tuple(args.symbols) if args.symbols else None
    options = EquityPriceMetricsOptions(source=args.source, symbols=symbols)
    with DuckDBStore(args.db_path) as store:
        result = EquityPriceMetricsDataset().run(store, options)
    print(
        json.dumps(
            {
                "step": "equity_price_metrics",
                "rows_loaded": result.rows_loaded,
                "dataset_id": result.dataset_id,
                "run_id": result.run_id,
                "details": result.details,
            },
            indent=2,
            default=str,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
