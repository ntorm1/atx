#!/usr/bin/env python
"""Load FINRA daily short-volume files and materialize daily short-flow metrics.

The script is offline/injectable: pass a local FINRA pipe-delimited daily file
(``Date|Symbol|ShortVolume|ShortExemptVolume|TotalVolume|Market``) or a normalized
CSV. It does not call public APIs.

Examples:
  python scripts/build_short_volume.py --source-file PATH
  python scripts/build_short_volume.py --materialize-only
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from db import DEFAULT_DB_PATH, DuckDBStore
from db.short_volume import (
    FinraShortVolumeDataset,
    FinraShortVolumeOptions,
    ShortVolumeMetricsDataset,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build FINRA daily short-volume flow surfaces.")
    parser.add_argument("--db-path", type=Path, default=DEFAULT_DB_PATH)
    parser.add_argument("--source-file", type=Path, default=None, help="FINRA daily short-volume file or normalized CSV.")
    parser.add_argument("--source", default="injected_finra_daily_short_volume_v1")
    parser.add_argument("--materialize-only", action="store_true", help="Skip raw file load and rebuild metrics from existing raw rows.")
    parser.add_argument(
        "--append",
        action="store_true",
        help="Do not delete existing rows from the same source file hash before inserting.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    options = FinraShortVolumeOptions(
        source_file=args.source_file,
        source=args.source,
        replace_source_file=not args.append,
    )
    out: dict[str, object] = {}
    with DuckDBStore(args.db_path) as store:
        if not args.materialize_only:
            raw = FinraShortVolumeDataset().run(store, options)
            out["finra_short_volume"] = {"rows_loaded": raw.rows_loaded, "run_id": raw.run_id}
        metrics = ShortVolumeMetricsDataset().run(store, options)
        out["short_volume_metrics"] = {"rows_materialized": metrics.rows_loaded, "run_id": metrics.run_id}
    print(json.dumps(out, indent=2, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
