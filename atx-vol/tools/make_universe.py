#!/usr/bin/env python3
"""Stage a reduced multi-name OPRA universe for the universe-cycle bench (U6).

`bench/universe_cycle_bench.cpp`'s real-cohort row (BM_UniverseCycle_Real) drives
the LANDED populate_surface_db pipeline (U1 streaming release + U2 LPT claim order
+ U3 date-granular durability + U4 shared small-book budget, over the E2 work-
stealing pool) on real OPRA boards. Its ingest is `atx::vol::load_opra_daterange`,
which reads a parquet hive at `<root>/{symbol}/{date}.parquet` — and the raw OPRA
CBBO day files under C:/atx-data/spy-dispersion/opra/<SYM>/<DATE>.parquet are
ALREADY single-minute, single-underlying snapshots in exactly the loader's 8-column
schema (ts, underlying, symbol, instrument_id, bid_px, ask_px, bid_sz, ask_sz).

So this tool is a validated project+filter+copy per (symbol, date): it selects the
loader's columns in order (dropping anything else) and writes each board under the
worktree's own `data/opra_universe/` tree, so the cohort is a REPRODUCIBLE artifact
kept inside the worktree (never committed — regenerate from the raw pull). It also
writes a newline-delimited symbol-list txt the bench/driver can read.

The bench itself needs NO staged copy — you can point ATX_UNIVERSE_OPRA_ROOT
straight at the raw pull. This tool exists so the proof is reproducible from a
committed generator and so the cohort lives beside the bench per the worktree
data-locality rule.

Example (the 11-name x 3-date north-star smoke cohort):

    python atx-vol/tools/make_universe.py \
        --src-root C:/atx-data/spy-dispersion/opra \
        --out-root data/opra_universe \
        --symbols AAPL,AMZN,AVGO,GOOGL,JPM,LLY,META,MSFT,NVDA,SPY,XOM \
        --dates 2026-01-02,2026-01-05,2026-01-06 \
        --symlist smoke11.txt

Then run the bench against the staged cohort:

    ATX_UNIVERSE_OPRA_ROOT=data/opra_universe \
    ATX_UNIVERSE_SYMBOLS=AAPL,AMZN,AVGO,GOOGL,JPM,LLY,META,MSFT,NVDA,SPY,XOM \
    ATX_UNIVERSE_DATE_LO=2026-01-02 ATX_UNIVERSE_DATE_HI=2026-01-06 \
    ATX_UNIVERSE_WORKERS=6 \
    atx-vol-universe-cycle-bench --benchmark_filter=universe/cycle/real
"""

from __future__ import annotations

import argparse
import os
import sys

import pyarrow.parquet as pq

# Columns the C++ loader consumes, in a stable order (see make_fit_slice.py).
# instrument_id/underlying are optional to the loader but preserved when present.
_WANTED = ["ts", "underlying", "symbol", "instrument_id", "bid_px", "ask_px", "bid_sz", "ask_sz"]
_REQUIRED = ["ts", "symbol", "bid_px", "ask_px", "bid_sz", "ask_sz"]

_DEFAULT_SYMBOLS = "AAPL,AMZN,AVGO,GOOGL,JPM,LLY,META,MSFT,NVDA,SPY,XOM"
_DEFAULT_DATES = "2026-01-02,2026-01-05,2026-01-06"


def _split(csv: str) -> list[str]:
    return [x.strip() for x in csv.split(",") if x.strip()]


def stage_one(src: str, dst: str) -> tuple[int, int]:
    """Project the loader's columns from one raw OPRA day file into dst. Returns
    (rows, n_expiries)."""
    table = pq.read_table(src)
    cols = set(table.column_names)
    missing = [c for c in _REQUIRED if c not in cols]
    if missing:
        raise SystemExit(f"ERROR: {src} missing required columns {missing}; has {sorted(cols)}")
    keep = [c for c in _WANTED if c in table.column_names]
    slice_tbl = table.select(keep)
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    pq.write_table(slice_tbl, dst)
    syms = slice_tbl.column("symbol").to_pylist()
    expiries = {s[-15:-9] for s in syms if len(s) >= 15}
    return slice_tbl.num_rows, len(expiries)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--src-root", default="C:/atx-data/spy-dispersion/opra",
                    help="raw OPRA hive root: <root>/<SYM>/<DATE>.parquet")
    ap.add_argument("--out-root", default="data/opra_universe",
                    help="destination hive root (kept inside the worktree)")
    ap.add_argument("--symbols", default=_DEFAULT_SYMBOLS, help="CSV underliers")
    ap.add_argument("--dates", default=_DEFAULT_DATES, help="CSV YYYY-MM-DD dates")
    ap.add_argument("--symlist", default="smoke11.txt",
                    help="newline-delimited symbol-list file written under out-root")
    args = ap.parse_args(argv)

    symbols = _split(args.symbols)
    dates = _split(args.dates)
    if not symbols or not dates:
        print("ERROR: empty --symbols or --dates", file=sys.stderr)
        return 2

    total_rows = 0
    n_boards = 0
    n_missing = 0
    for sym in symbols:
        for date in dates:
            src = os.path.join(args.src_root, sym, f"{date}.parquet")
            dst = os.path.join(args.out_root, sym, f"{date}.parquet")
            if not os.path.exists(src):
                print(f"  MISSING {src}", file=sys.stderr)
                n_missing += 1
                continue
            rows, n_exp = stage_one(src, dst)
            total_rows += rows
            n_boards += 1
            print(f"  {sym:6s} {date}  rows={rows:6d}  expiries={n_exp}")

    symlist_path = os.path.join(args.out_root, args.symlist)
    os.makedirs(os.path.dirname(os.path.abspath(symlist_path)), exist_ok=True)
    with open(symlist_path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(symbols) + "\n")

    print(f"\nstaged {n_boards} boards ({total_rows} quote rows, {n_missing} missing) "
          f"under {args.out_root}")
    print(f"symbol list: {symlist_path}  ({len(symbols)} symbols x {len(dates)} dates)")
    return 0 if n_boards > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
