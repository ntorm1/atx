#!/usr/bin/env python
"""SpiderRock tbloptionintradayhist ingest: zip -> partitioned parquet oracle store.

One-time per daily drop. Streaming end to end: the ~15 GB TSV is extracted once,
lazily scanned with polars, transformed, and sunk to parquet without ever
materializing the full frame. Spec:
docs/superpowers/specs/2026-08-15-oracle-rsi-loop-design.md

Transforms
  - drop the 9:30 America/New_York slice (DST-safe; the `date` column is UTC)
  - bucket = timestamp truncated to 5 minutes (groups quote-time drift), stored
    as UTC datetime plus a human `bucket_et` HH:MM string used for partitioning
  - -99 sentinels -> null on the columns known to use them
  - derived moneyness = strike / uPrc

Output layout
  <out>/date=<tradingDate>/bucket_et=<HHMM>/*.parquet
  <out>/oracle_manifest_<tradingDate>.json   (row counts, top underliers)

Usage
  python atx-vol/scripts/oracle_ingest.py --zip <path-to-tbloptionintraday-zip>
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import shutil
import sys
import zipfile
from pathlib import Path

import polars as pl

# Columns observed using -99 as a missing-value sentinel. Extend as discovered;
# never blanket-apply (-99 is a legal value for nothing seen so far, but strikes
# and signed greeks deserve caution).
SENTINEL_COLS = ("bidIV", "askIV", "error")

EXTRACT_HEADROOM = 1.05  # extracted TSV + slack
PARQUET_HEADROOM = 6 * 2**30  # generous bound for the compressed store


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--zip", required=True, type=Path, help="tbloptionintradayhist zip")
    p.add_argument("--out", type=Path, default=Path(r"C:\atx-cache\oracle\spiderrock"))
    p.add_argument("--work", type=Path, default=Path(r"C:\atx-cache\oracle\raw"), help="transient TSV extract dir")
    p.add_argument("--keep-txt", action="store_true", help="keep the extracted TSV")
    return p.parse_args()


def check_disk(zf: zipfile.ZipFile, work: Path, out: Path) -> int:
    member = zf.infolist()[0]
    need_work = int(member.file_size * EXTRACT_HEADROOM)
    for path, need in ((work, need_work), (out, PARQUET_HEADROOM)):
        path.mkdir(parents=True, exist_ok=True)
        free = shutil.disk_usage(path).free
        if free < need:
            sys.exit(f"ERROR: {path} has {free / 2**30:.1f} GiB free, need {need / 2**30:.1f} GiB")
    return member.file_size


def extract(zf: zipfile.ZipFile, work: Path) -> Path:
    member = zf.infolist()[0]
    dest = work / Path(member.filename).name
    if dest.exists() and dest.stat().st_size == member.file_size:
        print(f"extract: reusing existing {dest}")
        return dest
    print(f"extract: {member.filename} -> {dest} ({member.file_size / 2**30:.1f} GiB)")
    with zf.open(member) as src, open(dest, "wb") as sink:
        shutil.copyfileobj(src, sink, length=16 * 2**20)
    return dest


def build_lazyframe(tsv: Path) -> pl.LazyFrame:
    lf = pl.scan_csv(
        tsv,
        separator="\t",
        infer_schema_length=10_000,
        null_values={c: "-99" for c in SENTINEL_COLS},
        schema_overrides={
            "okey_xx": pl.Float64,  # fractional strikes exist
            "date": pl.Utf8,
            "timestamp": pl.Utf8,
            "securityID": pl.Utf8,
        },
    )
    ts = pl.col("timestamp").str.to_datetime("%Y-%m-%d %H:%M:%S%.f", time_unit="us")
    slice_et = (
        pl.col("date")
        .str.to_datetime("%Y-%m-%d %H:%M:%S%.f", time_unit="us")
        .dt.replace_time_zone("UTC")
        .dt.convert_time_zone("America/New_York")
    )
    bucket = ts.dt.truncate("5m")
    return (
        lf.with_columns(slice_et.alias("slice_et"), ts.alias("ts"))
        # drop the opening 9:30 ET slice, DST-safe
        .filter(~((pl.col("slice_et").dt.hour() == 9) & (pl.col("slice_et").dt.minute() == 30)))
        .with_columns(
            bucket.alias("bucket"),
            bucket.dt.replace_time_zone("UTC")
            .dt.convert_time_zone("America/New_York")
            .dt.strftime("%H%M")
            .alias("bucket_et"),
            (pl.col("okey_xx") / pl.col("uPrc")).alias("moneyness"),
        )
        .drop("slice_et", "ts")
    )


def sink_partitioned(lf: pl.LazyFrame, out: Path, trading_date: str) -> None:
    base = out / f"date={trading_date}"
    try:  # polars >= 1.21: single-pass partitioned sink
        lf.sink_parquet(pl.PartitionByKey(str(base), by=["bucket_et"]), compression="zstd")
        return
    except (AttributeError, TypeError):
        pass
    # Fallback: one streaming pass per bucket (a dozen buckets; one-time cost).
    buckets = lf.select("bucket_et").unique().collect(engine="streaming")["bucket_et"].sort().to_list()
    for b in buckets:
        dest = base / f"bucket_et={b}"
        dest.mkdir(parents=True, exist_ok=True)
        lf.filter(pl.col("bucket_et") == b).sink_parquet(dest / "data.parquet", compression="zstd")
        print(f"sink: bucket_et={b} done")


def write_manifest(out: Path, trading_date: str, tsv_bytes: int) -> None:
    store = out / f"date={trading_date}"
    lf = pl.scan_parquet(str(store / "**" / "*.parquet"))
    per_bucket = lf.group_by("bucket_et").len().sort("bucket_et").collect(engine="streaming")
    top = (
        lf.group_by("undSecKey_tk").len().sort("len", descending=True).head(50).collect(engine="streaming")
    )
    manifest = {
        "trading_date": trading_date,
        "source_tsv_bytes": tsv_bytes,
        "total_rows": int(per_bucket["len"].sum()),
        "buckets": {r["bucket_et"]: r["len"] for r in per_bucket.iter_rows(named=True)},
        "top_underliers_by_rows": {r["undSecKey_tk"]: r["len"] for r in top.iter_rows(named=True)},
        "ingested_at": dt.datetime.now(dt.timezone.utc).isoformat(),
    }
    path = out / f"oracle_manifest_{trading_date}.json"
    path.write_text(json.dumps(manifest, indent=2))
    print(f"manifest: {path}")
    print("top underliers (pick smoke/tune/holdout cohorts from these):")
    for name, rows in list(manifest["top_underliers_by_rows"].items())[:15]:
        print(f"  {name:8s} {rows:>10,}")


def main() -> None:
    args = parse_args()
    with zipfile.ZipFile(args.zip) as zf:
        tsv_bytes = check_disk(zf, args.work, args.out)
        tsv = extract(zf, args.work)
    lf = build_lazyframe(tsv)
    trading_date = lf.select("tradingDate").first().collect(engine="streaming").item()
    print(f"ingest: tradingDate={trading_date}")
    sink_partitioned(lf, args.out, trading_date)
    write_manifest(args.out, trading_date, tsv_bytes)
    if not args.keep_txt:
        tsv.unlink()
        print(f"cleanup: removed {tsv}")


if __name__ == "__main__":
    main()
