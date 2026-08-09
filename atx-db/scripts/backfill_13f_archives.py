from __future__ import annotations

import argparse
import datetime as dt
import json
import uuid
from pathlib import Path

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.thirteenf_archive import (
    ThirteenFArchiveBackfillOptions,
    archive_backfill_result,
    backfill_archives,
)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Backfill SEC Form 13F quarterly archives with native DuckDB ingestion")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--start", type=dt.date.fromisoformat, default=dt.date(2013, 4, 1))
    parser.add_argument("--end", type=dt.date.fromisoformat)
    parser.add_argument("--url", action="append", dest="urls")
    parser.add_argument("--cache-dir", type=Path)
    parser.add_argument("--extract-dir", type=Path)
    parser.add_argument("--keep-extracted", action="store_true")
    parser.add_argument("--no-hash", action="store_true")
    parser.add_argument("--continue-on-error", action="store_true")
    parser.add_argument("--user-agent")
    parser.add_argument("--run-id")
    parser.add_argument("--replace-loaded", action="store_true")
    parser.add_argument("--keep-indexes-during-load", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    defaults = ThirteenFArchiveBackfillOptions()
    options = ThirteenFArchiveBackfillOptions(
        start=args.start,
        end=args.end,
        urls=None if args.urls is None else tuple(args.urls),
        cache_dir=args.cache_dir or defaults.cache_dir,
        extract_dir=args.extract_dir or defaults.extract_dir,
        keep_extracted=args.keep_extracted,
        compute_source_hash=not args.no_hash,
        user_agent=args.user_agent or defaults.user_agent,
        run_id=args.run_id or f"sec-13f-archive-{uuid.uuid4()}",
        fail_fast=not args.continue_on_error,
        replace_loaded_archives=args.replace_loaded,
        defer_indexes=not args.keep_indexes_during_load,
    )
    with DuckDBStore(args.db_path) as store:
        results = backfill_archives(store, options)
    summary = archive_backfill_result(results)
    print(json.dumps({"dataset_id": summary.dataset_id, "rows_loaded": summary.rows_loaded, **summary.details}, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
