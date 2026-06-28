#!/usr/bin/env python
"""Build the estimates store: measure seed, actuals, surprise, and optional injectable loaders.

Runs datasets in dependency order:
  1. est_measure   — static measure dimension seed (5 rows)
  2. est_actual    — REAL: maps sec_company_facts XBRL facts to measure codes
  3. est_surprise  — REAL derived: SUE via seasonal-random-walk-with-drift (Foster-Olsen-Shevlin 1984)
  4. est_consensus — injectable; default-empty (skip unless --run-injectable)
  5. est_guidance  — injectable; default-empty (skip unless --run-injectable)
  6. est_recommendation — injectable; default-empty (skip unless --run-injectable)

Usage
-----
  python scripts/build_estimates.py [--db-path PATH]
         [--measure-codes EPS_DILUTED REVENUE ...]
         [--security-ids sec_0001 sec_0002 ...]
         [--window 8] [--min-obs 4]
         [--run-injectable]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore
from db.estimates import (
    EstimateActualsDataset,
    EstimateActualsOptions,
    EstimateConsensusDataset,
    EstimateConsensusOptions,
    EstimateGuidanceDataset,
    EstimateGuidanceOptions,
    EstimateMeasureSeedDataset,
    EstimateMeasureSeedOptions,
    EstimateRecommendationDataset,
    EstimateRecommendationOptions,
    EstimateSurpriseDataset,
    EstimateSurpriseOptions,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Seed estimates store: measure dimension, actuals, and SUE surprise."
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument(
        "--measure-codes",
        nargs="*",
        default=None,
        help="Restrict actuals/surprise to these measure codes (default: all).",
    )
    parser.add_argument(
        "--security-ids",
        nargs="*",
        default=None,
        help="Restrict actuals to these security IDs (default: all).",
    )
    parser.add_argument(
        "--window",
        type=int,
        default=8,
        help="SUE trailing seasonal-diff window (default: 8).",
    )
    parser.add_argument(
        "--min-obs",
        type=int,
        default=4,
        help="Minimum seasonal diffs required to compute SUE (default: 4).",
    )
    parser.add_argument(
        "--run-injectable",
        action="store_true",
        default=False,
        help="Also run injectable loaders (consensus/guidance/recommendation) — no-ops without providers.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    measure_codes = tuple(args.measure_codes) if args.measure_codes else None
    security_ids = tuple(args.security_ids) if args.security_ids else None

    with DuckDBStore(args.db_path) as store:
        # 1. Measure seed
        r1 = EstimateMeasureSeedDataset().run(store, EstimateMeasureSeedOptions())
        print(json.dumps({"step": "est_measure", "rows_loaded": r1.rows_loaded}, indent=2, default=str))

        # 2. Actuals (reads sec_company_facts)
        r2 = EstimateActualsDataset().run(
            store,
            EstimateActualsOptions(
                measure_codes=measure_codes,
                security_ids=security_ids,
            ),
        )
        print(
            json.dumps(
                {
                    "step": "est_actual",
                    "rows_loaded": r2.rows_loaded,
                    "details": r2.details,
                },
                indent=2,
                default=str,
            )
        )

        # 3. SUE surprise (derived from est_actual)
        r3 = EstimateSurpriseDataset().run(
            store,
            EstimateSurpriseOptions(
                measure_codes=measure_codes,
                window=args.window,
                min_obs=args.min_obs,
            ),
        )
        print(
            json.dumps(
                {
                    "step": "est_surprise",
                    "rows_loaded": r3.rows_loaded,
                    "details": r3.details,
                },
                indent=2,
                default=str,
            )
        )

        if args.run_injectable:
            # 4-6. Injectable loaders (default-empty without providers)
            r4 = EstimateConsensusDataset().run(store, EstimateConsensusOptions())
            print(json.dumps({"step": "est_consensus", "rows_loaded": r4.rows_loaded}, indent=2, default=str))

            r5 = EstimateGuidanceDataset().run(store, EstimateGuidanceOptions())
            print(json.dumps({"step": "est_guidance", "rows_loaded": r5.rows_loaded}, indent=2, default=str))

            r6 = EstimateRecommendationDataset().run(store, EstimateRecommendationOptions())
            print(json.dumps({"step": "est_recommendation", "rows_loaded": r6.rows_loaded}, indent=2, default=str))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
