"""Operator entry point for the wide quarterly and daily-market panel exports."""

from __future__ import annotations

import argparse
import datetime as dt
import json
from dataclasses import asdict
from pathlib import Path

from atx_db.connection import DuckDBStore, resolve_data_dir
from atx_db.panel_export import export_panel_daily_market, export_panel_quarterly


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=None)
    parser.add_argument("--as-of", type=dt.date.fromisoformat, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--item", action="append", default=[], dest="items")
    parser.add_argument("--metric", action="append", default=[], dest="metrics")
    parser.add_argument("--panel", choices=("quarterly", "daily", "both"), default="both")
    args = parser.parse_args(argv)

    db_path = args.db_path or (resolve_data_dir() / "warehouse.duckdb")
    with DuckDBStore(db_path) as store:
        store.initialize()
        if args.panel in ("quarterly", "both"):
            result = export_panel_quarterly(
                store, args.as_of, items=tuple(args.items), metrics=tuple(args.metrics), out_dir=args.out_dir
            )
            print(json.dumps(asdict(result), default=str, sort_keys=True))
        if args.panel in ("daily", "both"):
            result = export_panel_daily_market(store, args.as_of, metrics=tuple(args.metrics), out_dir=args.out_dir)
            print(json.dumps(asdict(result), default=str, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
