#!/usr/bin/env python3
"""Transcode one SpiderRock oracle-store bucket into an OPRA hive v2 date slice.

The oracle store (`atx-vol/scripts/oracle_ingest.py`, rooted at
C:/atx-cache/oracle/spiderrock) is the only market data we hold for some
sessions -- the OPRA hive at C:/atx-data/opra-hive simply has no file for them.
Everything downstream that FITS a surface reads the OPRA hive v2 schema, and
`atx::vol::load_spiderrock_parquet` is deliberately NotImplemented
(atx-vol/src/marketdata/data.cpp:582). So the cheapest honest bridge is a
transcode at the file boundary rather than a second loader inside the library:

    <store>/date=<D>/bucket_et=<HHMM>/*.parquet
        -> <out-hive>/date=<D>/data.parquet          (the option board)
        -> <out-underlier>/date=<D>/underlier.parquet (the underlier NBBO feed)

Both outputs carry the exact physical schemas the existing readers demand
(verified against C:/atx-data/opra-hive/date=2026-08-21/data.parquet and
C:/atx-data/underlier-hive/date=2026-08-14/underlier.parquet): prices are int64
fixed-point 1e-9 dollars with INT64_MIN as UNSET, and `symbol` is the 21-char
OSI string `atx::vol::parse_osi_symbol` reads back
(atx-vol/src/marketdata/opra_panel.cpp:427).

    python atx-vol/tools/spiderrock_to_opra_hive.py \
        --store C:/atx-cache/oracle/spiderrock --date 2026-08-14 \
        --bucket-et 1030 --symbols KMX,MRNA,SPY,IBM \
        --out-hive C:/atx-cache/sr-hive --out-underlier C:/atx-cache/sr-underlier

## The snapshot instant, and why it is stamped from the bucket

`ts` is written as the bucket instant (`bucket_et` read as ET, converted to
UTC), identically on every row, because the OPRA hive schema stamps ONE `ts` per
date file and `panel_from_scan` treats that column as the ground-truth valuation
instant -- it will hard-refuse a `--snapshot-suffix` that names a different one
(opra_panel.cpp:803). A vendor bucket is NOT an instant: each row carries its own
last-update stamp scattered across and past the minute (2026-08-14 bucket_et=1030
spans 14:30:32.986565 .. 14:31:03.420926). Collapsing that onto the bucket is the
same approximation the store's own partitioning already makes, and it is stated
here rather than hidden. The matching CLI stamp is printed on completion.

## What is carried and what is dropped

CARRIED: strike, expiry, side, underlier ticker, option bid/ask and sizes,
underlier bid/ask.

DROPPED, because the OPRA hive v2 schema has nowhere to put them: the vendor's
own `rate`, `sdiv`, `ddiv` and `years`. A consumer refits carry from the board
(put-call parity) and applies its own flat `--r`; it does NOT inherit
SpiderRock's carry inputs. That is a real divergence from the vendor's pricing
environment and any comparison against `srVol` inherits it -- as it also
inherits SpiderRock's hybrid vol-time clock for `years`, which our calendar tau
does not reproduce.

SYNTHESISED: `instrument_id` is a dense counter. It is optional to the loader
(opra_panel.cpp:407) and no Databento id exists for a vendor row; it is emitted
only so the file matches the hive schema column-for-column.
"""

from __future__ import annotations

import argparse
import datetime as dt
import pathlib
import sys

import polars as pl
import pyarrow as pa
import pyarrow.parquet as pq

INT64_MIN = -(2**63)

# The eight hive-v2 columns in the order the real files carry them.
HIVE_COLS = ["ts", "underlying", "symbol", "instrument_id",
             "bid_px", "ask_px", "bid_sz", "ask_sz"]
UNDERLIER_COLS = ["ts", "underlying", "instrument_id",
                  "bid_px", "ask_px", "bid_sz", "ask_sz"]


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--store", type=pathlib.Path,
                    default=pathlib.Path(r"C:\atx-cache\oracle\spiderrock"),
                    help="oracle store root holding date=<D>/bucket_et=<HHMM>")
    ap.add_argument("--date", required=True, help="YYYY-MM-DD trading session")
    ap.add_argument("--bucket-et", required=True, help="HHMM, e.g. 1030")
    ap.add_argument("--symbols", required=True,
                    help="comma-joined undSecKey_tk universe")
    ap.add_argument("--out-hive", type=pathlib.Path, required=True,
                    help="OPRA hive v2 root to write date=<D>/data.parquet under")
    ap.add_argument("--out-underlier", type=pathlib.Path, required=True,
                    help="underlier hive root to write date=<D>/underlier.parquet under")
    return ap.parse_args(argv)


