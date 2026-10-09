#!/usr/bin/env python3
"""Regenerate an atx-vol SPY fit-slice parquet from a raw OPRA CBBO day file.

A "fit slice" is a single-minute option-chain cross-section that the C++ loader
`atx::vol::load_opra_cbbo_parquet` (atx-vol/src/opra_panel.cpp) reads: the whole
file is treated as ONE snapshot minute. Its schema is identical to the raw OPRA
CBBO pull produced by atx-core `pull_opra_cbbo_1m_to_parquet`:

    ts            timestamp[ns]   one snapshot minute (many rows)
    underlying    string          e.g. "SPY"
    symbol        string          OSI/OCC 21-char option symbol
    instrument_id int64           Databento instrument id (optional to loader)
    bid_px        int64           fixed-point 1e-9 dollars; UNSET = INT64_MIN
    ask_px        int64
    bid_sz        int64
    ask_sz        int64

The raw OPRA day files under C:/atx-data/spy-dispersion/opra/<SYM>/<DATE>.parquet
are already single-minute snapshots in exactly this schema, so this tool is
essentially a validated project+filter+copy: it selects one underlying and one
timestamp minute, keeps the loader's eight columns in order, and writes the slice
under the fixture name the bench/tests probe (data/spy_fit_slices/).

Example (the E1 wall-win proof slice):

    python atx-vol/tools/make_fit_slice.py \
        --src C:/atx-data/spy-dispersion/opra/SPY/2026-01-02.parquet \
        --out data/spy_fit_slices/SPY_2026-01-02T1955Z.parquet \
        --underlying SPY

The snapshot minute is auto-selected (the single / most-populated `ts`) unless
`--ts` pins it. `find_spy_fit_parquet` resolves `data/spy_fit_slices/<name>` when
a bench runs with cwd = repo/worktree root.
"""

from __future__ import annotations

import argparse
import os
import sys

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

# Columns the C++ loader consumes, in a stable order. instrument_id/underlying
# are optional to the loader but preserved when present (strict-provenance path).
_WANTED = ["ts", "underlying", "symbol", "instrument_id", "bid_px", "ask_px", "bid_sz", "ask_sz"]
_REQUIRED = ["ts", "symbol", "bid_px", "ask_px", "bid_sz", "ask_sz"]


def _osi_expiry(symbol: str) -> str:
    """YYYY-MM-DD from the OSI fixed field (last 15 chars: YYMMDD C/P strike8)."""
    fixed = symbol[-15:]
    return "20%s-%s-%s" % (fixed[0:2], fixed[2:4], fixed[4:6])


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True, help="raw OPRA CBBO day parquet")
    ap.add_argument("--out", required=True, help="destination slice parquet")
    ap.add_argument("--underlying", default="SPY", help="underlying to keep (default SPY)")
    ap.add_argument("--ts", default=None,
                    help="snapshot minute to select (e.g. '2026-01-02 19:55:00'); "
                         "default = the single / most-populated ts in the file")
    args = ap.parse_args(argv)

    table = pq.read_table(args.src)
    cols = set(table.column_names)
    missing = [c for c in _REQUIRED if c not in cols]
    if missing:
        print(f"ERROR: source missing required columns {missing}; has {sorted(cols)}", file=sys.stderr)
        return 2

    n_in = table.num_rows

    # 1) underlying filter (only if the column exists; the loader tolerates its absence)
    if "underlying" in cols and args.underlying:
        mask = pc.equal(table.column("underlying"), pa.scalar(args.underlying))
        table = table.filter(mask)
        if table.num_rows == 0:
            print(f"ERROR: no rows for underlying '{args.underlying}'", file=sys.stderr)
            return 2

    # 2) select exactly one snapshot minute (loader treats the whole file as one)
    ts_col = table.column("ts")
    uniq = pc.unique(ts_col)
    if args.ts is not None:
        want = pa.scalar(args.ts, type=ts_col.type) if ts_col.type != pa.string() else pa.scalar(args.ts)
        table = table.filter(pc.equal(ts_col, want))
        if table.num_rows == 0:
            print(f"ERROR: no rows at ts '{args.ts}'; available: {uniq.to_pylist()[:8]}", file=sys.stderr)
            return 2
        chosen = args.ts
    elif len(uniq) == 1:
        chosen = uniq.to_pylist()[0]
    else:
        # Most-populated minute wins (a full-chain snapshot, not a thin tail).
        vc = pc.value_counts(ts_col)
        counts = vc.field("counts").to_pylist()
        values = vc.field("values").to_pylist()
        chosen = values[counts.index(max(counts))]
        table = table.filter(pc.equal(ts_col, pa.scalar(chosen, type=ts_col.type)))

    # 3) project the loader's columns, in order, dropping anything else
    keep = [c for c in _WANTED if c in table.column_names]
    slice_tbl = table.select(keep)

    # Diagnostics: expiries kept (matches the C++ OSI parse used downstream).
    syms = slice_tbl.column("symbol").to_pylist()
    expiries = sorted({_osi_expiry(s) for s in syms if len(s) >= 15})

    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    pq.write_table(slice_tbl, args.out)

    print(f"wrote {args.out}")
    print(f"  underlying={args.underlying}  ts={chosen}")
    print(f"  rows: {n_in} -> {slice_tbl.num_rows}  expiries={len(expiries)}  "
          f"[{expiries[0]} .. {expiries[-1]}]")
    print(f"  columns: {keep}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
