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
that check reads only parquet footer metadata, never the data.

When the destination exists and is only a PARTIAL overlap with the source (not a
superset), the date is never rebuilt from the v1 source alone — the destination
is read, validated against ``CANONICAL_SCHEMA``, and rewritten as the UNION of
its rows and the source's. **Conflict precedence: the destination wins.** For any
underlying present in both, the destination's rows are kept and the source's are
dropped — the destination may hold freshly pulled, paid Databento data and the
v1 source is by definition the older tree. The skip-when-superset case above is
just the special case of this same rule where the source contributes nothing new.
The union is validated and swapped in atomically (``.tmp`` + ``os.replace``); if
it fails validation the destination is left untouched and the date is reported as
an error, never partially written.

A destination that exists but cannot be read or validated is NEVER rewritten —
forcing a rebuild from the v1 source in that case is the least safe option, not
the safe one (it would silently discard everything the unreadable file held).
Such a date is skipped, counted, and reported as an error with the path and the
reason.

Every run drops a ``migration_manifest_<ts>.csv`` (date, n_source_files, n_rows,
status, detail).

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
    # "written"      -- fresh write, no destination existed
    # "skipped"      -- destination already a superset of the source (no rewrite)
    # "merged"       -- destination existed as a partial overlap; rewritten as the
    #                   union (destination wins conflicts) -- NOT the same outcome
    #                   as a fresh "written"
    # "error"        -- destination exists but could not be read or validated;
    #                   refused and left untouched (see ``detail``)
    # "planned"      -- dry-run: would be "written"
    # "planned_merge"-- dry-run: would be "merged"
    status: str
    detail: str = ""  # path/reason for "error"; empty otherwise


@dataclass
class MigrationStats:
    n_dates: int = 0
    n_written: int = 0
    n_skipped: int = 0
    n_merged: int = 0
    n_errors: int = 0
    n_source_files: int = 0
    n_rows: int = 0
    manifest_path: Optional[str] = None
    results: List[DateResult] = field(default_factory=list)


# ── schema / footer helpers ──────────────────────────────────────────────────
def _validate_schema(schema: pa.Schema, path) -> None:
    """Fail closed on any drift from the frozen canonical schema.

    The column contract is (name, type) in order -- that is what the C++ loaders
    validate and all that changes how bytes are read. Nullability is arrow
    metadata: the real v1 corpus was written with every field ``not null`` while
    ``CANONICAL_SCHEMA`` declares them nullable, and ``pa.Schema.equals`` counts
    that as drift, which rejected every genuine source file. Compare names and
    types so real drift (a renamed, reordered, missing or retyped column) still
    fails closed.
    """
    if list(schema.names) != list(CANONICAL_SCHEMA.names) or list(schema.types) != list(
        CANONICAL_SCHEMA.types
    ):
        raise ValueError(
            f"schema drift in {path}: file schema does not match the canonical "
            f"OPRA v2 schema.\n  expected: {CANONICAL_SCHEMA}\n  got:      {schema}"
        )


