#!/usr/bin/env python
"""Materialize derived corporate-action analytics surfaces.

Computes cash-dividend analytics from ``corporate_actions`` joined to ex-date bars,
and split-event reconciliation metrics from ``adjustment_factor_history`` joined to
``daily_adjustment_factors``. Pure derivation; no network.

Usage
-----
  python scripts/build_corporate_action_metrics.py [--db-path PATH]
         [--surface all|dividends|splits] [--symbols AAPL MSFT ...]
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
    DEFAULT_SPLIT_SOURCE,
    CorporateActionDividendMetricsDataset,
    CorporateActionDividendMetricsOptions,
    CorporateActionSplitMetricsDataset,
    CorporateActionSplitMetricsOptions,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build derived corporate-action analytics.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument(
        "--surface",
        choices=("all", "dividends", "splits"),
        default="all",
        help="Metric surface to build (default: all).",
    )
    parser.add_argument(
        "--symbols",
        nargs="*",
        default=None,
        help="Restrict metrics to these symbols (default: all securities with corporate actions).",
    )
    parser.add_argument("--source", default=DEFAULT_SOURCE, help="Output source tag for dividend metric rows.")
    parser.add_argument(
        "--split-source",
        default=DEFAULT_SPLIT_SOURCE,
        help="Output source tag for split metric rows.",
    )
    parser.add_argument("--factor-source", default=None, help="Adjustment-factor source for split metrics.")
    parser.add_argument("--daily-adjustment-source", default=None, help="Daily-adjustment source for split metrics.")
    parser.add_argument("--bar-source", default=None, help="Optional bar source for split metric reconciliation.")
    return parser.parse_args()


def _result_payload(step: str, result) -> dict[str, object]:
    return {
        "step": step,
        "rows_loaded": result.rows_loaded,
        "dataset_id": result.dataset_id,
        "run_id": result.run_id,
        "details": result.details,
    }


def main() -> int:
    args = parse_args()
    symbols = tuple(args.symbols) if args.symbols else None
    outputs: list[dict[str, object]] = []
    with DuckDBStore(args.db_path) as store:
        if args.surface in ("all", "dividends"):
            options = CorporateActionDividendMetricsOptions(source=args.source, symbols=symbols)
            result = CorporateActionDividendMetricsDataset().run(store, options)
            outputs.append(_result_payload("corporate_action_dividend_metrics", result))
        if args.surface in ("all", "splits"):
            default_split_options = CorporateActionSplitMetricsOptions()
            split_options = CorporateActionSplitMetricsOptions(
                source=args.split_source,
                factor_source=args.factor_source or default_split_options.factor_source,
                daily_adjustment_source=args.daily_adjustment_source or default_split_options.daily_adjustment_source,
                bar_source=args.bar_source,
                symbols=symbols,
            )
            result = CorporateActionSplitMetricsDataset().run(store, split_options)
            outputs.append(_result_payload("corporate_action_split_metrics", result))
    print(json.dumps({"steps": outputs}, indent=2, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
