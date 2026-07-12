#!/usr/bin/env python3
"""Pull one OPRA cbbo-1m snapshot minute for a whole symbol universe into the
per-symbol parquet hive that atx-vol `load_opra_daterange` reads
({symbol}/{date}.parquet with columns ts, underlying, symbol, bid_px, ask_px,
bid_sz, ask_sz; px int64 1e-9 fixed point, unset = INT64_MIN).

Cost discipline mirrors atx-core databento_bulk_opra:
  1. FREE preflight first (metadata.get_cost + get_record_count) over every
     chunk of parent symbols. No billable egress.
  2. HARD CAP: estimated total > --cap => refuse, exit 3, nothing pulled.
  3. --dry-run prints the plan + estimate and exits 0 without pulling.
  4. Raw DBN chunks are cached under <out>/_dbn/; a present chunk is decoded
     from disk instead of re-billed, so re-splitting is free and idempotent.

Usage:
  python pull_opra_universe_snapshot.py --symbols-file data/universe/r3000_proxy_symbols.txt \
      --date 2026-07-01 --snap-utc 14:00 --out data/opra_universe [--limit 10] \
      [--cap 25] [--chunk 250] [--dry-run]
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import sys
import time

import databento as db
import numpy as np
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq

INT64_MIN = np.iinfo(np.int64).min
DBN_UNDEF = np.iinfo(np.int64).max  # databento UNDEF_PRICE
DATASET = "OPRA.PILLAR"
SCHEMA = "cbbo-1m"

ARROW_SCHEMA = pa.schema([
    ("ts", pa.timestamp("ns")),
    ("underlying", pa.string()),
    ("symbol", pa.string()),
    ("bid_px", pa.int64()),
    ("ask_px", pa.int64()),
    ("bid_sz", pa.int64()),
    ("ask_sz", pa.int64()),
])


def read_api_key(env_path: pathlib.Path = pathlib.Path(".env")) -> str:
    for line in env_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("DATABENTO_API_KEY"):
            return line.split("=", 1)[1].strip().strip('"').strip("'")
    raise SystemExit("DATABENTO_API_KEY not found in .env")


def to_parent(sym: str) -> str:
    return sym.replace(".", "") + ".OPT"


def chunked(seq: list[str], n: int) -> list[list[str]]:
    return [seq[i:i + n] for i in range(0, len(seq), n)]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--symbols-file", type=pathlib.Path, required=True)
    ap.add_argument("--date", default="2026-07-01")
    ap.add_argument("--snap-utc", default="14:00", help="HH:MM UTC minute to pull")
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("data/opra_universe"))
    ap.add_argument("--cap", type=float, default=25.0)
    ap.add_argument("--chunk", type=int, default=250)
    ap.add_argument("--limit", type=int, default=0, help="use only the first N symbols")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--skip-preflight", action="store_true",
                    help="skip the free cost preflight (use only when the account's "
                         "flat-rate license makes the estimate $0 and the metadata "
                         "gateway is flaky)")
    ap.add_argument("--force", action="store_true", help="rewrite existing parquet files")
    args = ap.parse_args()

    symbols = [s.strip() for s in args.symbols_file.read_text().splitlines() if s.strip()]
    if args.limit > 0:
        symbols = symbols[: args.limit]
    root_to_sym = {s.replace(".", ""): s for s in symbols}

    start = f"{args.date}T{args.snap_utc}:00"
    hh, mm = args.snap_utc.split(":")
    end_minute = int(hh) * 60 + int(mm) + 1
    end = f"{args.date}T{end_minute // 60:02d}:{end_minute % 60:02d}:00"
    print(f"universe={len(symbols)} window=[{start}Z,{end}Z) dataset={DATASET} schema={SCHEMA}")

    key = read_api_key()
    client = db.Historical(key=key)
    chunks = chunked(symbols, args.chunk)

    # ── FREE preflight ────────────────────────────────────────────────────────
    total_cost, total_records = 0.0, 0
    per_chunk: list[tuple[float, int]] = []
    for ci, chunk in enumerate(chunks if not args.skip_preflight else []):
        parents = [to_parent(s) for s in chunk]
        for attempt in range(4):
            try:
                cost = client.metadata.get_cost(
                    dataset=DATASET, symbols=parents, schema=SCHEMA,
                    start=start, end=end, stype_in="parent")
                break
            except Exception as e:  # noqa: BLE001
                if attempt == 3:
                    raise
                print(f"  preflight chunk {ci} retry after: {str(e)[:100]}", file=sys.stderr)
                time.sleep(3 * (attempt + 1))
        per_chunk.append((cost, 0))
        total_cost += cost
        print(f"  preflight chunk {ci + 1}/{len(chunks)} ({len(chunk)} syms): est=${cost:.4f}")

    print(f"PREFLIGHT TOTAL: est=${total_cost:.4f} (cap ${args.cap:.2f})")
    if total_cost > args.cap:
        print(f"REFUSED: estimate ${total_cost:.4f} exceeds cap ${args.cap:.2f}. Nothing pulled.",
              file=sys.stderr)
        return 3
    if args.dry_run:
        print("DRY RUN: no data pulled.")
        return 0

    # ── Pull chunks (DBN cached on disk) and split to the per-symbol hive ────
    dbn_dir = args.out / "_dbn"
    dbn_dir.mkdir(parents=True, exist_ok=True)
    manifest_rows: list[dict] = []
    n_files, n_empty_syms, n_unmapped = 0, 0, 0
    seen_syms: set[str] = set()
    failed_chunks: list[int] = []

    for ci, chunk in enumerate(chunks):
        parents = [to_parent(s) for s in chunk]
        # Content-addressed cache name: the same (date, minute, symbol set) is
        # reusable across runs with different --chunk / --limit boundaries.
        digest = hashlib.sha256(",".join(chunk).encode()).hexdigest()[:12]
        dbn_path = dbn_dir / (f"{args.date}_{args.snap_utc.replace(':', '')}_"
                              f"chunk{ci:03d}_{digest}.dbn.zst")
        if dbn_path.exists():
            store = db.DBNStore.from_file(dbn_path)
            print(f"  chunk {ci + 1}/{len(chunks)}: cached {dbn_path.name}")
        else:
            store = None
            for attempt in range(6):
                try:
                    store = client.timeseries.get_range(
                        dataset=DATASET, symbols=parents, schema=SCHEMA,
                        start=start, end=end, stype_in="parent")
                    break
                except Exception as e:  # noqa: BLE001
                    print(f"  chunk {ci} pull retry {attempt + 1} after: {str(e)[:100]}",
                          file=sys.stderr)
                    time.sleep(min(10 * 2 ** attempt, 120))
            if store is None:
                # Leave this chunk for a later idempotent rerun (no dbn cache file).
                print(f"  chunk {ci + 1}/{len(chunks)}: FAILED after retries — skipping",
                      file=sys.stderr)
                failed_chunks.append(ci)
                continue
            store.to_file(dbn_path)
            print(f"  chunk {ci + 1}/{len(chunks)}: pulled -> {dbn_path.name}")

        df = store.to_df(price_type="fixed", pretty_ts=False, map_symbols=True)
        if df.empty:
            continue
        df = df.reset_index()
        # Constant snapshot stamp (naive UTC wall time), matching the reference
        # files the C++ puller writes (every row carries the bar boundary).
        snap_ts = pd.Timestamp(f"{args.date}T{args.snap_utc}:00")
        out = pd.DataFrame({
            "ts": pd.Series([snap_ts] * len(df), dtype="datetime64[ns]"),
            "symbol": df["symbol"].astype(str),
            "bid_px": df["bid_px_00"].astype("int64"),
            "ask_px": df["ask_px_00"].astype("int64"),
            "bid_sz": df["bid_sz_00"].astype("int64"),
            "ask_sz": df["ask_sz_00"].astype("int64"),
        })
        # UNDEF -> loader sentinel
        out.loc[out["bid_px"] == DBN_UNDEF, "bid_px"] = INT64_MIN
        out.loc[out["ask_px"] == DBN_UNDEF, "ask_px"] = INT64_MIN
        # OSI root = first 6 chars (space padded); map back to the universe symbol.
        roots = out["symbol"].str[:6].str.strip()
        base = roots.str.replace(r"\d+$", "", regex=True)
        out["underlying"] = base.map(root_to_sym)
        unmapped = out["underlying"].isna()
        if unmapped.any():
            n_unmapped += int(unmapped.sum())
            out = out[~unmapped]

        for sym, grp in out.groupby("underlying", sort=False):
            seen_syms.add(str(sym))
            tgt = args.out / str(sym) / f"{args.date}.parquet"
            if tgt.exists() and not args.force:
                manifest_rows.append({"symbol": sym, "records": len(grp), "status": "exists"})
                continue
            tgt.parent.mkdir(parents=True, exist_ok=True)
            tbl = pa.Table.from_pandas(
                grp[["ts", "underlying", "symbol", "bid_px", "ask_px", "bid_sz", "ask_sz"]],
                schema=ARROW_SCHEMA, preserve_index=False)
            pq.write_table(tbl, tgt)
            n_files += 1
            manifest_rows.append({"symbol": sym, "records": len(grp), "status": "ok"})

    failed_syms: set[str] = set()
    for ci in failed_chunks:
        failed_syms.update(chunks[ci])
    for sym in symbols:
        if sym in failed_syms:
            manifest_rows.append({"symbol": sym, "records": 0, "status": "chunk_failed"})
        elif sym not in seen_syms:
            n_empty_syms += 1
            manifest_rows.append({"symbol": sym, "records": 0, "status": "no_options"})

    mpath = args.out / f"manifest_{args.date}_{args.snap_utc.replace(':', '')}.csv"
    pd.DataFrame(manifest_rows).to_csv(mpath, index=False)
    print(f"\nDONE files={n_files} symbols_with_data={len(seen_syms)} "
          f"no_options={n_empty_syms} unmapped_records={n_unmapped} "
          f"failed_chunks={len(failed_chunks)}")
    print(f"manifest: {mpath}")
    return 0 if not failed_chunks else 5


if __name__ == "__main__":
    raise SystemExit(main())
