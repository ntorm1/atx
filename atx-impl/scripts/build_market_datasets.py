#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore, FinraShortInterestDataset, FinraShortInterestOptions
from db import ThirteenFDataSet, ThirteenFOptions
from db.thirteenf import AAPL_CUSIP, normalize_cusip


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build the atx-impl market DuckDB with FINRA short interest and SEC 13F data."
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--symbol", default="AAPL")
    parser.add_argument("--cusip", default=AAPL_CUSIP)
    parser.add_argument("--full-13f-holdings", action="store_true")
    parser.add_argument("--sec-dataset-url")
    parser.add_argument("--cache-dir", type=Path, default=ThirteenFOptions().cache_dir)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with DuckDBStore(args.db_path) as store:
        finra = FinraShortInterestDataset().run(
            store,
            FinraShortInterestOptions(symbol=args.symbol),
        )
        thirteenf = ThirteenFDataSet().run(
            store,
            ThirteenFOptions(
                dataset_url=args.sec_dataset_url,
                cache_dir=args.cache_dir,
                cusips=None if args.full_13f_holdings else (normalize_cusip(args.cusip),),
            ),
        )
    print(
        json.dumps(
            {
                "db_path": str(args.db_path),
                "finra": finra.details | {"rows_loaded": finra.rows_loaded, "source": finra.source},
                "thirteenf": thirteenf.details | {"rows_loaded": thirteenf.rows_loaded, "source": thirteenf.source},
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
