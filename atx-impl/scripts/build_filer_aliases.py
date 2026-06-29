#!/usr/bin/env python
"""Build the 13F filer entity-resolution alias spine (`filer_13f_cik_alias`).

Derives, from the loaded 13F manager universe:
  * SELF identity rows (one per filing-manager CIK),
  * NAME_HISTORY rows for intra-CIK renames,
  * low-confidence NAME_MATCH_CANDIDATE links between same-named filers,
and optionally injects authoritative subadvisor / M&A-continuity rollups from a
curated seed CSV (columns: parent_cik, child_cik, alias_type, valid_from,
confidence, evidence, [valid_to], [available_at]).

No network access — operates purely on the local warehouse plus the seed file.

Usage
-----
  python scripts/build_filer_aliases.py [--db-path PATH] [--seed-file PATH]
         [--source NAME] [--no-replace]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore
from db.filer_alias import FilerAliasDataset, FilerAliasOptions


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build the 13F filer CIK alias / entity-resolution spine."
    )
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument(
        "--seed-file",
        type=Path,
        default=None,
        help="Optional curated subadvisor / M&A-continuity seed CSV.",
    )
    parser.add_argument("--source", default=FilerAliasOptions().source)
    parser.add_argument(
        "--no-replace",
        action="store_true",
        help="Append rows for this source instead of replacing them first.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with DuckDBStore(args.db_path) as store:
        result = FilerAliasDataset().run(
            store,
            FilerAliasOptions(
                source=args.source,
                seed_file=args.seed_file,
                replace=not args.no_replace,
            ),
        )
        print(
            json.dumps(
                {
                    "step": "filer_13f_cik_alias",
                    "rows_loaded": result.rows_loaded,
                    "dataset_id": result.dataset_id,
                    "run_id": result.run_id,
                    "details": result.details,
                },
                indent=2,
                default=str,
            )
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
