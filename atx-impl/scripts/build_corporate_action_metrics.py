#!/usr/bin/env python
"""Materialize the derived ``corporate_action_dividend_metrics`` surface.

Computes per-dividend-event analytics (spot and trailing-twelve-month dividend yield,
TTM dividend sum/count, year-over-year dividend growth) from ``corporate_actions``
joined to the ex-date close in ``equity_daily_bars``. Pure derivation — no network.

Usage
-----
  python scripts/build_corporate_action_metrics.py [--db-path PATH]
         [--symbols AAPL MSFT ...] [--source TAG]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore
from db.corporate_action_metrics import (
    DEFAULT_SOURCE,
    CorporateActionDividendMetricsDataset,
    CorporateActionDividendMetricsOptions,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build derived cash-dividend analytics.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument(
        "--symbols",
        nargs="*",
        default=None,
        help="Restrict metrics to these symbols (default: all securities with dividends).",
    )
    parser.add_argument("--source", default=DEFAULT_SOURCE, help="Output source tag for the metric rows.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    symbols = tuple(args.symbols) if args.symbols else None
    options = CorporateActionDividendMetricsOptions(source=args.source, symbols=symbols)
    with DuckDBStore(args.db_path) as store:
        result = CorporateActionDividendMetricsDataset().run(store, options)
    print(
        json.dumps(
            {
                "step": "corporate_action_dividend_metrics",
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
