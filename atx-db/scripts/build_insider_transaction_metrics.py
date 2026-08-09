#!/usr/bin/env python
"""Materialize derived Section 16 insider transaction analytics.

The script is offline: it reads already-loaded ``insider_transaction`` rows and
writes ``insider_transaction_metrics``. Load Form 3/4/5 XML first with
``scripts/build_insider_ownership.py --source-files ...`` or an approved SEC
submissions-backed job, then run this materializer.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.insider_metrics import InsiderTransactionMetricsDataset, InsiderTransactionMetricsOptions
from atx_db.warehouse import symbol_key


def parse_csv(value: str | None) -> tuple[str, ...] | None:
    if not value:
        return None
    parsed = tuple(part.strip() for part in value.split(",") if part.strip())
    return parsed or None


def parse_symbols(value: str | None) -> tuple[str, ...] | None:
    parsed = parse_csv(value)
    if parsed is None:
        return None
    out = tuple(symbol_key(part) for part in parsed if symbol_key(part))
    return out or None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build insider transaction signal metrics.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--source", default=InsiderTransactionMetricsOptions().source)
    parser.add_argument("--input-source", default=InsiderTransactionMetricsOptions().input_source)
    parser.add_argument("--window-days", type=int, default=InsiderTransactionMetricsOptions().window_days)
    parser.add_argument("--cluster-min-buyers", type=int, default=InsiderTransactionMetricsOptions().cluster_min_buyers)
    parser.add_argument(
        "--cluster-min-purchase-value",
        type=float,
        default=InsiderTransactionMetricsOptions().cluster_min_purchase_value,
    )
    parser.add_argument("--security-ids", help="Comma-separated warehouse security_id filter.")
    parser.add_argument("--symbols", help="Comma-separated issuer ticker filter.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    options = InsiderTransactionMetricsOptions(
        source=args.source,
        input_source=args.input_source,
        window_days=args.window_days,
        cluster_min_buyers=args.cluster_min_buyers,
        cluster_min_purchase_value=args.cluster_min_purchase_value,
        security_ids=parse_csv(args.security_ids),
        symbols=parse_symbols(args.symbols),
    )
    with DuckDBStore(args.db_path) as store:
        result = InsiderTransactionMetricsDataset().run(store, options)
    print(
        json.dumps(
            {
                "db_path": str(args.db_path),
                "dataset_id": result.dataset_id,
                "rows_materialized": result.rows_loaded,
                "source": result.source,
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
