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
fact revisions, statement points, periods, TTM), unless ``--defer-derived-surfaces``
leaves those for the subsequent activation run.

``--symbol-source archive_members`` discovers every exact local CIK JSON member,
independently of current tickers. It cannot download or use HTTP. Replacement is
the default; ``--append-missing`` skips CIKs with any facts and is not completion
evidence for a new archive or concept allowlist. There are no persisted completion
receipts: a full replacement rerun rereads all selected members. CIK-only identities
do not establish historical traded-US-equity membership.

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
import datetime as dt
import json
import logging
import sys
from collections.abc import Sequence
from pathlib import Path

import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.cli import _configure_analytical_session
from atx_db.clock import utc_today
from atx_db.fundamentals import COMPANY_FACT_SYMBOL_SOURCES, SecCompanyFactsDataset, SecCompanyFactsOptions
from atx_db.security_master import SEC_USER_AGENT, sec_session

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


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Backfill companyfacts from the SEC bulk zip (S45).")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--memory-limit", default="1GB", help="DuckDB query memory bound (default: 1GB).")
    parser.add_argument("--threads", type=int, default=1, help="DuckDB query threads (default: 1).")
    parser.add_argument("--companyfacts-zip", type=Path, required=True, help="Path to companyfacts.zip.")
    parser.add_argument("--download", action="store_true", help="Stream the archive to --companyfacts-zip first.")
    parser.add_argument(
        "--symbol-source",
        default="loaded_facts",
        choices=COMPANY_FACT_SYMBOL_SOURCES,
        help="archive_members: every exact local CIK member, offline; loaded_facts (default): existing issuers only.",
    )
    parser.add_argument("--universe-id", default=None)
    parser.add_argument(
        "--factor-ids",
        nargs="+",
        default=None,
        help="Restrict targets to symbols with current observations for these factor IDs.",
    )
    parser.add_argument("--limit", type=int, default=None, help="Cap targets (debug/smoke).")
    parser.add_argument("--offset", type=int, default=0, help="Skip this many deterministically ordered targets.")
    parser.add_argument(
        "--append-missing", "--skip-loaded",
        dest="skip_loaded",
        action="store_true",
        help="Append-missing only: exclude CIKs with any facts. NOT completion proof for this archive/allowlist.",
    )
    parser.add_argument(
        "--defer-derived-surfaces",
        action="store_true",
        help="Commit raw facts only; rebuild global derived surfaces once after all batches.",
    )
    parser.add_argument("--user-agent", default=SEC_USER_AGENT)
    parser.add_argument("--progress-every", type=int, default=25)
    parser.add_argument("--as-of-date", type=dt.date.fromisoformat, default=None)
    args = parser.parse_args(argv)
    if args.symbol_source == "archive_members" and (args.download or args.factor_ids):
        parser.error("archive_members is fully offline and cannot combine with --download or --factor-ids")
    return args


def main() -> int:
    args = parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    if args.download:
        _download(args.companyfacts_zip, user_agent=args.user_agent)
    if not args.companyfacts_zip.exists():
        raise SystemExit(f"companyfacts.zip not found at {args.companyfacts_zip}; pass --download to fetch it.")

    with DuckDBStore(args.db_path) as store:
        _configure_analytical_session(store, memory_limit=args.memory_limit, threads=args.threads)
        symbols: tuple[str, ...] = ("AAPL",)
        symbol_source = args.symbol_source
        if args.factor_ids:
            store.con.register(
                "companyfacts_factor_filter",
                pd.DataFrame({"factor_id": sorted(set(args.factor_ids))}),
            )
            try:
                symbols = tuple(
                    str(row[0])
                    for row in store.con.execute(
                        """
                        SELECT DISTINCT f.symbol
                        FROM fundamental_factor_values f
                        JOIN companyfacts_factor_filter x USING (factor_id)
                        WHERE f.is_latest_revision AND f.symbol IS NOT NULL
                        ORDER BY f.symbol
                        """
                    ).fetchall()
                )
            finally:
                store.con.unregister("companyfacts_factor_filter")
            if not symbols:
                raise SystemExit("No current factor observations matched --factor-ids")
            symbol_source = "symbols"
        options = SecCompanyFactsOptions(
            symbols=symbols,
            companyfacts_zip=args.companyfacts_zip,
            symbol_source=symbol_source,
            symbol_limit=args.limit,
            symbol_offset=args.offset,
            skip_loaded_targets=args.skip_loaded,
            universe_id=args.universe_id,
            skip_failed_targets=True,  # CIKs absent from the archive are recorded + skipped
            refresh_derived_surfaces=not args.defer_derived_surfaces,
            progress_every_targets=args.progress_every,
            as_of_date=args.as_of_date or utc_today(),
        )
        result = SecCompanyFactsDataset().run(store, options)
    details = dict(result.details)
    print(
        json.dumps(
            {"step": "build_companyfacts_bulk", "rows_loaded": result.rows_loaded,
             "run_id": result.run_id, "details": details},
            indent=2, default=str,
        )
    )
    return 1 if details["failed_target_count"] or details["outcome"] == "no_valid_targets" else 0


if __name__ == "__main__":
    raise SystemExit(main())
