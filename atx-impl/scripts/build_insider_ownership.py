#!/usr/bin/env python
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore
from db.insider_ownership import DEFAULT_FORMS, InsiderOwnershipDataset, InsiderOwnershipOptions
from db.warehouse import symbol_key


def parse_csv(value: str | None) -> tuple[str, ...] | None:
    if not value:
        return None
    return tuple(part.strip() for part in value.split(",") if part.strip())


def parse_paths(value: str | None) -> tuple[Path, ...] | None:
    parsed = parse_csv(value)
    if parsed is None:
        return None
    return tuple(Path(part) for part in parsed)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Load SEC Section 16 ownership XML into insider ownership warehouse tables."
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--symbols", default="AAPL")
    parser.add_argument("--forms", default=",".join(DEFAULT_FORMS))
    parser.add_argument("--accession-numbers")
    parser.add_argument("--source-urls")
    parser.add_argument("--source-files")
    parser.add_argument("--max-filings", type=int, default=InsiderOwnershipOptions().max_filings)
    parser.add_argument("--request-timeout", type=int, default=InsiderOwnershipOptions().request_timeout)
    parser.add_argument("--user-agent", default=InsiderOwnershipOptions().user_agent)
    parser.add_argument("--source", default=InsiderOwnershipOptions().source)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    symbols = tuple(symbol_key(part) for part in args.symbols.split(",") if symbol_key(part))
    with DuckDBStore(args.db_path) as store:
        result = InsiderOwnershipDataset().run(
            store,
            InsiderOwnershipOptions(
                symbols=symbols,
                forms=parse_csv(args.forms) or DEFAULT_FORMS,
                accession_numbers=parse_csv(args.accession_numbers),
                source_urls=parse_csv(args.source_urls),
                source_files=parse_paths(args.source_files),
                max_filings=args.max_filings,
                request_timeout=args.request_timeout,
                user_agent=args.user_agent,
                source=args.source,
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