def bucket_instant_utc(date: str, bucket_et: str) -> dt.datetime:
    """The bucket's ET wall-clock read as UTC-naive, the way the store means it.

    The store's `bucket` column is already UTC-naive (the vendor `date` column is
    UTC and ingest truncates it), so this must agree with it or the snapshot
    guard downstream will refuse the pair. Verified: bucket_et=1030 on
    2026-08-14 carries bucket == 2026-08-14 14:30:00.
    """
    if len(bucket_et) != 4 or not bucket_et.isdigit():
        raise SystemExit(f"--bucket-et must be 4 digits, got {bucket_et!r}")
    et = dt.datetime.strptime(f"{date} {bucket_et}", "%Y-%m-%d %H%M")
    try:
        from zoneinfo import ZoneInfo
    except ImportError as exc:  # pragma: no cover - stdlib since 3.9
        raise SystemExit(f"zoneinfo unavailable: {exc}") from exc
    return et.replace(tzinfo=ZoneInfo("America/New_York")).astimezone(dt.UTC).replace(tzinfo=None)


def to_fixed_point(col: str) -> pl.Expr:
    """Dollars -> int64 1e-9, with null/negative becoming the UNSET sentinel."""
    px = pl.col(col)
    return (pl.when(px.is_null() | px.is_nan() | (px < 0.0))
              .then(pl.lit(INT64_MIN, dtype=pl.Int64))
              .otherwise((px * 1_000_000_000.0).round(0).cast(pl.Int64))
              .alias(col))


def osi_symbol() -> pl.Expr:
    """`okey_*` -> the 21-char OSI string, root left-justified into six chars.

    The reader takes the LAST 15 chars as the fixed field and trims whatever
    precedes it as the root, so the padding is cosmetic to it -- it is written
    because every real hive file we hold is padded that way.
    """
    root = pl.col("okey_tk").str.slice(0, 6).str.pad_end(6, " ")
    yy = (pl.col("okey_yr") % 100).cast(pl.Int64).cast(pl.Utf8).str.pad_start(2, "0")
    mm = pl.col("okey_mn").cast(pl.Int64).cast(pl.Utf8).str.pad_start(2, "0")
    dd = pl.col("okey_dy").cast(pl.Int64).cast(pl.Utf8).str.pad_start(2, "0")
    cp = pl.when(pl.col("okey_cp").str.to_lowercase().str.starts_with("p")).then(pl.lit("P")).otherwise(pl.lit("C"))
    strike = ((pl.col("okey_xx") * 1000.0).round(0).cast(pl.Int64)
              .cast(pl.Utf8).str.pad_start(8, "0"))
    return pl.concat_str([root, yy, mm, dd, cp, strike]).alias("symbol")


