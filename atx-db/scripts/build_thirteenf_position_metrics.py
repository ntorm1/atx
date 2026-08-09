#!/usr/bin/env python
"""Materialize the derived ``thirteenf_position_metrics`` surface from cached 13F data.

Computes per-manager institutional position analytics (quarter-over-quarter
common-share change, NEW/ADDED/TRIMMED/UNCHANGED/EXITED action, voting-authority
concentration) from the cached ``thirteenf_security_positions`` + ``thirteenf_manager_reports``
feed. Complements the issuer-level ``thirteenf_security_ownership`` rollup with the
per-manager conviction layer. Pure derivation — no network access.

Usage
-----
  python scripts/build_thirteenf_position_metrics.py [--db-path PATH]
         [--symbols AAPL ...] [--source TAG]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.thirteenf_position_metrics import (
    DEFAULT_SOURCE,
    ThirteenFPositionMetricsDataset,
    ThirteenFPositionMetricsOptions,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build derived 13F manager-level position analytics.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument(
        "--symbols",
        nargs="*",
        default=None,
        help="Restrict metrics to these issuer symbols (default: all securities in the 13F feed).",
    )
    parser.add_argument("--source", default=DEFAULT_SOURCE, help="Output source tag for the metric rows.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    symbols = tuple(args.symbols) if args.symbols else None
    options = ThirteenFPositionMetricsOptions(source=args.source, symbols=symbols)
    with DuckDBStore(args.db_path) as store:
        result = ThirteenFPositionMetricsDataset().run(store, options)
    print(
        json.dumps(
            {
                "step": "thirteenf_position_metrics",
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
