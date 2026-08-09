#!/usr/bin/env python
"""Extract consolidated inline-XBRL canonical metrics from cached filing facts.

Pulls entity-level (non-segment) us-gaap facts out of ``xbrl_filing_facts`` into
``fundamental_xbrl_metric`` (current assets/liabilities, cash, inventory, ...), the
balance detail the narrow companyfacts feed omits. Pure derivation over already-cached
data — no network. Run this before ``build_fundamental_ratios.py`` so the liquidity
ratios pick up the new metrics.

Usage
-----
  python scripts/build_fundamental_xbrl_metrics.py [--db-path PATH]
         [--symbols AAPL MSFT ...] [--source TAG]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.fundamental_xbrl_metrics import (
    DEFAULT_SOURCE,
    FundamentalXbrlMetricDataset,
    FundamentalXbrlMetricOptions,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Extract consolidated inline-XBRL canonical metrics.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--symbols", nargs="*", default=None, help="Restrict to these primary symbols.")
    parser.add_argument("--source", default=DEFAULT_SOURCE, help="Output source tag.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    symbols = tuple(args.symbols) if args.symbols else None
    options = FundamentalXbrlMetricOptions(source=args.source, symbols=symbols)
    with DuckDBStore(args.db_path) as store:
        result = FundamentalXbrlMetricDataset().run(store, options)
    print(
        json.dumps(
            {
                "step": "fundamental_xbrl_metric",
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
