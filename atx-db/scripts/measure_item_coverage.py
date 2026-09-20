#!/usr/bin/env python
"""Measure standardized item coverage and optionally publish docs/ITEM_COVERAGE.md."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.item_coverage import (
    DEFAULT_UNIVERSE_ID,
    ItemCoverageOptions,
    evaluate_item_coverage_gate,
    measure_item_coverage,
    refresh_item_coverage,
    render_item_coverage_markdown,
)
from atx_db.item_coverage_cohort import AnnualCoverageCohortOptions, refresh_item_coverage_cohort
from atx_db.migrations.registry import MIGRATIONS

PROJECT_ROOT = Path(__file__).resolve().parents[1]
DOCS_PATH = PROJECT_ROOT / "docs" / "ITEM_COVERAGE.md"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--universe-id", default=DEFAULT_UNIVERSE_ID)
    parser.add_argument("--as-of-date", type=dt.date.fromisoformat, required=True)
    parser.add_argument("--minimum-fiscal-year", type=int, default=2015)
    parser.add_argument("--maximum-fiscal-year", type=int)
    parser.add_argument("--rebuild-cohort", action="store_true")
    parser.add_argument("--memory-limit", default="1GB")
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--basis", action="append", dest="bases")
    parser.add_argument("--write-docs", action="store_true")
    parser.add_argument("--run-id")
    args = parser.parse_args(argv)

    options = ItemCoverageOptions(
        as_of_date=args.as_of_date,
        universe_id=args.universe_id,
        bases=tuple(args.bases or ("annual", "quarterly", "instant", "ttm")),
        minimum_fiscal_year=args.minimum_fiscal_year,
        maximum_fiscal_year=args.maximum_fiscal_year,
        run_id=args.run_id,
    )
    # Refuse implicit live migration: the governed operator migration must run first.
    with DuckDBStore(args.db_path, read_only=True) as store:
        applied = {int(r[0]) for r in store.con.execute("SELECT version FROM schema_migrations").fetchall()}
        pending = sorted(m.version for m in MIGRATIONS if m.version not in applied)
        if pending:
            parser.error(f"governed migration required before measurement: {pending}")
    with DuckDBStore(args.db_path) as store:
        store.con.execute("SET memory_limit=?", [args.memory_limit])
        store.con.execute("SET threads=?", [args.threads])
        store.con.execute("SET preserve_insertion_order=false")
        if args.rebuild_cohort:
            if args.universe_id != DEFAULT_UNIVERSE_ID:
                parser.error("--rebuild-cohort requires the authoritative annual universe")
            refresh_item_coverage_cohort(
                store,
                AnnualCoverageCohortOptions(
                    as_of_date=args.as_of_date,
                    minimum_fiscal_year=args.minimum_fiscal_year,
                    maximum_fiscal_year=args.maximum_fiscal_year,
                    run_id=args.run_id,
                ),
            )
        frame = measure_item_coverage(store, options)
        written = refresh_item_coverage(store, options, frame=frame)
    gate = evaluate_item_coverage_gate(frame, as_of_date=args.as_of_date)
    if args.write_docs:
        DOCS_PATH.parent.mkdir(parents=True, exist_ok=True)
        DOCS_PATH.write_text(
            render_item_coverage_markdown(
                frame, generated_from="scripts/measure_item_coverage.py", as_of_date=args.as_of_date
            ),
            encoding="utf-8",
        )
    print(
        json.dumps(
            {"rows_written": written, "gate": gate, "docs": str(DOCS_PATH) if args.write_docs else None},
            indent=2,
            default=str,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
