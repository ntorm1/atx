#!/usr/bin/env python
"""Recover an interrupted broad-bar partition and publish it natively."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import logging
import os
import sys
from dataclasses import asdict
from pathlib import Path

import duckdb

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.migrations import apply_pending_migrations, verify_migration_checksums
from atx_db.ticker_history import SOURCE_NAME
from atx_db.ticker_history_bulk import BulkTickerHistoryOptions, publish_bulk_ticker_history

LOGGER = logging.getLogger(__name__)


def _status(path: Path, state: str, **values: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schema_version": 1,
        "state": state,
        "updated_at": dt.datetime.now(dt.UTC).isoformat(),
        **values,
    }
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(payload, indent=2, sort_keys=True, default=str) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument(
        "--tsv-path",
        type=Path,
        default=Path("data/staging/broad-bars/tbltickerhistory3_10y.txt"),
    )
    parser.add_argument(
        "--status-path",
        type=Path,
        default=Path("research/broad-bars-native-bulk-status.json"),
    )
    parser.add_argument("--memory-limit", default="4GB")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--run-id", default="broad-bars-native-bulk-v1")
    args = parser.parse_args()
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    status_path = args.status_path.resolve()
    try:
        _status(status_path, "clearing_reproducible_partition", run_id=args.run_id)
        store = DuckDBStore(args.db_path)
        store.connection = duckdb.connect(str(args.db_path))
        try:
            store._configure_session(store.con)
            store.con.execute("PRAGMA disable_progress_bar")
            store.con.execute("SET memory_limit = ?", [args.memory_limit])
            store.con.execute("SET threads = ?", [args.threads])
            source_count = store.con.execute(
                "SELECT count(*) FROM equity_daily_bars WHERE source = ?", [SOURCE_NAME]
            ).fetchone()
            LOGGER.info("clearing %s interrupted source rows", source_count[0] if source_count else None)
            store.con.execute("DROP INDEX IF EXISTS idx_equity_daily_bars_security_date")
            store.con.execute("DROP INDEX IF EXISTS idx_equity_daily_bars_symbol_date")
            with store.transaction():
                store.con.execute("DELETE FROM equity_daily_bars WHERE source = ?", [SOURCE_NAME])
            store.con.execute("CHECKPOINT")
            _status(status_path, "applying_canonical_market_cap", run_id=args.run_id)
            applied = apply_pending_migrations(store.con)
            verify_migration_checksums(store.con)
            LOGGER.info("applied migrations %s", applied)
            store.con.execute("CHECKPOINT")
            _status(status_path, "building_native_bulk_stage", run_id=args.run_id)
            result = publish_bulk_ticker_history(
                store,
                BulkTickerHistoryOptions(
                    tsv_path=args.tsv_path.resolve(),
                    memory_limit=args.memory_limit,
                    threads=args.threads,
                    run_id=args.run_id,
                ),
            )
        finally:
            if store.connection is not None:
                store.connection.close()
                store.connection = None
        payload = asdict(result)
        _status(status_path, "complete", **payload)
        print(json.dumps(payload, indent=2, sort_keys=True, default=str))
        return 0
    except Exception as exc:
        LOGGER.exception("native bulk publication failed")
        _status(status_path, "failed", run_id=args.run_id, error=str(exc))
        raise


if __name__ == "__main__":
    raise SystemExit(main())