def _footer_underlyings(path) -> Optional[Set[str]]:
    """Distinct ``underlying`` values in a destination date file — exact for
    any writer, not just the one this tool uses.

    Fast path: when every row group holds a single underlying (min == max ==
    that value), footer statistics give the full set with no data scan. But
    the one-row-group-per-underlying layout ``_write_atomic`` produces is a
    convention of this tool, not a frozen invariant of every file this
    function might be asked about — a hand-written or future destination file
    could pack several underlyings into one sorted row group. In that case
    min/max are only the two extremes and every name sorting strictly between
    them is invisible to a naive {min, max} union, which makes the union a
    strict SUBSET of the true set.

    That undercount is not cosmetic. ``migrate()`` treats this return value as
    "what the destination already has" and skips the source merge only when it
    is a superset of the source's underlyings. An undercounted destination set
    can spuriously fail that superset test even though the true destination
    already covers the source, and when it does, control falls through to
    ``_merge_date``, which rebuilds the date file from the v1 sources ALONE
    and ``os.replace``s it over the destination — silently discarding every
    underlying the destination had that is absent from the v1 tree (for
    example, anything a paid Databento pull added there). So when a row
    group's statistics are absent, lack min/max, or have ``min != max``, fall
    back to a single-column full read of ``underlying`` for the whole file —
    still far cheaper than reading the data columns, and correct regardless of
    row-group layout. Do not "optimize" this fallback away: the failure mode
    it prevents is not a wrong skip, it is a destructive rewrite.

    Returns ``None`` only when the file cannot be opened, or has no
    ``underlying`` column, or the fallback read itself fails — every one of
    those is "cannot determine the destination's set" and the caller treats
    ``None`` as "cannot prove superset", forcing a rewrite. That is the safe
    direction: an empty ``set()`` would also fail the superset test and force
    the same rewrite, but without honestly saying the check couldn't be done,
    so a read failure must never be reported as ``set()``.
    """
    try:
        pf = pq.ParquetFile(path)
        names = pf.schema_arrow.names
        if "underlying" not in names:
            return None
        ci = names.index("underlying")
        md = pf.metadata
        out: Set[str] = set()
        for rg in range(md.num_row_groups):
            stats = md.row_group(rg).column(ci).statistics
            if stats is None or not stats.has_min_max or stats.min != stats.max:
                return set(pf.read(columns=["underlying"]).column(0).to_pylist())
            out.add(stats.min)
        return out
    except Exception:  # noqa: BLE001 — unreadable file/footer -> force rewrite,
        # never silently degrade to set() (see docstring)
        return None


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


