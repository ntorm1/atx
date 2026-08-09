#!/usr/bin/env python
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, SHORT_INTEREST_WITH_13F_SQL, short_interest_with_13f_positioning


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Query recent short interest with latest 13F positioning.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--symbol", default="AAPL")
    parser.add_argument("--periods", type=int, default=10)
    parser.add_argument("--show-sql", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.show_sql:
        print(SHORT_INTEREST_WITH_13F_SQL.strip())
        print()
    frame = short_interest_with_13f_positioning(args.symbol, args.periods, args.db_path)
    print(frame.to_string(index=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
