#!/usr/bin/env python3
"""Migrate the OPRA hive v1 layout to v2 — pure local IO, $0, zero network.

v1 (per-symbol, one underlier per file):

    <src>/<underlying>/<YYYY-MM-DD>.parquet

v2 (date-partitioned hive, all symbols for a session in one file):

    <dst>/date=<YYYY-MM-DD>/data.parquet

Per date, every per-symbol source file is read, its schema validated against the
frozen canonical 8-column schema (hard error on drift), the rows concatenated,
sorted by ``(underlying, symbol)``, and written atomically (``.tmp`` +
``os.replace``) with **one parquet row group per underlying** so a date's
distinct-underlying set is recoverable from footer statistics alone.

Idempotent and resumable: a destination date file whose distinct-underlying set
already contains (is a superset of) the source's is skipped without a rewrite —
that check reads only parquet footer metadata, never the data. Every run drops a
``migration_manifest_<ts>.csv`` (date, n_source_files, n_rows, status).

Usage:

    python migrate_opra_hive.py --src C:/atx-data/spy-dispersion/opra \\
                                --dst C:/atx-data/opra-hive \\
                                [--from YYYY-MM-DD --to YYYY-MM-DD] [--dry-run]
"""
from __future__ import annotations

import argparse
import csv
import datetime
import os
import pathlib
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

# Frozen v2 schema — exact column order + types every date file must have
# (px int64 1e-9 fixed-point, unset side = INT64_MIN). Kept byte-for-byte in
# sync with the v1 pull tool (tools/pull_opra_universe_batch.py ARROW_SCHEMA).
CANONICAL_SCHEMA = pa.schema(
    [
        ("ts", pa.timestamp("ns")),
        ("underlying", pa.string()),
        ("symbol", pa.string()),
        ("instrument_id", pa.int64()),
        ("bid_px", pa.int64()),
        ("ask_px", pa.int64()),
        ("bid_sz", pa.int64()),
        ("ask_sz", pa.int64()),
    ]
)

DATA_FILE = "data.parquet"


@dataclass
class DateResult:
    """One row of the migration manifest."""

    date: str
    n_source_files: int
    n_rows: int
    status: str  # "written" | "skipped" | "planned"


@dataclass
class MigrationStats:
    n_dates: int = 0
    n_written: int = 0
    n_skipped: int = 0
    n_source_files: int = 0
    n_rows: int = 0
    manifest_path: Optional[str] = None
    results: List[DateResult] = field(default_factory=list)


# ── schema / footer helpers ──────────────────────────────────────────────────
def _validate_schema(schema: pa.Schema, path) -> None:
    """Fail closed on any drift from the frozen canonical schema."""
    if not schema.equals(CANONICAL_SCHEMA, check_metadata=False):
        raise ValueError(
            f"schema drift in {path}: file schema does not match the canonical "
            f"OPRA v2 schema.\n  expected: {CANONICAL_SCHEMA}\n  got:      {schema}"
        )


def _footer_underlyings(path) -> Optional[Set[str]]:
    """Distinct ``underlying`` values recoverable from parquet footer statistics
    alone (no data scan). Returns ``None`` when the footer cannot answer it
    (missing stats), which the caller treats as "cannot prove superset" and so
    rewrites — the safe, correctness-preserving direction.

    Files we write have one row group per underlying (min == max per group), so
    the union of per-row-group {min, max} is the exact distinct set. For any
    other file this union is always a SUBSET of the true set (min/max are real
    values present), so it can never wrongly claim a superset.
    """
    try:
        pf = pq.ParquetFile(path)
    except Exception:  # noqa: BLE001 — unreadable footer -> force rewrite
        return None
    md = pf.metadata
    names = pf.schema_arrow.names
    if "underlying" not in names:
        return None
    ci = names.index("underlying")
    out: Set[str] = set()
    for rg in range(md.num_row_groups):
        stats = md.row_group(rg).column(ci).statistics
        if stats is None or not stats.has_min_max:
            return None
        out.add(stats.min)
        out.add(stats.max)
    return out


# ── per-date merge / atomic write ────────────────────────────────────────────
def _write_atomic(table: pa.Table, dst_file) -> None:
    """Write ``table`` to ``dst_file`` atomically (``.tmp`` then ``os.replace``),
    emitting one row group per contiguous underlying. ``table`` must already be
    sorted by ``underlying`` so equal-underlying rows are contiguous."""
    dst_file = pathlib.Path(dst_file)
    dst_file.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst_file.with_name(dst_file.name + ".tmp")
    und = table.column("underlying").to_pylist()
    n = len(und)
    with pq.ParquetWriter(tmp, table.schema) as writer:
        if n == 0:
            writer.write_table(table)
        else:
            start = 0
            for i in range(1, n + 1):
                if i == n or und[i] != und[start]:
                    writer.write_table(table.slice(start, i - start))
                    start = i
    os.replace(tmp, dst_file)


