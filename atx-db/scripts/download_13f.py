#!/usr/bin/env python
"""SEC Form 13F data sets: load one data set into DuckDB, or (``--fetch-only``) fetch every data set to files.

``--fetch-only`` is the fetch half of X.1 (ruling C-91): every quarterly / ranged ``*_form13f.zip`` from
``--start`` (default 2013-04-01, i.e. 2013Q2) to the latest listed data set is streamed into ``--fetch-dir``
(default ``<data dir>/raw/sec-13f``) with one receipt line per zip in ``receipts.jsonl`` (url, bytes, sha256,
fetched_at, Last-Modified, ETag, required members and a CRC pass). No DuckDB connection is opened. Every
request uses the approved SEC user agent and the host-wide 5 req/s limiter (``sec_http.sec_session``). A rerun
verifies each retained zip against its receipt (size + sha256) instead of requesting it again.

Run under the memory guard from a git-archive export (rulings C-58, C-68):
``run_memory_guarded.py --job-gb 0.3 --wait-minutes 30 -- python scripts/download_13f.py --fetch-only``.
Exit 3: the host SEC limiter tripped (or its state is unreadable); nothing is requested until an operator
runs ``python -m atx_db.sec_http --clear-block``.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

EXIT_SEC_BLOCKED = 3


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Load SEC Form 13F bulk data into the atx-db DuckDB store.")
    parser.add_argument("--db-path", type=Path, default=None)
    parser.add_argument("--dataset-url", help="SEC Form 13F ZIP URL. Defaults to latest discovered SEC ZIP.")
    parser.add_argument("--cache-dir", type=Path, default=None)
    parser.add_argument(
        "--cusip",
        action="append",
        default=None,
        help="CUSIP filter. Repeat for multiple. Defaults to AAPL; use --full-holdings for all rows.",
    )
    parser.add_argument("--full-holdings", action="store_true", help="Import every INFOTABLE holding row.")
    parser.add_argument("--chunk-size", type=int, default=200_000)
    parser.add_argument("--request-timeout", type=int, default=180)
    fetch = parser.add_argument_group("fetch-only (files + receipts, no DuckDB)")
    fetch.add_argument("--fetch-only", action="store_true", help="Fetch every data set to --fetch-dir; no load.")
    fetch.add_argument("--fetch-dir", type=Path, default=None, help="Default: <data dir>/raw/sec-13f.")
    fetch.add_argument("--start", type=dt.date.fromisoformat, default=dt.date(2013, 4, 1))
    fetch.add_argument("--end", type=dt.date.fromisoformat, default=None)
    fetch.add_argument("--url", action="append", dest="urls", help="Explicit data-set URL(s) instead of discovery.")
    fetch.add_argument("--max-attempts", type=int, default=4)
    fetch.add_argument("--summary-json", type=Path, default=None)
    return parser.parse_args()


def fetch_main(args: argparse.Namespace) -> int:
    from atx_db.sec_http import SecBlockedError
    from atx_db.thirteenf_archive import fetch_archives

    started = time.perf_counter()

    def progress(index: int, total: int, record: dict[str, object]) -> None:
        print(
            json.dumps({
                "i": index,
                "n": total,
                "period": record.get("source_period"),
                "outcome": record.get("outcome"),
                "bytes": record.get("bytes"),
                "s": record.get("download_s", record.get("verify_s")),
                "elapsed_s": round(time.perf_counter() - started, 1),
            }),
            flush=True,
        )

    try:
        summary = fetch_archives(
            fetch_dir=args.fetch_dir,
            start=args.start,
            end=args.end,
            urls=args.urls,
            timeout=args.request_timeout,
            max_attempts=args.max_attempts,
            progress=progress,
        )
    except SecBlockedError as exc:
        print(json.dumps({"stopped": "sec_blocked", "error": str(exc)[:300]}), flush=True)
        return EXIT_SEC_BLOCKED
    summary["elapsed_s"] = round(time.perf_counter() - started, 1)
    text = json.dumps(summary, indent=2, default=str)
    if args.summary_json is not None:
        args.summary_json.parent.mkdir(parents=True, exist_ok=True)
        args.summary_json.write_text(text, encoding="utf-8")
    print(text)
    return 0 if not summary["not_ok"] else 1


def load_main(args: argparse.Namespace) -> int:
    from atx_db import DEFAULT_DB_PATH, DuckDBStore, ThirteenFDataSet, ThirteenFOptions
    from atx_db.thirteenf import AAPL_CUSIP, normalize_cusip

    if args.full_holdings:
        cusips = None
    else:
        cusips = tuple(normalize_cusip(value) for value in (args.cusip or [AAPL_CUSIP]))
    options = ThirteenFOptions(
        dataset_url=args.dataset_url,
        cache_dir=args.cache_dir or ThirteenFOptions().cache_dir,
        cusips=cusips,
        chunk_size=args.chunk_size,
        request_timeout=args.request_timeout,
    )
    with DuckDBStore(args.db_path or DEFAULT_DB_PATH) as store:
        result = ThirteenFDataSet().run(store, options)
    print(json.dumps(result.details | {"rows_loaded": result.rows_loaded, "source": result.source}, indent=2))
    return 0


def main() -> int:
    args = parse_args()
    if args.fetch_only:
        return fetch_main(args)
    return load_main(args)


if __name__ == "__main__":
    raise SystemExit(main())
