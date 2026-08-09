#!/usr/bin/env python
"""Materialize the derived ``fundamental_ratios`` surface from warehouse fundamentals.

Computes Compustat/FactSet-style calculated financial ratios (profitability,
leverage, cash-flow, payout, per-share) from the trailing-twelve-month flows in
``fundamental_ttm_points`` and the instant balances in ``fundamental_statement_points``.
Pure derivation — no network access.

Usage
-----
  python scripts/build_fundamental_ratios.py [--db-path PATH]
         [--symbols AAPL MSFT ...] [--source TAG] [--basis ttm]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.fundamental_ratios import (
    DEFAULT_BASIS,
    DEFAULT_SOURCE,
    FundamentalRatiosDataset,
    FundamentalRatiosOptions,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build derived point-in-time financial ratios.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument(
        "--symbols",
        nargs="*",
        default=None,
        help="Restrict ratios to these primary symbols (default: all securities with fundamentals).",
    )
    parser.add_argument("--source", default=DEFAULT_SOURCE, help="Output source tag for the ratio rows.")
    parser.add_argument("--basis", default=DEFAULT_BASIS, help="Input basis (currently 'ttm').")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    symbols = tuple(args.symbols) if args.symbols else None
    options = FundamentalRatiosOptions(source=args.source, basis=args.basis, symbols=symbols)
    with DuckDBStore(args.db_path) as store:
        result = FundamentalRatiosDataset().run(store, options)
    print(
        json.dumps(
            {
                "step": "fundamental_ratios",
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
