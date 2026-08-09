#!/usr/bin/env python
"""Load Form 144 sale-intent files and reconcile them to Form 4 sales.

The script is offline/injectable: pass local SEC Form 144 XML/TXT files or a
normalized CSV/pipe file. It does not call public APIs.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from atx_db import DEFAULT_DB_PATH, DuckDBStore
from atx_db.form144 import Form144IntentDataset, Form144Options, Form144ReconciliationDataset


def _load_metadata(path: Path | None) -> dict[str, dict[str, object]]:
    if path is None:
        return {}
    with path.open("r", encoding="utf-8") as handle:
        payload = json.load(handle)
    if not isinstance(payload, dict):
        raise ValueError("--metadata-json must be a JSON object keyed by source-file path")
    return {str(key): dict(value) for key, value in payload.items()}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build SEC Form 144 intent and reconciliation surfaces.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--source-file", type=Path, action="append", dest="source_files", help="Form 144 XML/TXT/CSV file.")
    parser.add_argument("--source-files", type=Path, nargs="*", dest="source_files_many", help="One or more Form 144 files.")
    parser.add_argument("--metadata-json", type=Path, help="JSON object keyed by source-file path with accession/date metadata.")
    parser.add_argument("--source", default=Form144Options().source)
    parser.add_argument("--match-window-days", type=int, default=Form144Options().match_window_days)
    parser.add_argument("--run-id")
    parser.add_argument("--materialize-only", action="store_true", help="Skip intent loading and rebuild reconciliation links only.")
    parser.add_argument("--no-reconcile", action="store_true", help="Load intent rows without rebuilding Form 4 links.")
    parser.add_argument("--append", action="store_true", help="Do not replace existing rows for the same Form 144 filing IDs.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source_files = tuple([*(args.source_files or ()), *(args.source_files_many or ())])
    options = Form144Options(
        source_files=source_files or None,
        source=args.source,
        replace_source_files=not args.append,
        reconcile=False,
        match_window_days=args.match_window_days,
        run_id=args.run_id,
        metadata_by_source=_load_metadata(args.metadata_json),
    )

    out: dict[str, object] = {"db_path": str(args.db_path), "source": args.source}
    with DuckDBStore(args.db_path) as store:
        if not args.materialize_only:
            loaded = Form144IntentDataset().run(store, options)
            out["form144_intent"] = {
                "rows_loaded": loaded.rows_loaded,
                "run_id": loaded.run_id,
                "details": loaded.details,
            }
        if not args.no_reconcile:
            linked = Form144ReconciliationDataset().run(store, options)
            out["form144_to_form4_link"] = {
                "rows_materialized": linked.rows_loaded,
                "run_id": linked.run_id,
                "details": linked.details,
            }
    print(json.dumps(out, indent=2, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
