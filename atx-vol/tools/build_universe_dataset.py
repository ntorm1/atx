#!/usr/bin/env python3
"""Build a LABEL-FREE universe quote dataset (the dev200 layout) from one SpiderRock board.

WHY. Every sprint number is measured on dev200 / dev200b / cohort5 (200 + 200 + 5 underliers).
Robustness across "a wide array of underlying assets" needs the WHOLE board in the quote-bench
layout -- for fit speed, an input census and the refusal taxonomy only. A universe dataset
necessarily contains the holdout underliers, so it is allowed ONLY label-free (vol-sota PM
decision 4, 2026-10-20): no labels.parquet, no srPrc / srVol anywhere, never scored, never used
for tuning.

HOW. The adapter is quote_benchmark.py's own ``normalize_quotes`` (imported from the sibling
file, never copied), fed exactly its raw-input allowlist ``INPUT_COLUMNS``: the parquet reader is
asked for those columns and no other, so a label column cannot reach this process even when the
source carries one. ``quote_benchmark.py prepare`` cannot be driven label-free -- it requires the
label columns in its source and writes labels.parquet unconditionally. This tool writes
quotes.parquet, quotes.csv (the native runner's input) and manifest.json only, in the shape
``verify_dataset`` accepts, so ``quote_benchmark.py run`` and ``shape_cohort.py build`` consume
the result unchanged. Row ids are the adapter's (the source row index), so a cohort prepared from
the same source is a row-for-row subset of the universe (only ``split`` differs).

``split`` is written as the constant "universe": the dataset must not mark which underliers are
holdout members. A row's ``input_status`` is whatever the adapter sets (unsupported_exercise for
European roots, invalid_* for bad quotes); nothing is filtered silently.

Usage:
  python atx-vol/tools/build_universe_dataset.py \
      --source C:/Users/<you>/Downloads/OptionIntradayHist_30min_2026-10-14.parquet \
      --output C:/atx-cache/quote-benchmark/universe-20261014 --buckets 1030
  # a sub-universe (e.g. a stratified sample roster, one underlier per line)
  python atx-vol/tools/build_universe_dataset.py --source <board> --output <folder>/sample-1pct \
      --buckets 1030 --symbols-file roster.txt --tag draw_seed=L5u-1pct-v1
"""
from __future__ import annotations

import argparse
from collections import Counter
import datetime as dt
import hashlib
import importlib.util
import json
from pathlib import Path
import time
import zoneinfo

import polars as pl
import pyarrow.csv as pacsv
import pyarrow.parquet as pq


TOOL_SHA256 = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
# Columns that must never be readable or writable through this tool.
LABEL_BLOCKLIST = ("srPrc", "srVol", "reference_price", "reference_vol")
SPLIT_LABEL = "universe"
NEW_YORK = zoneinfo.ZoneInfo("America/New_York")
# normalize_quotes needs these to compute a split that this tool then overwrites.
ADAPTER_SPLIT_FRACTION, ADAPTER_SPLIT_SEED = 0.2, "atx-vol-v1"


