"""Operator entry point for the public delisting evidence streams."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from atx_db.clock import utc_today
from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.delisting_evidence import (
    ARCHIVE_GAP_SESSIONS,
    MERGER_LOOKBACK_DAYS,
    DelistingEvidenceOptions,
    fold_evidence_into_delisting_events,
    refresh_delisting_evidence,
)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="build-delisting-evidence")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--archive-gap-sessions", type=int, default=ARCHIVE_GAP_SESSIONS)
    parser.add_argument("--merger-lookback-days", type=int, default=MERGER_LOOKBACK_DAYS)
    parser.add_argument("--no-archive-inference", action="store_true")
    parser.add_argument("--run-id")
    args = parser.parse_args(argv)

    options = DelistingEvidenceOptions(
        archive_gap_sessions=args.archive_gap_sessions,
        merger_lookback_days=args.merger_lookback_days,
        include_archive_inference=not args.no_archive_inference,
        as_of_date=utc_today(),
        run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        evidence = refresh_delisting_evidence(store, options)
        events = fold_evidence_into_delisting_events(store, options)
    print(json.dumps({"evidence_rows": evidence, "delisting_events": events}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
