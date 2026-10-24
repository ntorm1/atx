#!/usr/bin/env python3
"""Partition a SpiderRock OptionIntradayHist_30min parquet drop into the oracle store layout.

Input : one DuckDB-written parquet (62 columns; `date` = 30-min slice instant, UTC;
        `okey_dt` = expiry date; float32 vendor columns; -99 sentinels).
Output: <out>/date=<tradingDate>/bucket_et=<HHMM>/data_*.parquet with the column set and
        types pinned by atx-vol/scripts/oracle_store_metadata.py (float64 numerics, int64
        counts/keys, `okey_yr/mn/dy`, `bucket`, `bucket_et`, `moneyness`) plus the extra
        SpiderRock columns this drop carries (`exType`, `prcFramework`, `modelType`, `synSpot`,
        `srSlope`, `ddivPv`, `yearsC`, `sDaysT`, `sDaysE`, `okey_dt`, `undSecKey_dt`).
        Strings are written as parquet `string` (utf8), not `large_string`; readers in
        atx-vol accept both. Every -99 vendor sentinel becomes NULL. bucket_et is the
        America/New_York wall-clock HHMM of the slice instant (DST-safe).
        A manifest `oracle_manifest_<date>.json` is written next to the date directory.

Usage: python -I spiderrock_intraday_to_store.py --parquet <file> --out <store> [--threads 8]
       [--memory 8GB] [--temp <dir>] [--buckets 1000,1030,...]
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import shutil
import sys
import time
from pathlib import Path

import duckdb

SENTINEL_COLS = (
    "bidIV", "askIV", "srPrc", "srVol", "de", "ga", "th", "ve", "rh", "ph", "vo", "va",
    "srSlope", "deDecay", "sdiv", "ddiv", "ddivPv", "rate", "years", "yearsC", "atmVol", "error",
)
DOUBLE_COLS = (
    "okey_xx", "uBid", "uAsk", "uPrc", "synSpot", "bidPrc", "askPrc", "bidPrice2", "askPrice2",
    "bidIV", "askIV", "srPrc", "srVol", "de", "ga", "th", "ve", "rh", "ph", "vo", "va", "srSlope",
    "deDecay", "sdiv", "ddiv", "ddivPv", "rate", "years", "yearsC", "atmVol", "error",
)
INT64_COLS = (
    "bidSz", "askSz", "cumBidSize", "cumAskSize", "bidMask", "askMask", "cumBidSize2",
    "cumAskSize2", "prtVolume", "sDaysT", "sDaysE",
)
STRING_COLS = (
    "okey_at", "okey_ts", "okey_tk", "okey_cp", "tradingSession", "undSecKey_at", "undSecKey_ts",
    "undSecKey_tk", "undSecType", "securityID", "bidExch", "askExch", "modelType",
    "prcFramework", "exType",
)
TS_FMT = "%Y-%m-%d %H:%M:%S.%f"


def numeric_expr(col: str) -> str:
    if col in SENTINEL_COLS:
        return f"CASE WHEN {col} = -99 THEN NULL ELSE CAST({col} AS DOUBLE) END AS {col}"
    return f"CAST({col} AS DOUBLE) AS {col}"


def build_select(src: str, buckets: list[str] | None) -> str:
    cols: list[str] = []
    cols += [f"CAST({c} AS VARCHAR) AS {c}" for c in STRING_COLS]
    cols += [
        "CAST(year(okey_dt) AS BIGINT) AS okey_yr",
        "CAST(month(okey_dt) AS BIGINT) AS okey_mn",
        "CAST(day(okey_dt) AS BIGINT) AS okey_dy",
        "strftime(okey_dt, '%Y-%m-%d') AS okey_dt",
        "CAST(year(undSecKey_dt) AS BIGINT) AS undSecKey_yr",
        "CAST(month(undSecKey_dt) AS BIGINT) AS undSecKey_mn",
        "CAST(day(undSecKey_dt) AS BIGINT) AS undSecKey_dy",
        "strftime(undSecKey_dt, '%Y-%m-%d') AS undSecKey_dt",
        "strftime(tradingDate, '%Y-%m-%d') AS tradingDate",
        f"strftime(CAST(date AT TIME ZONE 'UTC' AS TIMESTAMP), '{TS_FMT}') AS date",
        "CAST(epoch_us(date) AS BIGINT) AS date_us",
        f"strftime(CAST(\"timestamp\" AT TIME ZONE 'UTC' AS TIMESTAMP), '{TS_FMT}') AS timestamp",
        "CAST(epoch_us(\"timestamp\") AS BIGINT) AS timestamp_us",
        f"strftime(CAST(date AT TIME ZONE 'America/Chicago' AS TIMESTAMP), '{TS_FMT}') AS date_cst",
        f"strftime(CAST(\"timestamp\" AT TIME ZONE 'America/Chicago' AS TIMESTAMP), '{TS_FMT}') AS timestamp_cst",
        "CAST(date AT TIME ZONE 'UTC' AS TIMESTAMP) AS bucket",
        "strftime(CAST(date AT TIME ZONE 'America/New_York' AS TIMESTAMP), '%H%M') AS bucket_et",
        "CASE WHEN uPrc IS NULL OR uPrc <= 0 THEN NULL ELSE CAST(okey_xx AS DOUBLE) / CAST(uPrc AS DOUBLE) END AS moneyness",
    ]
    cols += [numeric_expr(c) for c in DOUBLE_COLS]
    cols += [f"CAST({c} AS BIGINT) AS {c}" for c in INT64_COLS]
    where = ""
    if buckets:
        quoted = ",".join(f"'{b}'" for b in buckets)
        where = (f" WHERE strftime(CAST(date AT TIME ZONE 'America/New_York' AS TIMESTAMP), '%H%M')"
                 f" IN ({quoted})")
    return f"SELECT {', '.join(cols)} FROM read_parquet('{src}'){where}"


def sql_path(p: Path) -> str:
    return str(p).replace("\\", "/").replace("'", "''")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--parquet", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--memory", default="8GB")
    ap.add_argument("--temp", default=None)
    ap.add_argument("--buckets", default=None, help="comma list of HHMM ET slices; default all")
    args = ap.parse_args()

    src = Path(args.parquet).resolve()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    con = duckdb.connect()
    con.execute(f"SET threads={args.threads}; SET memory_limit='{args.memory}';")
    con.execute("SET TimeZone='UTC'; SET preserve_insertion_order=false;")
    if args.temp:
        tmp = Path(args.temp)
        tmp.mkdir(parents=True, exist_ok=True)
        con.execute(f"SET temp_directory='{sql_path(tmp)}';")

    dates = [r[0] for r in con.execute(
        f"SELECT DISTINCT tradingDate FROM read_parquet('{sql_path(src)}')").fetchall()]
    if len(dates) != 1:
        print(f"expected one tradingDate, got {dates}", file=sys.stderr)
        return 1
    trading_date = dates[0].isoformat()
    date_dir = out / f"date={trading_date}"
    if date_dir.exists():
        print(f"refusing to overwrite existing {date_dir}", file=sys.stderr)
        return 1
    tmp_dir = out / f".ingest-{trading_date}"
    if tmp_dir.exists():
        shutil.rmtree(tmp_dir)
    buckets = [b.strip() for b in args.buckets.split(",")] if args.buckets else None
    select = build_select(sql_path(src), buckets)
    t0 = time.perf_counter()
    print(f"ingest: {src} ({src.stat().st_size / 2**30:.2f} GiB) -> {date_dir}", flush=True)
    con.execute(
        f"COPY ({select}) TO '{sql_path(tmp_dir)}' (FORMAT PARQUET, PARTITION_BY (bucket_et), "
        f"WRITE_PARTITION_COLUMNS TRUE, COMPRESSION ZSTD, FILENAME_PATTERN 'data_{{i}}', "
        f"ROW_GROUP_SIZE 262144)")
    tmp_dir.rename(date_dir)
    t1 = time.perf_counter()
    print(f"ingest: partitioned write done in {t1 - t0:.1f} s", flush=True)

    glob = sql_path(date_dir / "**" / "*.parquet")
    per_bucket = con.execute(
        f"SELECT bucket_et, count(*) FROM read_parquet('{glob}') GROUP BY 1 ORDER BY 1").fetchall()
    top = con.execute(
        f"SELECT undSecKey_tk, count(*) AS n FROM read_parquet('{glob}') GROUP BY 1 "
        f"ORDER BY n DESC, undSecKey_tk LIMIT 50").fetchall()
    manifest = {
        "trading_date": trading_date,
        "source_tsv_bytes": src.stat().st_size,
        "total_rows": int(sum(n for _, n in per_bucket)),
        "buckets": {b: int(n) for b, n in per_bucket},
        "top_underliers_by_rows": {u: int(n) for u, n in top},
        "ingested_at": dt.datetime.now(dt.timezone.utc).isoformat(),
    }
    (out / f"oracle_manifest_{trading_date}.json").write_text(json.dumps(manifest, indent=2))
    total_bytes = sum(p.stat().st_size for p in date_dir.rglob("*.parquet"))
    print(f"store: {len(per_bucket)} buckets, {manifest['total_rows']:,} rows, "
          f"{total_bytes / 2**30:.2f} GiB on disk, {time.perf_counter() - t0:.1f} s total")
    for b, n in per_bucket:
        print(f"  bucket_et={b} {n:>10,}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
