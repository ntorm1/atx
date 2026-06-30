#!/usr/bin/env python
"""Materialize derived security listing / reference-compliance analytics.

The script is offline: it reads already-loaded ``listing_status_intervals`` and
``nasdaq_symbol_directory`` rows and writes ``security_listing_metrics``. Load
the symbol directory and the listing-status spine first
(``scripts/build_security_master.py`` / the ``listing_status_intervals`` job),
then run this materializer.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore
from db.listing_metrics import SecurityListingMetricsDataset, SecurityListingMetricsOptions
from db.warehouse import symbol_key


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
    parser = argparse.ArgumentParser(description="Build security listing reference metrics.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--source", default=SecurityListingMetricsOptions().source)
    parser.add_argument("--input-listing-source", default=SecurityListingMetricsOptions().input_listing_source)
    parser.add_argument("--security-ids", help="Comma-separated warehouse security_id filter.")
    parser.add_argument("--symbols", help="Comma-separated ticker filter.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    options = SecurityListingMetricsOptions(
        source=args.source,
        input_listing_source=args.input_listing_source,
        security_ids=parse_csv(args.security_ids),
        symbols=parse_symbols(args.symbols),
    )
    with DuckDBStore(args.db_path) as store:
        result = SecurityListingMetricsDataset().run(store, options)
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
