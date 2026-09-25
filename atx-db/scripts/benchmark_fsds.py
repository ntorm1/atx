"""P12 benchmark: standardized fundamentals vs SEC Financial Statement Data Sets (FSDS).

Subcommands (light unless stated):
  fetch     download <= 8 FSDS quarterly zips once (RX10; approved SEC UA; hashed manifest)
  load      stream the cached zips' sub/num/tag into the panel subset DB (DuckDB 384MB / 2 threads)
  coverage  50 issuers x 5 FY x 10 core items mapping coverage + unmapped reasons (no warehouse)
  compare   score the grid against fundamental_standardized in --db through fact_disagreement.
            --db must not be the governed warehouse. HEAVY SLOT ONLY with --copy-from-warehouse:
            the warehouse is attached READ_ONLY, the panel's fundamental_standardized rows are
            copied into the scratch --db, and all writes go to the scratch DB.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from pathlib import Path
from typing import Any

from atx_db.connection import DuckDBStore
from atx_db.fsds_baseline import (
    BENCHMARK_PANEL,
    BENCHMARK_QUARTERS,
    CORE_ITEMS,
    SUBSET_DB_NAME,
    FsdsComparisonOptions,
    benchmark_coverage,
    cached_fsds_archives,
    copy_warehouse_standardized_subset,
    default_cache_dir,
    fetch_fsds_quarters,
    governed_warehouse_path,
    load_fsds_subset,
    open_fsds_subset,
    run_fsds_comparison,
)


def _json_default(value: Any) -> Any:
    if isinstance(value, (dt.date, dt.datetime)):
        return value.isoformat()
    return str(value)


def _write(path: Path | None, payload: Any) -> None:
    text = json.dumps(payload, indent=2, sort_keys=True, default=_json_default)
    if path is None:
        print(text)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    print(f"wrote {path}")


def _coverage_markdown(summary: dict[str, Any]) -> str:
    lines = [
        f"cells {summary['cells']} | mapped {summary['mapped']} ({summary['mapped_ratio']:.1%}) | "
        f"direct {summary['mapped_direct']} | derived {summary['mapped_derived']} | "
        f"in-window {summary['cells_in_window']} (mapped {summary['mapped_ratio_in_window']:.1%})",
        "",
        "| item | cells | mapped | direct | derived | unmapped reasons |",
        "|---|---:|---:|---:|---:|---|",
    ]
    for name, row in summary["by_item"].items():
        reasons = ", ".join(f"{k} {v}" for k, v in row["unmapped_reasons"].items()) or "-"
        lines.append(f"| {name} | {row['cells']} | {row['mapped']} | {row['mapped_direct']} | {row['mapped_derived']} | {reasons} |")
    return "\n".join(lines)


def _subset_path(args: argparse.Namespace) -> Path:
    return Path(args.subset_db) if args.subset_db else Path(args.cache_dir) / SUBSET_DB_NAME


def _coverage(args: argparse.Namespace):
    con = open_fsds_subset(_subset_path(args), read_only=True)
    try:
        return benchmark_coverage(con, BENCHMARK_PANEL, CORE_ITEMS, years=args.years)
    finally:
        con.close()


def cmd_fetch(args: argparse.Namespace) -> int:
    archives = fetch_fsds_quarters(args.quarters, Path(args.cache_dir))
    _write(None, [{"quarter": a.quarter, "sha256": a.sha256, "bytes": a.size_bytes, "downloaded_now": a.downloaded_now} for a in archives])
    return 0


def cmd_load(args: argparse.Namespace) -> int:
    archives = cached_fsds_archives(Path(args.cache_dir), args.quarters)
    stats = load_fsds_subset(archives, _subset_path(args), ciks=[issuer.cik for issuer in BENCHMARK_PANEL])
    _write(None, stats)
    return 0


def cmd_coverage(args: argparse.Namespace) -> int:
    coverage = _coverage(args)
    print(_coverage_markdown(coverage.summary))
    if args.out:
        out = Path(args.out)
        _write(out / "P12-fsds-coverage-summary.json", coverage.summary)
        coverage.cells.to_csv(out / "P12-fsds-coverage-cells.csv", index=False)
    return 0


def cmd_compare(args: argparse.Namespace) -> int:
    db_path = Path(args.db).resolve()
    if db_path == governed_warehouse_path():
        print("refusing: --db is the governed warehouse; compare in a scratch DB (see --copy-from-warehouse)", file=sys.stderr)
        return 2
    if args.copy_from_warehouse and not args.heavy_slot:
        print("refusing: --copy-from-warehouse opens the governed warehouse; pass --heavy-slot only inside the heavy slot", file=sys.stderr)
        return 2
    coverage = _coverage(args)
    with DuckDBStore(db_path) as store:
        store.con.execute(f"SET memory_limit='{args.memory_limit}'")
        store.con.execute(f"SET threads={int(args.threads)}")
        copied = None
        if args.copy_from_warehouse:
            copied = copy_warehouse_standardized_subset(store, Path(args.copy_from_warehouse), [i.cik for i in BENCHMARK_PANEL])
        result = run_fsds_comparison(
            store,
            coverage.grid_facts,
            panel=BENCHMARK_PANEL,
            options=FsdsComparisonOptions(run_id=args.run_id),
        )
    payload = {"copied_standardized_rows": copied, "coverage": coverage.summary, "comparison": result.summary}
    _write(Path(args.out) / "P12-fsds-comparison.json" if args.out else None, payload)
    if args.out and not result.rows.empty:
        result.rows.to_csv(Path(args.out) / "P12-fsds-comparison-rows.csv", index=False)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cache-dir", default=str(default_cache_dir()))
    parser.add_argument("--subset-db", default=None)
    parser.add_argument("--quarters", nargs="+", default=list(BENCHMARK_QUARTERS))
    parser.add_argument("--years", type=int, default=5)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("fetch").set_defaults(func=cmd_fetch)
    sub.add_parser("load").set_defaults(func=cmd_load)
    coverage = sub.add_parser("coverage")
    coverage.add_argument("--out", default=None)
    coverage.set_defaults(func=cmd_coverage)
    compare = sub.add_parser("compare")
    compare.add_argument("--db", required=True, help="scratch/fixture DB holding fundamental_standardized")
    compare.add_argument("--copy-from-warehouse", default=None, help="HEAVY SLOT: warehouse path attached READ_ONLY")
    compare.add_argument("--heavy-slot", action="store_true")
    compare.add_argument("--memory-limit", default="1GB")
    compare.add_argument("--threads", type=int, default=2)
    compare.add_argument("--run-id", default=None)
    compare.add_argument("--out", default=None)
    compare.set_defaults(func=cmd_compare)
    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
