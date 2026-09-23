"""Evaluate a completed frozen fundamental signal panel in an existing warehouse."""

from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path

from atx_db.connection import DuckDBStore, open_duckdb_connection
from atx_db.fundamental_signal_evaluation import (
    FundamentalSignalEvaluationOptions,
    evaluate_fundamental_signals,
)


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    root.add_argument("--db-path", type=Path, required=True)
    root.add_argument("--build-run-id", required=True)
    root.add_argument("--run-id", required=True)
    root.add_argument("--as-of-date", type=dt.date.fromisoformat, required=True)
    root.add_argument("--run-at", type=dt.datetime.fromisoformat, required=True)
    root.add_argument("--label-source", required=True)
    root.add_argument("--memory-limit", default="256MB")
    root.add_argument("--threads", type=int, default=1)
    return root


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    if not args.db_path.is_file():
        parser().error("--db-path must identify an existing migrated warehouse")
    options = FundamentalSignalEvaluationOptions(
        build_run_id=args.build_run_id, run_id=args.run_id,
        as_of_date=args.as_of_date, run_at=args.run_at,
        label_source=args.label_source, memory_limit=args.memory_limit,
        threads=args.threads,
    )
    store = DuckDBStore(args.db_path)
    # Direct open avoids DuckDBStore's implicit initialization/migration path.
    store.connection = open_duckdb_connection(args.db_path)
    try:
        store._configure_session(store.con)
        result = evaluate_fundamental_signals(store, options)
    finally:
        store.connection.close()
        store.connection = None
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
