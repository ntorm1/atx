"""Load SEC Item 2.02 EX-99 evidence for already-loaded submissions.

With ``--fetch-dir`` the run is loader-only: bytes come from the fetch store written by
``scripts/fetch_sec_earnings_releases.py`` and no SEC request is made. Without it the
legacy path fetches from SEC while holding the warehouse writer.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path

from atx_db.connection import DuckDBStore
from atx_db.press_release import SecEarningsReleaseOptions, refresh_sec_earnings_release_facts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, required=True)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--history-start", type=dt.date.fromisoformat)
    parser.add_argument("--history-end", type=dt.date.fromisoformat)
    parser.add_argument("--cik", action="append", default=[])
    parser.add_argument("--request-timeout", type=float, default=30.0)
    parser.add_argument("--max-index-bytes", type=int, default=2_000_000)
    parser.add_argument("--max-document-bytes", type=int, default=8_000_000)
    parser.add_argument("--fetch-dir", type=Path, default=None,
                        help="Loader-only: read the sec_http fetch store instead of the network.")
    parser.add_argument("--run-id", default=None)
    args = parser.parse_args()
    with DuckDBStore(args.db_path) as store:
        result = refresh_sec_earnings_release_facts(
            store,
            SecEarningsReleaseOptions(
                cache_dir=args.cache_dir,
                history_start=args.history_start,
                history_end=args.history_end,
                ciks=tuple(args.cik) or None,
                request_timeout=args.request_timeout,
                max_index_bytes=args.max_index_bytes,
                max_document_bytes=args.max_document_bytes,
                fetch_dir=args.fetch_dir,
                run_id=args.run_id,
            ),
        )
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
