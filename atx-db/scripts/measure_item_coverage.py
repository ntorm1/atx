#!/usr/bin/env python
"""Measure standardized item coverage and optionally publish docs/ITEM_COVERAGE.md."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.item_coverage import (
    ItemCoverageOptions,
    compute_item_coverage_rows,
    evaluate_item_coverage_gate,
    load_item_coverage_inputs,
    refresh_item_coverage,
    render_item_coverage_markdown,
)

PROJECT_ROOT = Path(__file__).resolve().parents[1]
DOCS_PATH = PROJECT_ROOT / "docs" / "ITEM_COVERAGE.md"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--universe-id", default="us_common_equity_liquid_v1")
    parser.add_argument("--minimum-fiscal-year", type=int, default=1990)
    parser.add_argument("--basis", action="append", dest="bases")
    parser.add_argument("--write-docs", action="store_true")
    parser.add_argument("--run-id")
    args = parser.parse_args(argv)

    options = ItemCoverageOptions(
        universe_id=args.universe_id,
        bases=tuple(args.bases or ("annual", "quarterly", "instant", "ttm")),
        minimum_fiscal_year=args.minimum_fiscal_year,
        run_id=args.run_id,
    )
    with DuckDBStore(args.db_path) as store:
        written = refresh_item_coverage(store, options)
        standardized, universe = load_item_coverage_inputs(store, options)
        frame = compute_item_coverage_rows(standardized, universe, options)
    gate = evaluate_item_coverage_gate(frame)
    if args.write_docs:
        DOCS_PATH.parent.mkdir(parents=True, exist_ok=True)
        DOCS_PATH.write_text(
            render_item_coverage_markdown(frame, generated_from="scripts/measure_item_coverage.py"),
            encoding="utf-8",
        )
    print(json.dumps({"rows_written": written, "gate": gate, "docs": str(DOCS_PATH) if args.write_docs else None}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
