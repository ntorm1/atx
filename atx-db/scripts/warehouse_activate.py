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
This script is a thin CLI wrapper: all governance logic lives in
``run_activation_from_args`` (``atx_db.activation``), shared with the
``atx-db activate`` subcommand. ``stage_migrate`` applies pending migrations
directly against the ladder's own connection, which is the right thing to do
the FIRST time a warehouse is built (there is nothing yet to back up). Once a
warehouse file already exists AND has at least one pending migration, the
governed path runs instead -- checkpoint + backup + locked apply + verify,
with automatic restore-on-failure (``run_governed_migrations`` in
``atx_db.migration_admin``), followed by ``--backup-keep``-bounded backup
pruning -- BEFORE opening the ladder's own store, so a bad migration on a live
warehouse can never leave it half-upgraded. A warehouse already at head skips
the governed path entirely (it is a full-file copy + re-hash, too expensive to
pay on every resume/``--only`` call). A fresh (non-existent) database file
also skips it and lets ``stage_migrate`` bootstrap it from scratch.
``--dry-run`` skips it too: dry-run's contract is that it touches nothing.

Usage
-----
  python scripts/warehouse_activate.py --dry-run
  python scripts/warehouse_activate.py
  python scripts/warehouse_activate.py --start-stage companyfacts_load
  python scripts/warehouse_activate.py --only standardized --only reconciliation --force
"""
from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.activation import (
    GovernedMigrationsCallable,
    RunActivationCallable,
    add_activation_arguments,
    run_activation,
    run_activation_from_args,
)
from atx_db.migration_admin import run_governed_migrations as _run_governed_migrations


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_activation_arguments(parser)
    return parser.parse_args(argv)


def main(
    argv: Sequence[str] | None = None,
    *,
    governed_migrations: GovernedMigrationsCallable = _run_governed_migrations,
    run_activation: RunActivationCallable = run_activation,
) -> int:
    """Parse ``activate`` args and delegate to ``run_activation_from_args``.

    ``governed_migrations``/``run_activation`` are kept as explicit injection
    seams (not module-attribute monkeypatch targets) so this script and the
    ``atx-db activate`` subcommand share exactly one governed-migration-then-
    ladder implementation -- see ``run_activation_from_args`` in
    ``atx_db.activation`` for the pending-migration guard and backup retention.
    """
    args = parse_args(argv)
    return run_activation_from_args(
        args, governed_migrations=governed_migrations, run_activation=run_activation
    )


if __name__ == "__main__":
    raise SystemExit(main())
