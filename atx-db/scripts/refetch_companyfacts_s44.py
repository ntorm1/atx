#!/usr/bin/env python
"""S44: re-fetch SEC companyfacts over the already-loaded universe with widened concepts.

The S44 code change widened ``DEFAULT_CONCEPTS`` from 16 to 31 to add the balance-sheet
and income-statement detail (current assets/liabilities, cash, inventory, receivables,
PP&E, retained earnings, noncurrent long-term debt, period-end shares, gross profit, cost
of revenue, interest expense, D&A, SG&A) that unlocks the liquidity / leverage / margin /
activity ratio families (S10 codes). Those concepts were never fetched, so this run
re-pulls companyfacts for exactly the securities already present in ``sec_company_facts``
(the ``loaded_facts`` resolver) and lets ``SecCompanyFactsDataset.load`` rebuild the
chained surfaces (concept catalog, fact revisions, statement points, periods, TTM).

Network access to SEC is used here — this is a build/smoke load, NOT a test.

Usage
-----
  python scripts/refetch_companyfacts_s44.py [--db-path PATH] [--limit N]
         [--delay 0.15] [--max-attempts 3]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.fundamentals import SecCompanyFactsDataset, SecCompanyFactsOptions


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Re-fetch companyfacts over the loaded universe (S44).")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--limit", type=int, default=None, help="Cap targets (debug/smoke).")
    parser.add_argument("--delay", type=float, default=0.15, help="Per-request throttle seconds.")
    parser.add_argument("--max-attempts", type=int, default=3, help="Retries per target on transient errors.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    options = SecCompanyFactsOptions(
        symbol_source="loaded_facts",
        symbol_limit=args.limit,
        skip_failed_targets=True,
        request_delay_seconds=args.delay,
        max_attempts=args.max_attempts,
    )
    with DuckDBStore(args.db_path) as store:
        result = SecCompanyFactsDataset().run(store, options)
    details = dict(result.details)
    # Trim the (potentially long) failed-target list for a compact summary line.
    failed = details.pop("failed_targets", [])
    print(
        json.dumps(
            {
                "step": "refetch_companyfacts_s44",
                "rows_loaded": result.rows_loaded,
                "run_id": result.run_id,
                "details": details,
                "failed_sample": failed[:10],
            },
            indent=2,
            default=str,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
