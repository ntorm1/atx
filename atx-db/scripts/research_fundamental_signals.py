"""Build a bounded run-versioned fundamental signal research panel."""

from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path

from atx_db.connection import DuckDBStore, open_duckdb_connection
from atx_db.fundamental_signal_research import (
    FundamentalSignalResearchOptions,
    build_fundamental_signal_panel,
    canonical_signals,
)


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build")
    build.add_argument("--db-path", type=Path, required=True)
    build.add_argument("--start-date", type=dt.date.fromisoformat, required=True)
    build.add_argument("--end-date", type=dt.date.fromisoformat, required=True)
    build.add_argument("--as-of-date", type=dt.date.fromisoformat, required=True)
    build.add_argument("--run-at", type=dt.datetime.fromisoformat, required=True)
    build.add_argument("--run-id", required=True)
    build.add_argument("--signals-json", type=Path)
    build.add_argument("--max-age-days", type=int, default=200)
    build.add_argument("--memory-limit", default="256MB")
    build.add_argument("--threads", type=int, default=1)
    return root


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    if not args.db_path.is_file():
        parser().error("--db-path must identify an existing migrated warehouse")
    specs = None
    if args.signals_json:
        raw = json.loads(args.signals_json.read_text(encoding="utf-8"))
        specs = canonical_signals(raw)
    options = FundamentalSignalResearchOptions(
        start_date=args.start_date, end_date=args.end_date, as_of_date=args.as_of_date,
        run_at=args.run_at, run_id=args.run_id, signals=specs,
        max_age_days=args.max_age_days, memory_limit=args.memory_limit, threads=args.threads,
    )
    # Open the existing warehouse directly: DuckDBStore.__enter__ initializes
    # and migrates, which a research build must never do implicitly.
    store = DuckDBStore(args.db_path)
    store.connection = open_duckdb_connection(args.db_path)
    try:
        store._configure_session(store.con)
        store.con.execute("SET memory_limit = ?", [args.memory_limit])
        store.con.execute("SET threads = ?", [args.threads])
        store.con.execute("SET preserve_insertion_order = false")
        result = build_fundamental_signal_panel(store, options)
    finally:
        store.connection.close()
        store.connection = None
    print(json.dumps(result.__dict__, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
