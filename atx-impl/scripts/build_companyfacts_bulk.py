#!/usr/bin/env python
"""S45: backfill SEC companyfacts from the local bulk archive (companyfacts.zip).

SEC publishes the entire XBRL company-facts universe as one nightly archive
(~1.4 GB) at
``https://www.sec.gov/Archives/edgar/daily-index/xbrl/companyfacts.zip``,
one ``CIK##########.json`` per filer — byte-identical to the per-CIK
``data.sec.gov/.../companyfacts/CIK*.json`` endpoint. Backfilling from the local
zip replaces N throttled network round trips with one download plus N lazy member
reads and drops the per-CIK 404/throttle/retry burden.

The archive is a one-time operator download — supply it with ``--companyfacts-zip``,
or pass ``--download`` to stream it to that path first. Reading the zip is offline;
``SecCompanyFactsDataset.load`` then rebuilds the chained surfaces (concept catalog,
fact revisions, statement points, periods, TTM) exactly as the network path does.

Usage
-----
  # operator already downloaded the archive:
  python scripts/build_companyfacts_bulk.py --companyfacts-zip C:\\data\\companyfacts.zip

  # let the tool download it first (build/smoke — never in tests):
  python scripts/build_companyfacts_bulk.py --companyfacts-zip C:\\data\\companyfacts.zip --download

  # restrict the target set (default: the already-loaded universe):
  python scripts/build_companyfacts_bulk.py --companyfacts-zip <zip> --symbol-source universe --limit 500
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore
from db.fundamentals import SecCompanyFactsDataset, SecCompanyFactsOptions
from db.security_master import SEC_USER_AGENT, sec_session

COMPANYFACTS_ZIP_URL = "https://www.sec.gov/Archives/edgar/daily-index/xbrl/companyfacts.zip"


def _download(dest: Path, *, user_agent: str) -> None:
    """Stream the bulk archive to ``dest`` (network — build/smoke only, never tests)."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    session = sec_session(user_agent)
    with session.get(COMPANYFACTS_ZIP_URL, stream=True, timeout=600) as resp:
        resp.raise_for_status()
        written = 0
        with open(dest, "wb") as fh:
            for chunk in resp.iter_content(chunk_size=1 << 20):
                if chunk:
                    fh.write(chunk)
                    written += len(chunk)
        print(f"downloaded {written / 1e9:.2f} GB -> {dest}", file=sys.stderr)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Backfill companyfacts from the SEC bulk zip (S45).")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--companyfacts-zip", type=Path, required=True, help="Path to companyfacts.zip.")
    parser.add_argument("--download", action="store_true", help="Stream the archive to --companyfacts-zip first.")
    parser.add_argument(
        "--symbol-source",
        default="loaded_facts",
        help="Target resolver: loaded_facts (default) | universe | sec_company_tickers | symbols.",
    )
    parser.add_argument("--universe-id", default=None)
    parser.add_argument("--limit", type=int, default=None, help="Cap targets (debug/smoke).")
    parser.add_argument("--user-agent", default=SEC_USER_AGENT)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.download:
        _download(args.companyfacts_zip, user_agent=args.user_agent)
    if not args.companyfacts_zip.exists():
        raise SystemExit(f"companyfacts.zip not found at {args.companyfacts_zip}; pass --download to fetch it.")

    options = SecCompanyFactsOptions(
        companyfacts_zip=args.companyfacts_zip,
        symbol_source=args.symbol_source,
        symbol_limit=args.limit,
        universe_id=args.universe_id,
        skip_failed_targets=True,  # CIKs absent from the archive are recorded + skipped
    )
    with DuckDBStore(args.db_path) as store:
        result = SecCompanyFactsDataset().run(store, options)
    details = dict(result.details)
    details.pop("failed_targets", None)
    print(
        json.dumps(
            {"step": "build_companyfacts_bulk", "rows_loaded": result.rows_loaded,
             "run_id": result.run_id, "details": details},
            indent=2, default=str,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