def load_adapter():
    """quote_benchmark.py beside this file: the one adapter, imported, never copied."""
    path = Path(__file__).resolve().with_name("quote_benchmark.py")
    spec = importlib.util.spec_from_file_location("quote_benchmark", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_buckets(text: str) -> list[str]:
    buckets = sorted(set(filter(None, (t.strip() for t in text.split(",")))))
    if not buckets:
        raise ValueError("--buckets needs at least one New York HHMM bucket, e.g. 1030")
    for token in buckets:
        if len(token) != 4:
            raise ValueError("buckets use four-digit New York HHMM")
        dt.datetime.strptime(token, "%H%M")
    return buckets


def parse_tags(items: list[str]) -> dict[str, str]:
    tags: dict[str, str] = {}
    for item in items:
        key, sep, value = item.partition("=")
        if not sep or not key:
            raise ValueError(f"--tag wants KEY=VALUE, got {item!r}")
        tags[key] = value
    return tags


def read_symbols(args: argparse.Namespace, sha256) -> tuple[set[str] | None, dict]:
    names = set(filter(None, (s.strip() for s in args.symbols.split(","))))
    provenance: dict = {"requested_symbols": sorted(names), "symbols_file": None}
    if args.symbols_file is not None:
        lines = args.symbols_file.read_text(encoding="utf-8").splitlines()
        listed = [line.strip() for line in lines if line.strip() and not line.startswith("#")]
        if not listed:
            raise ValueError(f"{args.symbols_file}: no underliers listed")
        names.update(listed)
        provenance["symbols_file"] = {"path": str(args.symbols_file.resolve()),
                                      "sha256": sha256(args.symbols_file),
                                      "underliers": len(set(listed))}
    return (names or None), provenance


def _stamp(ns: int) -> dict:
    when = dt.datetime.fromtimestamp(ns / 1e9, dt.timezone.utc)
    return {"utc": when.isoformat(), "new_york": when.astimezone(NEW_YORK).isoformat()}


def build(args: argparse.Namespace) -> dict:
    qb = load_adapter()
    if args.batch_size < 1 or args.bucket_minutes < 1 or 60 % args.bucket_minutes:
        raise ValueError("batch size must be positive; bucket minutes must divide 60")
    buckets = parse_buckets(args.buckets)
    tags = parse_tags(args.tag)
    symbols, symbol_provenance = read_symbols(args, qb.sha256)
    columns = list(qb.INPUT_COLUMNS)  # the ONLY columns ever requested from the source
    if set(columns) & set(LABEL_BLOCKLIST):
        raise ValueError("adapter input allowlist overlaps the label blocklist")
    source_path = args.source.resolve()
    source = pq.ParquetFile(source_path)
    missing = set(columns) - set(source.schema_arrow.names)
    if missing:
        raise ValueError(f"source missing input columns: {sorted(missing)}")
    source_stat = source_path.stat()
    started = time.perf_counter()
    source_hash = qb.sha256(source_path)
    args.output.mkdir(parents=True, exist_ok=False)
    quote_path, csv_path = args.output / "quotes.parquet", args.output / "quotes.csv"
    bucket_mask = (pl.col("snapshot_ns").cast(pl.Datetime("ns", "UTC"))
                   .dt.truncate(f"{args.bucket_minutes}m")
                   .dt.convert_time_zone("America/New_York")
                   .dt.strftime("%H%M").is_in(buckets))
    counts: Counter = Counter()
    names: set[str] = set()
    chains: set[int] = set()
    snap_min = snap_max = None
    offset = 0
    quote_writer = pq.ParquetWriter(quote_path, qb.QUOTE_SCHEMA, compression="zstd")
    csv_writer = pacsv.CSVWriter(csv_path, qb.QUOTE_SCHEMA)
    try:
        for raw in source.iter_batches(columns=columns, batch_size=args.batch_size):
            norm = qb.normalize_quotes(raw, offset, bucket_minutes=args.bucket_minutes,
                                       validation_fraction=ADAPTER_SPLIT_FRACTION,
                                       split_seed=ADAPTER_SPLIT_SEED)
            offset += raw.num_rows
            norm = norm.filter(bucket_mask)
            if symbols is not None:
                norm = norm.filter(pl.col("underlier").is_in(sorted(symbols)))
            if norm.is_empty():
                continue
            norm = norm.with_columns(pl.lit(SPLIT_LABEL, dtype=pl.String).alias("split"))
            table = norm.to_arrow().cast(qb.QUOTE_SCHEMA)
            if set(table.schema.names) & set(LABEL_BLOCKLIST):
                raise ValueError("a label column reached the output table")
            quote_writer.write_table(table)
            csv_writer.write_table(table)
            names.update(norm["underlier"].drop_nulls().unique().to_list())
            chains.update(norm["chain_id"].unique().to_list())
            counts.update(norm["input_status"].to_list())
            lo, hi = norm["snapshot_ns"].min(), norm["snapshot_ns"].max()
            snap_min = lo if snap_min is None else min(snap_min, lo)
            snap_max = hi if snap_max is None else max(snap_max, hi)
    finally:
        quote_writer.close()
        csv_writer.close()
    current = source_path.stat()
    if (source_stat.st_size, source_stat.st_mtime_ns) != (current.st_size, current.st_mtime_ns):
        raise ValueError("source changed during the build; dataset is incomplete")
    if not counts:
        raise ValueError("selection is empty: no row matched the buckets / symbols")
    written = set(pq.read_schema(quote_path).names)
    if written & set(LABEL_BLOCKLIST) or written != set(qb.QUOTE_SCHEMA.names):
        raise ValueError("written quote schema is not the fixed label-free allowlist")
    if (args.output / "labels.parquet").exists():
        raise ValueError("a labels.parquet appeared in a label-free dataset folder")
    rows = sum(counts.values())
    manifest = {
        "schema_version": qb.SCHEMA_VERSION,
        "dataset_kind": "universe-label-free",
        "source": {"path": str(source_path), "sha256": source_hash,
                   "bytes": source_stat.st_size, "mtime_ns": source_stat.st_mtime_ns,
                   "rows": source.metadata.num_rows,
                   "row_groups": source.metadata.num_row_groups,
                   "columns_read": columns},
        "adapter": {"file": "quote_benchmark.py", "function": "normalize_quotes",
                    "sha256": qb.ADAPTER_SHA256},
        "builder": {"file": "build_universe_dataset.py", "sha256": TOOL_SHA256},
        "input_allowlist": columns,
        "label_columns_read": [],
        "label_columns_blocked": list(LABEL_BLOCKLIST),
        "labels_parquet": None,
        "label_isolation": ("the parquet reader is asked for INPUT_COLUMNS only; "
                            "srPrc / srVol / reference_* can be neither read nor written; "
                            "no labels.parquet exists; never scored, never used for tuning"),
        "holdout_statement": ("the universe contains every listed underlier, holdout members "
                              "included; split is the constant 'universe' so membership is not "
                              "marked; label-free by construction"),
        "quote_columns": qb.QUOTE_SCHEMA.names,
        "selection": {"bucket_minutes": args.bucket_minutes, "buckets_new_york": buckets,
                      **symbol_provenance, "selected_underliers": sorted(names),
                      "underliers": len(names), "chains": len(chains),
                      "split_label": SPLIT_LABEL},
        "snapshot": {"first": _stamp(snap_min), "last": _stamp(snap_max)},
        "rows_selected": rows,
        "rows_by_input_status": dict(sorted(counts.items())),
        "rows_by_split": {SPLIT_LABEL: rows},
        "files": {p.name: {"sha256": qb.sha256(p), "bytes": p.stat().st_size}
                  for p in (quote_path, csv_path)},
        "tags": tags,
        "preparation_seconds": time.perf_counter() - started,
        "time_convention": "years is variance time; years_calendar is carry/discount time",
        "dividend_convention": "aggregate cash amount/PV retained; event timing is unavailable",
    }
    qb.write_json(args.output / "manifest.json", manifest)
    print(json.dumps({"dataset": str(args.output), "rows": rows, "underliers": len(names),
                      "chains": len(chains), "input_status": manifest["rows_by_input_status"],
                      "labels": None, "seconds": manifest["preparation_seconds"]}))
    return manifest


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", required=True, type=Path, help="SpiderRock OptionIntradayHist parquet")
    ap.add_argument("--output", required=True, type=Path, help="new folder (refuses an existing one)")
    ap.add_argument("--buckets", required=True,
                    help="comma-separated New York HHMM snapshot buckets, e.g. 1030")
    ap.add_argument("--bucket-minutes", type=int, default=30)
    ap.add_argument("--symbols", default="", help="optional comma-separated underliers")
    ap.add_argument("--symbols-file", type=Path, help="optional roster, one underlier per line")
    ap.add_argument("--batch-size", type=int, default=131072)
    ap.add_argument("--tag", action="append", default=[],
                    help="KEY=VALUE provenance recorded under manifest.tags (repeatable)")
    return ap


def main(argv: list[str] | None = None) -> int:
    build(parser().parse_args(argv))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
