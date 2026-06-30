#!/usr/bin/env python
"""Materialize the derived ``thirteenf_option_metrics`` surface from cached 13F data.

Computes issuer/report-period call and put option-positioning aggregates from
cached ``thirteenf_security_positions`` rows: put/call share and value ratios,
common-share denominators, top option managers, option-bias labels, and QoQ
net-call changes. Pure derivation, no network access.

Usage
-----
  python scripts/build_thirteenf_option_metrics.py [--db-path PATH]
         [--symbols AAPL ...] [--source TAG]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore
from db.thirteenf_option_metrics import (
    DEFAULT_SOURCE,
    ThirteenFOptionMetricsDataset,
    ThirteenFOptionMetricsOptions,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build derived 13F issuer-level option analytics.")
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
    options = ThirteenFOptionMetricsOptions(source=args.source, symbols=symbols)
    with DuckDBStore(args.db_path) as store:
        result = ThirteenFOptionMetricsDataset().run(store, options)
    print(
        json.dumps(
            {
                "step": "thirteenf_option_metrics",
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
