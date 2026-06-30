#!/usr/bin/env python
"""Materialize derived quality reports for off-exchange and short-flow surfaces.

This script is fully offline. It summarizes rows already loaded into
``offexchange_volume`` and ``finra_short_volume``/``short_volume_metrics`` into
the control table ``offexchange_quality_report``.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore
from db.offexchange_quality import OffExchangeQualityReportDataset, OffExchangeQualityReportOptions


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build off-exchange quality report surfaces.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--source", default=OffExchangeQualityReportOptions().source)
    parser.add_argument("--offexchange-source", default=OffExchangeQualityReportOptions().offexchange_source)
    parser.add_argument("--short-volume-source", default=OffExchangeQualityReportOptions().short_volume_source)
    parser.add_argument("--skip-offexchange", action="store_true", help="Do not summarize offexchange_volume.")
    parser.add_argument("--skip-short-volume", action="store_true", help="Do not summarize FINRA daily short-volume rows.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    options = OffExchangeQualityReportOptions(
        source=args.source,
        offexchange_source=args.offexchange_source,
        short_volume_source=args.short_volume_source,
        include_offexchange=not args.skip_offexchange,
        include_short_volume=not args.skip_short_volume,
    )
    with DuckDBStore(args.db_path) as store:
        result = OffExchangeQualityReportDataset().run(store, options)
    print(
        json.dumps(
            {
                "offexchange_quality_report": {
                    "rows_materialized": result.rows_loaded,
                    "run_id": result.run_id,
                    "source": result.source,
                }
            },
            indent=2,
            default=str,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
