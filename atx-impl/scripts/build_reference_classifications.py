#!/usr/bin/env python
"""Build the industry-classification taxonomy and PIT entity classification rows.

Runs four datasets in dependency order:
  1. sic_taxonomy        - SIC division/major-group hierarchy
  2. fama_french_taxonomy - Fama-French 12 industry nodes
  3. naics_taxonomy       - NAICS 2022 2-digit sector nodes
  4. entity_classification - PIT entity-level SIC + derived FF12/NAICS rows

Usage
-----
  python scripts/build_reference_classifications.py [--db-path PATH]
         [--symbols AAPL MSFT ...] [--user-agent "email@example.com"]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore
from db.reference_classifications import (
    EntityClassificationDataset,
    EntityClassificationOptions,
    FamaFrenchTaxonomyDataset,
    FamaFrenchTaxonomyOptions,
    NaicsTaxonomyDataset,
    NaicsTaxonomyOptions,
    SicTaxonomyDataset,
    SicTaxonomyOptions,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Seed industry-classification taxonomies and PIT entity classification rows."
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument(
        "--symbols",
        nargs="*",
        default=None,
        help="Restrict entity classification to these primary symbols (default: all securities).",
    )
    parser.add_argument(
        "--sic-file",
        type=Path,
        default=None,
        help="Offline CIK->SIC CSV (cols: cik, sic[, sic_description]); skips SEC network calls.",
    )
    parser.add_argument(
        "--user-agent",
        default=EntityClassificationOptions().user_agent,
        help="HTTP User-Agent sent to SEC EDGAR (format: 'AppName email@example.com').",
    )
    parser.add_argument(
        "--request-timeout",
        type=int,
        default=EntityClassificationOptions().request_timeout,
        help="Per-request timeout in seconds for SEC EDGAR calls.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    with DuckDBStore(args.db_path) as store:
        # 1. SIC taxonomy
        r1 = SicTaxonomyDataset().run(store, SicTaxonomyOptions())
        print(json.dumps({"step": "sic_taxonomy", "rows_loaded": r1.rows_loaded}, indent=2, default=str))

        # 2. Fama-French taxonomy
        r2 = FamaFrenchTaxonomyDataset().run(store, FamaFrenchTaxonomyOptions())
        print(json.dumps({"step": "fama_french_taxonomy", "rows_loaded": r2.rows_loaded}, indent=2, default=str))

        # 3. NAICS taxonomy
        r3 = NaicsTaxonomyDataset().run(store, NaicsTaxonomyOptions())
        print(json.dumps({"step": "naics_taxonomy", "rows_loaded": r3.rows_loaded}, indent=2, default=str))

        # 4. Entity classification (SEC network calls unless symbols=None with no data)
        symbols = tuple(args.symbols) if args.symbols else None
        r4 = EntityClassificationDataset().run(
            store,
            EntityClassificationOptions(
                symbols=symbols,
                sic_file=args.sic_file,
                user_agent=args.user_agent,
                request_timeout=args.request_timeout,
            ),
        )
        print(
            json.dumps(
                {
                    "step": "entity_classification",
                    "rows_loaded": r4.rows_loaded,
                    "dataset_id": r4.dataset_id,
                    "run_id": r4.run_id,
                    "details": r4.details,
                },
                indent=2,
                default=str,
            )
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
