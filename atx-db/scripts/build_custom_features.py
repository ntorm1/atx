"""Build/evaluate predeclared custom features after governed warehouse activation."""

from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path

from atx_db.connection import DuckDBStore, open_duckdb_connection
from atx_db.custom_features import (
    FEATURE_SOURCE,
    LABEL_SOURCE,
    PRICE_SOURCE,
    CustomEvaluationOptions,
    CustomFeatureOptions,
    evaluate_custom_features,
    refresh_custom_features,
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("build", "evaluate"))
    parser.add_argument("--db-path", required=True, type=Path)
    parser.add_argument("--as-of-date", required=True, type=dt.date.fromisoformat)
    parser.add_argument("--run-at", required=True, type=dt.datetime.fromisoformat,
                        help="Explicit UTC run timestamp, e.g. 2026-09-20T22:00:00+00:00")
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--source-sha256")
    parser.add_argument("--build-run-id")
    parser.add_argument("--price-source", default=PRICE_SOURCE)
    parser.add_argument("--feature-source", default=FEATURE_SOURCE)
    parser.add_argument("--label-source", default=LABEL_SOURCE)
    parser.add_argument("--partitions", type=int, default=16)
    parser.add_argument("--memory-limit", default="1GB")
    args = parser.parse_args()
    if not args.db_path.is_file():
        parser.error("--db-path must be an existing governed warehouse")
    if args.mode == "build" and not args.source_sha256:
        parser.error("build requires --source-sha256 of the published bulk source")
    if args.mode == "evaluate" and not args.build_run_id:
        parser.error("evaluate requires --build-run-id")
    # Avoid DuckDBStore.__enter__: it auto-initializes migrations. This command
    # requires the operator's separately backed-up governed migration workflow.
    store = DuckDBStore(args.db_path)
    store.connection = open_duckdb_connection(args.db_path)
    try:
        store.con.execute("SET memory_limit=?", [args.memory_limit])
        store.con.execute("SET threads=1")
        store.con.execute("SET preserve_insertion_order=false")
        spill = args.db_path.resolve().parent / ".custom-feature-spill"
        store.con.execute("SET temp_directory=?", [str(spill)])
        from atx_db.migrations import MIGRATIONS

        applied = {int(row[0]) for row in store.con.execute("SELECT version FROM schema_migrations").fetchall()}
        pending = [migration.version for migration in MIGRATIONS if migration.version not in applied]
        if pending or 313 not in applied:
            raise RuntimeError(f"apply governed migrations first; pending={pending}, requires0313")
        if args.mode == "build":
            result = refresh_custom_features(store, CustomFeatureOptions(
                args.as_of_date, args.run_at, args.run_id, args.source_sha256,
                args.feature_source, args.price_source, args.partitions,
            ))
        else:
            result = evaluate_custom_features(store, CustomEvaluationOptions(
                args.as_of_date, args.run_at, args.run_id, args.build_run_id, args.label_source,
            ))
        print(json.dumps(result, indent=2, sort_keys=True))
    finally:
        store.con.close()


if __name__ == "__main__":
    main()