def _merge_union_date(dst_file, add_files) -> int:
    """Merge ``add_files`` (v1 per-symbol sources for underlyings the
    destination does not already have -- caller has already excluded any
    underlying present in the destination) into the existing ``dst_file``,
    validate the union against ``CANONICAL_SCHEMA``, sort by
    ``(underlying, symbol)``, and swap it in atomically.

    Conflict precedence: the destination wins. Because the caller pre-filters
    ``add_files`` to only the underlyings missing from the destination, no
    source row for an underlying the destination already holds is ever read
    into the union -- the destination's own rows for that underlying are used
    as-is, never re-derived from the (possibly older) v1 copy.

    Reads everything into memory and validates before calling
    ``_write_atomic`` -- ``dst_file`` on disk is never touched until every
    input has validated, so any exception raised here (unreadable/corrupt
    destination, schema drift in the destination or a source file, or a
    union that fails validation) always leaves ``dst_file`` byte-identical to
    what it was before the call. The caller is responsible for catching that
    exception, treating the date as an error, and not retrying the write.

    Returns the total row count of the written union."""
    # NOTE: read the destination via ``ParquetFile.read()``, NOT
    # ``pq.read_table(path)`` -- the destination lives under a
    # ``date=YYYY-MM-DD`` directory, and ``pq.read_table`` infers Hive-style
    # partitioning from a ``key=value`` path segment even for a single file,
    # silently appending a spurious ``date`` dictionary column that then
    # fails canonical-schema validation. ``ParquetFile.read()`` reads the
    # file's actual on-disk schema only (same convention already used by
    # ``_footer_underlyings`` for this same reason).
    dst_table = pq.ParquetFile(dst_file).read()
    _validate_schema(dst_table.schema, dst_file)
    tables = [dst_table.cast(CANONICAL_SCHEMA)]
    for f in add_files:
        t = pq.read_table(f)
        _validate_schema(t.schema, f)
        tables.append(t.cast(CANONICAL_SCHEMA))
    merged = pa.concat_tables(tables)
    _validate_schema(merged.schema, dst_file)  # the union itself, before any write
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

        if dst_file.exists():
            # Footer-only check: never a data scan just to classify the date.
            dst_unds = _footer_underlyings(dst_file)

            if dst_unds is None:
                # Cannot prove ANYTHING about the destination's contents -- do
                # NOT force a rewrite (that is the destructive direction, not
                # the safe one). Refuse, count, report; destination untouched.
                stats.n_errors += 1
                reason = f"destination exists but is unreadable: {dst_file}"
                stats.results.append(
                    DateResult(date, len(src_files), 0, "error", detail=reason)
                )
                print(f"ERROR date={date}: {reason}", file=sys.stderr)
                continue

            if dst_unds >= src_unds:
                # Destination already a superset -- skip without a rewrite.
                n_rows = pq.ParquetFile(dst_file).metadata.num_rows
                stats.n_skipped += 1
                stats.results.append(DateResult(date, len(src_files), n_rows, "skipped"))
                continue

            # Partial overlap: rewrite as the union. Destination wins any
            # conflict, so only source files for underlyings the destination
            # does NOT already have are read at all.
            add_files = [f for f in src_files if f.parent.name not in dst_unds]

            if dry_run:
                try:
                    dst_pf = pq.ParquetFile(dst_file)
                    _validate_schema(dst_pf.schema_arrow, dst_file)  # metadata only
                    # The real run (_merge_union_date) validates every add_file's
                    # schema too, not just the destination's -- a dry-run that
                    # skipped this could report a clean "planned_merge" for a date
                    # whose v1 source has schema drift, when the real run would
                    # actually raise inside _merge_union_date and refuse the date.
                    # Predict the same outcome here, still metadata-only (no data
                    # scan, consistent with the rest of this footer-only check).
                    add_pfs = [pq.ParquetFile(f) for f in add_files]
                    for f, add_pf in zip(add_files, add_pfs):
                        _validate_schema(add_pf.schema_arrow, f)
                except Exception as exc:  # noqa: BLE001 — cannot validate -> error, not a plan
                    stats.n_errors += 1
                    reason = f"would fail schema validation, real run would refuse: {exc}"
                    stats.results.append(
                        DateResult(date, len(src_files), 0, "error", detail=reason)
                    )
                    print(f"ERROR date={date}: {reason}", file=sys.stderr)
                    continue
                n_rows = dst_pf.metadata.num_rows + sum(
                    add_pf.metadata.num_rows for add_pf in add_pfs
                )
                stats.results.append(
                    DateResult(date, len(src_files), n_rows, "planned_merge")
                )
                continue

            try:
                n_rows = _merge_union_date(dst_file, add_files)
            except Exception as exc:  # noqa: BLE001 — leave dst untouched, report, move on
                stats.n_errors += 1
                reason = f"union merge failed, destination left untouched: {exc}"
                stats.results.append(
                    DateResult(date, len(src_files), 0, "error", detail=reason)
                )
                print(f"ERROR date={date}: {reason}", file=sys.stderr)
                continue

            stats.n_merged += 1
            stats.n_rows += n_rows
            stats.results.append(DateResult(date, len(src_files), n_rows, "merged"))
            continue

        # No destination at all -- fresh write.
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
        writer.writerow(["date", "n_source_files", "n_rows", "status", "detail"])
        for r in results:
            writer.writerow([r.date, r.n_source_files, r.n_rows, r.status, r.detail])
    return str(path)


# ── CLI ──────────────────────────────────────────────────────────────────────
def _print_plan(stats: MigrationStats, dry_run: bool) -> None:
    verb = "PLAN (dry-run, nothing written)" if dry_run else "MIGRATION"
    print(f"OPRA hive v1 -> v2 {verb}")
    for r in stats.results:
        suffix = f"  ({r.detail})" if r.detail else ""
        print(f"  date={r.date}  files={r.n_source_files:>4}  rows={r.n_rows:>9}  "
              f"{r.status}{suffix}")
    print(
        f"dates={stats.n_dates} written={stats.n_written} merged={stats.n_merged} "
        f"skipped={stats.n_skipped} errors={stats.n_errors} rows={stats.n_rows}"
    )
    if stats.manifest_path:
        print(f"manifest: {stats.manifest_path}")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description="Migrate OPRA hive v1 (<symbol>/<date>.parquet) to v2 "
        "(date=<date>/data.parquet). Pure local IO, $0, idempotent. A destination "
        "that already exists is never rebuilt from the v1 source alone: a partial "
        "overlap is rewritten as the UNION of destination + source rows, and the "
        "DESTINATION WINS any conflicting underlying (it may hold paid data newer "
        "than the v1 tree). A destination that cannot be read or validated is "
        "never rewritten -- it is refused, counted, and reported as an error.",
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
