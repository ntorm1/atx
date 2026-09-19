#!/usr/bin/env python
"""Rank observed us-gaap concepts against canonical items (research only).

Writes research/alias_candidates.csv. It never edits a seed. Curate the output
by hand into src/atx_db/seeds/statement_map.csv + fundamental_items.csv +
standardization_rules.csv, then run scripts/normalize_fundamental_seeds.py.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.alias_mining import AliasMiningOptions, mine_alias_candidates, write_alias_candidates
from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore

DEFAULT_OUTPUT = Path(__file__).resolve().parents[1] / "research" / "alias_candidates.csv"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--minimum-filer-count", type=int, default=5)
    parser.add_argument("--minimum-fact-count", type=int, default=1)
    parser.add_argument("--top-n", type=int, default=25)
    parser.add_argument("--minimum-similarity", type=float, default=0.05)
    args = parser.parse_args(argv)

    options = AliasMiningOptions(
        minimum_filer_count=args.minimum_filer_count,
        minimum_fact_count=args.minimum_fact_count,
        top_n=args.top_n,
        minimum_similarity=args.minimum_similarity,
    )
    with DuckDBStore(args.db_path, read_only=True) as store:
        candidates = mine_alias_candidates(store, options)
    written = write_alias_candidates(candidates, args.output)
    print(
        json.dumps(
            {
                "output": str(args.output),
                "rows": written,
                "items_ranked": len({c.item_id for c in candidates}),
                "concepts_profiled": len({(c.taxonomy, c.concept) for c in candidates}),
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