def _merge_date(src_files, dst_file) -> int:
    """Read every per-symbol source file for one date, validate each against the
    canonical schema (hard error on drift), concatenate, sort by
    ``(underlying, symbol)`` and write atomically. Returns the row count."""
    tables = []
    for f in src_files:
        t = pq.read_table(f)
        _validate_schema(t.schema, f)
        # cast strips any per-file metadata so schemas are byte-identical for concat
        tables.append(t.cast(CANONICAL_SCHEMA))
    merged = pa.concat_tables(tables)
    order = pc.sort_indices(
        merged, sort_keys=[("underlying", "ascending"), ("symbol", "ascending")]
    )
    merged = merged.take(order)
    _write_atomic(merged, dst_file)
    return merged.num_rows


# ── enumeration + orchestration ──────────────────────────────────────────────
def _enumerate_by_date(src: pathlib.Path, date_lo, date_hi) -> Dict[str, List[pathlib.Path]]:
    """Group ``src/<underlying>/<date>.parquet`` files by date (inclusive range).
    ISO dates compare correctly as strings."""
    by_date: Dict[str, List[pathlib.Path]] = {}
    if not src.is_dir():
        return by_date
    for sym_dir in sorted(p for p in src.iterdir() if p.is_dir()):
        for pqf in sorted(sym_dir.glob("*.parquet")):
            date = pqf.stem
            if date_lo is not None and date < date_lo:
                continue
            if date_hi is not None and date > date_hi:
                continue
            by_date.setdefault(date, []).append(pqf)
    return by_date


def migrate(src, dst, date_lo=None, date_hi=None, dry_run=False) -> MigrationStats:
    """Migrate the v1 tree at ``src`` to the v2 hive at ``dst``.

    ``date_lo``/``date_hi`` are inclusive ``YYYY-MM-DD`` bounds (either may be
    ``None``). ``dry_run`` computes and records the plan without writing anything
    (no date files, no manifest)."""
    src = pathlib.Path(src)
    dst = pathlib.Path(dst)
    by_date = _enumerate_by_date(src, date_lo, date_hi)

    stats = MigrationStats()
    for date in sorted(by_date):
        src_files = by_date[date]
        stats.n_dates += 1
        stats.n_source_files += len(src_files)
        dst_file = dst / f"date={date}" / DATA_FILE
        src_unds = {f.parent.name for f in src_files}  # v1 dir name == underlying

        # Skip when the destination already holds a superset of the source's
        # underlyings (footer-only check).
        if dst_file.exists():
            dst_unds = _footer_underlyings(dst_file)
            if dst_unds is not None and dst_unds >= src_unds:
                n_rows = pq.ParquetFile(dst_file).metadata.num_rows
                stats.n_skipped += 1
                stats.results.append(DateResult(date, len(src_files), n_rows, "skipped"))
                continue

        if dry_run:
            n_rows = sum(pq.ParquetFile(f).metadata.num_rows for f in src_files)
            stats.results.append(DateResult(date, len(src_files), n_rows, "planned"))
            continue

        n_rows = _merge_date(src_files, dst_file)
        stats.n_written += 1
        stats.n_rows += n_rows
        stats.results.append(DateResult(date, len(src_files), n_rows, "written"))

    if not dry_run:
        stats.manifest_path = _write_manifest(dst, stats.results)
    return stats


def _write_manifest(dst: pathlib.Path, results: List[DateResult]) -> str:
    dst.mkdir(parents=True, exist_ok=True)
    ts = datetime.datetime.now().strftime("%Y%m%dT%H%M%S_%f")
    path = dst / f"migration_manifest_{ts}.csv"
    with open(path, "w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(["date", "n_source_files", "n_rows", "status"])
        for r in results:
            writer.writerow([r.date, r.n_source_files, r.n_rows, r.status])
    return str(path)


# ── CLI ──────────────────────────────────────────────────────────────────────
def _print_plan(stats: MigrationStats, dry_run: bool) -> None:
    verb = "PLAN (dry-run, nothing written)" if dry_run else "MIGRATION"
    print(f"OPRA hive v1 -> v2 {verb}")
    for r in stats.results:
        print(f"  date={r.date}  files={r.n_source_files:>4}  rows={r.n_rows:>9}  {r.status}")
    print(
        f"dates={stats.n_dates} written={stats.n_written} "
        f"skipped={stats.n_skipped} rows={stats.n_rows}"
    )
    if stats.manifest_path:
        print(f"manifest: {stats.manifest_path}")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Migrate OPRA hive v1 (<symbol>/<date>.parquet) to v2 "
        "(date=<date>/data.parquet). Pure local IO, $0, idempotent.",
    )
    p.add_argument("--src", required=True, help="v1 hive root (<symbol>/<date>.parquet)")
    p.add_argument("--dst", required=True, help="v2 hive root (date=<date>/data.parquet)")
    p.add_argument("--from", dest="date_lo", default=None, help="inclusive lower date YYYY-MM-DD")
    p.add_argument("--to", dest="date_hi", default=None, help="inclusive upper date YYYY-MM-DD")
    p.add_argument("--dry-run", action="store_true", help="print the plan, write nothing")
    args = p.parse_args(argv)

    stats = migrate(
        args.src, args.dst, date_lo=args.date_lo, date_hi=args.date_hi, dry_run=args.dry_run
    )
    _print_plan(stats, args.dry_run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