def write_hive_parquet(frame: pl.DataFrame, path: pathlib.Path) -> None:
    """Write with utf8 (not large_utf8) string columns.

    polars' own writer emits LARGE_STRING, and atx-core's Arrow reader refuses
    it outright -- `atx-vol-chain-export` dies with
    `error: strings: LARGE_STRING not supported` before reading a row. The real
    hive files do not have this problem, so the transcode must match them: go
    through pyarrow and pin every string column to `pa.string()`.
    """
    table = frame.to_arrow()
    fields = [pa.field(f.name, pa.string() if pa.types.is_large_string(f.type) else f.type)
              for f in table.schema]
    pq.write_table(table.cast(pa.schema(fields)), path)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    symbols = [s.strip() for s in args.symbols.split(",") if s.strip()]
    if not symbols:
        raise SystemExit("--symbols named nothing")

    part = args.store / f"date={args.date}" / f"bucket_et={args.bucket_et}"
    if not part.is_dir():
        raise SystemExit(f"no such store partition: {part}")
    files = sorted(part.glob("*.parquet"))
    if not files:
        raise SystemExit(f"store partition holds no parquet: {part}")

    ts = bucket_instant_utc(args.date, args.bucket_et)

    src = (pl.scan_parquet(files)
             .filter(pl.col("undSecKey_tk").is_in(symbols))
             .select(["undSecKey_tk", "okey_tk", "okey_yr", "okey_mn", "okey_dy",
                      "okey_xx", "okey_cp", "bidPrc", "askPrc", "bidSz", "askSz",
                      "uBid", "uAsk"])
             .collect())
    if src.height == 0:
        raise SystemExit(f"no rows for {symbols} in {part}")

    missing = sorted(set(symbols) - set(src["undSecKey_tk"].unique().to_list()))
    if missing:
        print(f"WARNING: no rows for {','.join(missing)}", file=sys.stderr)

    board = (src.with_columns([osi_symbol(),
                               to_fixed_point("bidPrc"), to_fixed_point("askPrc")])
                .with_columns([
                    pl.lit(ts).cast(pl.Datetime("ns")).alias("ts"),
                    pl.col("undSecKey_tk").alias("underlying"),
                    pl.col("bidPrc").alias("bid_px"),
                    pl.col("askPrc").alias("ask_px"),
                    pl.col("bidSz").fill_null(0).cast(pl.Int64).clip(lower_bound=0).alias("bid_sz"),
                    pl.col("askSz").fill_null(0).cast(pl.Int64).clip(lower_bound=0).alias("ask_sz"),
                ])
                .sort(["underlying", "symbol"]))
    board = board.with_columns(
        pl.int_range(0, board.height, dtype=pl.Int64).alias("instrument_id")
    ).select(HIVE_COLS)

    # One underlier row per ticker. The vendor repeats uBid/uAsk on every option
    # row of a name; they are identical within the bucket by construction, and a
    # disagreement would mean the bucket mixes two underlier states -- so take
    # the first and report the spread of what was collapsed.
    und_spread = (src.group_by("undSecKey_tk")
                     .agg([(pl.col("uBid").max() - pl.col("uBid").min()).alias("ubid_span"),
                           (pl.col("uAsk").max() - pl.col("uAsk").min()).alias("uask_span")]))
    under = (src.group_by("undSecKey_tk", maintain_order=True)
                .agg([pl.col("uBid").first(), pl.col("uAsk").first()])
                .with_columns([to_fixed_point("uBid"), to_fixed_point("uAsk")])
                .with_columns([
                    pl.lit(ts).cast(pl.Datetime("ns")).alias("ts"),
                    pl.col("undSecKey_tk").alias("underlying"),
                    pl.col("uBid").alias("bid_px"),
                    pl.col("uAsk").alias("ask_px"),
                    pl.lit(0, dtype=pl.Int64).alias("bid_sz"),
                    pl.lit(0, dtype=pl.Int64).alias("ask_sz"),
                ])
                .sort("underlying"))
    under = under.with_columns(
        pl.int_range(0, under.height, dtype=pl.Int64).alias("instrument_id")
    ).select(UNDERLIER_COLS)

    hive_path = args.out_hive / f"date={args.date}" / "data.parquet"
    und_path = args.out_underlier / f"date={args.date}" / "underlier.parquet"
    hive_path.parent.mkdir(parents=True, exist_ok=True)
    und_path.parent.mkdir(parents=True, exist_ok=True)
    write_hive_parquet(board, hive_path)
    write_hive_parquet(under, und_path)

    print(f"wrote {board.height} option rows -> {hive_path}")
    print(f"wrote {under.height} underlier rows -> {und_path}")
    print(f"snapshot ts = {ts.isoformat()}Z  "
          f"(pass --snapshot-suffix T{ts.strftime('%H:%M:%S')}Z)")
    for row in und_spread.sort("undSecKey_tk").iter_rows(named=True):
        print(f"  {row['undSecKey_tk']}: uBid span {row['ubid_span']:.6f}, "
              f"uAsk span {row['uask_span']:.6f}")
    n_unset = int((board["bid_px"] == INT64_MIN).sum() + (board["ask_px"] == INT64_MIN).sum())
    print(f"  unset option quote sides: {n_unset}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
