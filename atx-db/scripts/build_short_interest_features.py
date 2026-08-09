#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.short_interest_features import ShortInterestFeatureDataset, ShortInterestFeatureOptions


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build PIT FINRA short-interest feature-store rows from loaded consolidated short interest."
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--feature-set", default=ShortInterestFeatureOptions().feature_set)
    parser.add_argument("--source", default=ShortInterestFeatureOptions().source)
    parser.add_argument("--min-cross-section", type=int, default=ShortInterestFeatureOptions().min_cross_section)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with DuckDBStore(args.db_path) as store:
        result = ShortInterestFeatureDataset().run(
            store,
            ShortInterestFeatureOptions(
                feature_set=args.feature_set,
                source=args.source,
                min_cross_section=args.min_cross_section,
            ),
        )
    print(
        json.dumps(
            {
                "db_path": str(args.db_path),
                "dataset_id": result.dataset_id,
                "rows_loaded": result.rows_loaded,
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
