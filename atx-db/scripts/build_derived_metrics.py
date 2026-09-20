"""Operator entry point for the declarative derived-metric engine."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from atx_db.connection import DuckDBStore, resolve_data_dir
from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
from atx_db.derived_registry import seed_derived_metric_definitions


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=None)
    parser.add_argument("--metric", action="append", default=None)
    parser.add_argument("--security-id", action="append", default=None)
    parser.add_argument("--batch-size", type=int, default=500)
    parser.add_argument("--run-id", default=None)
    args = parser.parse_args(argv)

    db_path = args.db_path or (resolve_data_dir() / "warehouse.duckdb")
    with DuckDBStore(db_path) as store:
        store.initialize()
        seeded = seed_derived_metric_definitions(store)
        rows = refresh_derived_metrics(
            store,
            DerivedMetricsOptions(
                metric_codes=tuple(args.metric) if args.metric else None,
                security_ids=tuple(args.security_id) if args.security_id else None,
                batch_size=args.batch_size,
                run_id=args.run_id,
            ),
        )
    print(json.dumps({"definitions_seeded": seeded, "values_written": rows}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
