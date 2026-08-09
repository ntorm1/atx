#!/usr/bin/env python
"""Load FINRA OTC Transparency off-exchange volume and materialize the ATS-share rollup.

Ingests an injectable FINRA ``weeklySummary``-shaped or normalized CSV into
``offexchange_volume`` (per security/venue/period), upserts the ``offexchange_venue``
MPID dimension, then materializes the per (security, period) ATS-share rollup into
``offexchange_security_period``. No network access in this script — the FINRA Query
API (OAuth2, tiered publication delay) is the production source; tests and this CLI
use a local file.

Usage
-----
  python scripts/build_offexchange.py --db-path PATH --source-file PATH \
         [--period-type weekly|monthly|block] [--tier T1] [--no-replace]
  python scripts/build_offexchange.py --db-path PATH --materialize-only \
         [--period-type weekly]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.offexchange import (
    FinraOffExchangeDataset,
    FinraOffExchangeOptions,
    OffExchangeSecurityPeriodDataset,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build FINRA off-exchange transparency surfaces.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--source-file", type=Path, default=None, help="FINRA weeklySummary-shaped or normalized CSV.")
    parser.add_argument("--source", default=FinraOffExchangeOptions().source)
    parser.add_argument("--period-type", default="weekly", choices=["weekly", "monthly", "block"])
    parser.add_argument("--tier", default=None)
    parser.add_argument("--no-replace", action="store_true", help="Append rows for this source file instead of replacing.")
    parser.add_argument("--materialize-only", action="store_true", help="Only rebuild the security-period rollup.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    options = FinraOffExchangeOptions(
        source_file=args.source_file,
        source=args.source,
        period_type=args.period_type,
        tier=args.tier,
        replace_source_file=not args.no_replace,
    )
    out = {}
    with DuckDBStore(args.db_path) as store:
        if not args.materialize_only:
            vol = FinraOffExchangeDataset().run(store, options)
            out["offexchange_volume"] = {"rows_loaded": vol.rows_loaded, "run_id": vol.run_id}
        roll = OffExchangeSecurityPeriodDataset().run(store, options)
        out["offexchange_security_period"] = {"rows_materialized": roll.rows_loaded, "run_id": roll.run_id}
    print(json.dumps(out, indent=2, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
