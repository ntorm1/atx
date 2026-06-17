#!/usr/bin/env python3
"""Derive the p3 S1-6 liquid expansion universe.

Selection rule (frozen in atx-engine/plans/p3-impl/data-ingestion-reference.md
section 6.1): the databento equs_ohlcv_1d universe filtered to the top-N by
**trailing-21d average dollar-volume** (close * volume) over the most recent 21
trading-date partitions, intersected with the Nasdaq Trader **common-equity**
universe (drops ETFs / test issues / non-common issues), ties broken by ascending
canonical symbol. N kept tractable for a polite crawl.

Writes one symbol per line to --out. Reuses looks_common_equity / the Nasdaq
Trader feeds from build_us_split_adjustments so the "common equity" notion matches
the builder exactly.
"""
from __future__ import annotations

import argparse
import csv
import glob
import io
import os
import sys

import pyarrow.parquet as pq

# Reuse the builder's exact common-equity heuristic + public feeds.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_us_split_adjustments as b  # noqa: E402


def trailing_dollar_volume(hive_root: str, window: int) -> dict[str, float]:
    """Average close*volume per symbol over the most recent `window` partitions."""
    parts = sorted(glob.glob(os.path.join(hive_root, "date=*", "part-*.parquet")))
    if not parts:
        raise SystemExit(f"no databento partitions under {hive_root}")
    recent = parts[-window:]
    totals: dict[str, float] = {}
    counts: dict[str, int] = {}
    for path in recent:
        tbl = pq.ParquetFile(path).read(columns=["symbol", "close", "volume"])
        syms = tbl.column("symbol").to_pylist()
        closes = tbl.column("close").to_pylist()
        vols = tbl.column("volume").to_pylist()
        for sym, c, v in zip(syms, closes, vols):
            if c is None or v is None:
                continue
            dv = float(c) * float(v)
            if dv <= 0.0:
                continue
            totals[sym] = totals.get(sym, 0.0) + dv
            counts[sym] = counts.get(sym, 0) + 1
    return {s: totals[s] / counts[s] for s in totals}


def common_equity_symbols(timeout: float) -> set[str]:
    """Nasdaq Trader common-equity symbols (no ETFs / test issues)."""
    out: set[str] = set()
    text = b.strip_footer(b.read_public_text(b.NASDAQ_LISTED_URL, timeout))
    for row in csv.DictReader(io.StringIO(text), delimiter="|"):
        sym = (row.get("Symbol") or "").strip()
        name = (row.get("Security Name") or "").strip()
        if not sym:
            continue
        if (row.get("ETF") or "").strip().upper() == "Y":
            continue
        if (row.get("NextShares") or "").strip().upper() == "Y":
            continue
        if (row.get("Test Issue") or "").strip().upper() == "Y":
            continue
        if not b.looks_common_equity(name):
            continue
        out.add(sym.upper())
    text = b.strip_footer(b.read_public_text(b.OTHER_LISTED_URL, timeout))
    for row in csv.DictReader(io.StringIO(text), delimiter="|"):
        sym = (row.get("NASDAQ Symbol") or row.get("ACT Symbol") or "").strip()
        name = (row.get("Security Name") or "").strip()
        if not sym:
            continue
        if (row.get("ETF") or "").strip().upper() == "Y":
            continue
        if (row.get("Test Issue") or "").strip().upper() == "Y":
            continue
        if not b.looks_common_equity(name):
            continue
        out.add(sym.upper())
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--hive-root", default="data/databento/equs_ohlcv_1d_by_date")
    ap.add_argument("--window", type=int, default=21, help="trailing trading-date partitions")
    ap.add_argument("--top-n", type=int, default=500)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--out", default="data/databento_liquid_top500.txt")
    args = ap.parse_args()

    adv = trailing_dollar_volume(args.hive_root, args.window)
    print(f"databento symbols with positive trailing-{args.window}d $vol: {len(adv)}")

    common = common_equity_symbols(args.timeout)
    print(f"nasdaq common-equity symbols: {len(common)}")

    # databento symbols use '.'/'/' class separators that the listing feeds render
    # with '-'; normalize both sides to the listing form before intersecting.
    def norm(s: str) -> str:
        return s.strip().upper().replace(".", "-").replace("/", "-")

    common_norm = {norm(s) for s in common}
    ranked = [s for s in adv if norm(s) in common_norm]
    # ties: descending $vol, then ascending canonical symbol.
    ranked.sort(key=lambda s: (-adv[s], s))
    top = ranked[: args.top_n]
    print(f"liquid common-equity intersection: {len(ranked)}; keeping top {len(top)}")
    if top:
        print(f"top 10: {top[:10]}")
        print(f"cutoff $vol (rank {len(top)}): {adv[top[-1]]:.0f}")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        for s in top:
            fh.write(s + "\n")
    print(f"wrote {len(top)} symbols -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
