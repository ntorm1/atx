#!/usr/bin/env python
"""Activate a complete atx-db warehouse from zero, resumably.

Runs the governed activation ladder in dependency order, one JSON line per stage,
recording every attempt in ``activation_stage_runs``. A rerun skips stages whose
newest attempt completed, so an interrupted multi-hour build resumes exactly where
it stopped.

Stages (in order)
-----------------
  migrate, security_master, symbol_directory, ticker_history_extract,
  ticker_history_publish, sec_bulk_download, submissions_load,
  companyfacts_load, statement_points, periods, ttm, calendarization,
  standardized, industry_templates, reconciliation, provider_coverage

Network is limited to security_master, symbol_directory, and sec_bulk_download.
``ATX_SEC_USER_AGENT`` (or ``--sec-user-agent``) is required before any SEC
request; the ladder fails fast without it.

Migration governance
---------------------
``stage_migrate`` applies pending migrations directly against the ladder's own
connection, which is the right thing to do the FIRST time a warehouse is built
(there is nothing yet to back up). Once a warehouse file already exists, this
script runs the governed path instead -- checkpoint + backup + locked apply +
verify, with automatic restore-on-failure (``run_governed_migrations`` in
``atx_db.migration_admin``) -- BEFORE opening the ladder's own store, so a bad
migration on a live warehouse can never leave it half-upgraded. A fresh
(non-existent) database file skips this and lets ``stage_migrate`` bootstrap it
from scratch. ``--dry-run`` skips it too: dry-run's contract is that it touches
nothing, and the governed path always writes a checkpoint/backup.

Usage
-----
  python scripts/warehouse_activate.py --dry-run
  python scripts/warehouse_activate.py
  python scripts/warehouse_activate.py --start-stage companyfacts_load
  python scripts/warehouse_activate.py --only standardized --only reconciliation --force
"""
from __future__ import annotations

import argparse
import datetime as dt
import os
import sys
from collections.abc import Callable, Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.activation import (
    DEFAULT_TICKER_HISTORY_ZIP,
    STAGE_ORDER,
    ActivationOptions,
    requests_downloader,
    run_activation,
    select_stages,
)
from atx_db.clock import utc_today
from atx_db.connection import DEFAULT_DB_PATH
from atx_db.migration_admin import run_governed_migrations as _run_governed_migrations

GovernedMigrationsCallable = Callable[[Path], object]


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--as-of-date", type=dt.date.fromisoformat, default=None)
    parser.add_argument("--ticker-history-zip", type=Path, default=DEFAULT_TICKER_HISTORY_ZIP)
    parser.add_argument("--staging-dir", type=Path, default=Path("data/staging/broad-bars"))
    parser.add_argument("--cache-dir", type=Path, default=Path("data/cache"))
    parser.add_argument("--sec-user-agent", default=None)
    parser.add_argument("--memory-limit", default="8GB")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--shards", type=int, default=16)
    parser.add_argument("--companyfacts-limit", type=int, default=None)
    parser.add_argument("--start-stage", choices=STAGE_ORDER, default=None)
    parser.add_argument("--stop-stage", choices=STAGE_ORDER, default=None)
    parser.add_argument("--only", action="append", choices=STAGE_ORDER, default=None)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--run-id", default=None)
    return parser.parse_args(argv)


def main(
    argv: Sequence[str] | None = None,
    *,
    governed_migrations: GovernedMigrationsCallable = _run_governed_migrations,
) -> int:
    args = parse_args(argv)
    as_of_date = args.as_of_date or utc_today()  # the only clock read in the ladder's path
    sec_user_agent = args.sec_user_agent or os.environ.get("ATX_SEC_USER_AGENT")
    run_id = args.run_id or f"warehouse-activate-{as_of_date.isoformat()}"
    options = ActivationOptions(
        db_path=args.db_path,
        as_of_date=as_of_date,
        ticker_history_zip=args.ticker_history_zip,
        staging_dir=args.staging_dir,
        cache_dir=args.cache_dir,
        sec_user_agent=sec_user_agent,
        downloader=requests_downloader,
        memory_limit=args.memory_limit,
        threads=args.threads,
        reconciliation_shards=args.shards,
        companyfacts_limit=args.companyfacts_limit,
        dry_run=args.dry_run,
        force=args.force,
        run_id=run_id,
    )
    stages = select_stages(
        start=args.start_stage,
        stop=args.stop_stage,
        only=tuple(args.only or ()),
    )
    # A pre-existing warehouse goes through checkpoint + backup + locked apply +
    # verify (with restore-on-failure) before the ladder ever opens its own
    # connection; a brand-new file has nothing to protect yet, so stage_migrate
    # bootstraps it directly. --dry-run never mutates anything, governed path
    # included.
    if not options.dry_run and options.db_path.exists():
        governed_migrations(options.db_path)
    run_activation(options, stages=stages)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
