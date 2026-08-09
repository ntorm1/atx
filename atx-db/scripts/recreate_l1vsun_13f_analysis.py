from __future__ import annotations

import argparse
import json
from pathlib import Path

from atx_db.connection import DEFAULT_DB_PATH, DuckDBStore
from atx_db.thirteenf_analysis import write_thirteenf_analysis_report


def main() -> int:
    parser = argparse.ArgumentParser(description="Render the independent 13F amendment-spike reproduction")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("research") / "recreated-l1vsun-13f-amendment-analysis.md",
    )
    parser.add_argument("--json-output", type=Path)
    args = parser.parse_args()
    with DuckDBStore(args.db_path, read_only=True) as store:
        summary = write_thirteenf_analysis_report(store, args.output, json_path=args.json_output)
    print(json.dumps(summary, default=str, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
