#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.xbrl_filing_contexts import XbrlFilingContextDataset, XbrlFilingContextOptions


def split_csv(value: str | None) -> tuple[str, ...]:
    if value in (None, ""):
        return ()
    return tuple(part.strip() for part in value.replace(",", " ").split() if part.strip())


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Load SEC inline XBRL filing contexts and dimensions.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--symbols", default="AAPL")
    parser.add_argument("--forms", default="10-K,10-Q")
    parser.add_argument("--accession-numbers")
    parser.add_argument("--max-filings", type=int, default=3)
    parser.add_argument(
        "--max-filings-per-symbol",
        type=int,
        help="Optional balanced cap applied within each security before the overall --max-filings cap.",
    )
    parser.add_argument("--request-timeout", type=int, default=120)
    parser.add_argument("--user-agent", default="atx-db XBRL filing context loader nathan.tormaschy@gmail.com")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.max_filings < 1:
        raise ValueError("--max-filings must be positive")
    if args.max_filings_per_symbol is not None and args.max_filings_per_symbol < 1:
        raise ValueError("--max-filings-per-symbol must be positive")
    options = XbrlFilingContextOptions(
        symbols=tuple(symbol.upper() for symbol in split_csv(args.symbols)),
        forms=tuple(form.upper() for form in split_csv(args.forms)),
        accession_numbers=split_csv(args.accession_numbers) or None,
        max_filings=args.max_filings,
        max_filings_per_symbol=args.max_filings_per_symbol,
        request_timeout=args.request_timeout,
        user_agent=args.user_agent,
    )
    with DuckDBStore(args.db_path) as store:
        result = XbrlFilingContextDataset().run(store, options)
    print(json.dumps({"run_id": result.run_id, "rows_loaded": result.rows_loaded, "details": result.details}, indent=2, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
