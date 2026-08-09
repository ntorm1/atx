#!/usr/bin/env python
"""Materialize the derived ``macro_metrics`` surface from cached FRED observations.

Computes per-series macro analytics (level, change vs prior observation, year-over-year
change/growth, expanding z-score) plus synthetic 10Y-2Y Treasury term-spread and real fed funds
from the cached ``macro_observations`` feed. Pure derivation — no network access.

Usage
-----
  python scripts/build_macro_metrics.py [--db-path PATH]
         [--series-ids DGS10 DGS2 ...] [--source TAG]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.macro_metrics import DEFAULT_SOURCE, MacroMetricsDataset, MacroMetricsOptions


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build derived macro analytics.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument(
        "--series-ids",
        nargs="*",
        default=None,
        help="Restrict metrics to these FRED series ids (default: all cached series).",
    )
    parser.add_argument("--source", default=DEFAULT_SOURCE, help="Output source tag for the metric rows.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    series = tuple(args.series_ids) if args.series_ids else None
    options = MacroMetricsOptions(source=args.source, series_ids=series)
    with DuckDBStore(args.db_path) as store:
        result = MacroMetricsDataset().run(store, options)
    print(
        json.dumps(
            {
                "step": "macro_metrics",
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
