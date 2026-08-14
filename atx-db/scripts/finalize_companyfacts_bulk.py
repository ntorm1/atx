#!/usr/bin/env python
"""Rebuild global SEC companyfacts-derived surfaces after raw batch ingestion."""

from __future__ import annotations

import argparse
import json
import sys
import time
from collections.abc import Callable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.fundamental_statements import (
    refresh_fundamental_periods,
    refresh_fundamental_statement_points,
    refresh_fundamental_ttm_points,
)
from atx_db.fundamentals import (
    refresh_fundamental_fact_revisions,
    refresh_xbrl_concept_catalog,
)
from atx_db.standardization import (
    FundamentalStandardizationOptions,
    refresh_fundamental_standardized,
)

STEP_ORDER = ("catalog", "revisions", "statements", "periods", "ttm", "standardized")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument(
        "--steps",
        nargs="+",
        choices=STEP_ORDER,
        default=list(STEP_ORDER),
        help="Idempotent surfaces to rebuild in dependency order.",
    )
    args = parser.parse_args()
    selected = [step for step in STEP_ORDER if step in set(args.steps)]
    results: list[dict[str, object]] = []
    with DuckDBStore(args.db_path) as store:
        operations: dict[str, Callable[[], int]] = {
            "catalog": lambda: refresh_xbrl_concept_catalog(store),
            "revisions": lambda: refresh_fundamental_fact_revisions(store),
            "statements": lambda: refresh_fundamental_statement_points(store),
            "periods": lambda: refresh_fundamental_periods(store),
            "ttm": lambda: refresh_fundamental_ttm_points(store),
            "standardized": lambda: refresh_fundamental_standardized(
                store,
                FundamentalStandardizationOptions(materialize_result_limit=0),
            ).standardized_row_count,
        }
        for step in selected:
            print(json.dumps({"step": step, "status": "running"}), flush=True)
            started = time.perf_counter()
            rows = operations[step]()
            result = {
                "step": step,
                "status": "complete",
                "rows": rows,
                "elapsed_seconds": round(time.perf_counter() - started, 3),
            }
            results.append(result)
            print(json.dumps(result), flush=True)
    print(json.dumps({"status": "complete", "steps": results}, indent=2), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
