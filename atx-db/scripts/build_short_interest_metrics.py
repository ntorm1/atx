#!/usr/bin/env python
"""Materialize the derived ``short_interest_metrics`` surface from cached FINRA data.

Computes per-security short-interest analytics (short-interest change, recomputed
days-to-cover, short percent of point-in-time shares outstanding, and the
cross-sectional percentile of days-to-cover / short-interest change within each
settlement cohort) from the cached ``finra_short_interest`` feed. Pure derivation —
no network access.

Usage
-----
  python scripts/build_short_interest_metrics.py [--db-path PATH]
         [--symbols AAPL GME ...] [--source TAG]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.short_interest_metrics import (
    DEFAULT_SOURCE,
    ShortInterestMetricsDataset,
    ShortInterestMetricsOptions,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build derived FINRA short-interest analytics.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument(
        "--symbols",
        nargs="*",
        default=None,
        help="Restrict metrics to these symbols (default: all securities in the FINRA feed).",
    )
    parser.add_argument("--source", default=DEFAULT_SOURCE, help="Output source tag for the metric rows.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    symbols = tuple(args.symbols) if args.symbols else None
    options = ShortInterestMetricsOptions(source=args.source, symbols=symbols)
    with DuckDBStore(args.db_path) as store:
        result = ShortInterestMetricsDataset().run(store, options)
    print(
        json.dumps(
            {
                "step": "short_interest_metrics",
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
