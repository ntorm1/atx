#!/usr/bin/env python3
"""Compare two `atx-vol-surface-db band-audit` runs, splitting on WHO IS NEW.

A change that widens a fit's admission criteria buys coverage; the only question
that matters afterwards is what grade the extra coverage is. A single pooled
in-band fraction cannot answer it, because the new names dilute the old ones and
a small quality loss on the incumbents hides behind a large gain in count. So
this reports three populations separately:

  * SHARED   -- names both runs serve. Their in-band fraction must not move.
                Any drift here is a regression the coverage gain is paying for.
  * NEW      -- names only the treatment serves. This is the grade of what was
                bought, and it is expected to be worse than the incumbents:
                these are the boards the tighter budget refused.
  * DROPPED  -- names only the baseline serves. Should be empty.

`frac_in_band` is the share of the expiry's listed contracts the stored surface
reprices inside the quoted bid-ask; `avg_signed_hs` is the mean signed distance
from mid in HALF-SPREADS, so +1 is the ask and -1 the bid, and its SIGN says
whether the surface sits rich or cheap rather than merely far.

Usage:
  python atx-vol/tools/band_audit_compare.py --base band_base.tsv --treat band_both4.tsv
"""

from __future__ import annotations

import argparse
import pathlib
import sys

import pandas as pd

COLS = ["date", "symbol", "T", "n", "frac_in_band", "frac_above_ask", "avg_signed_hs"]


def load(path: pathlib.Path) -> pd.DataFrame:
    # The audit writes a TSV with a header; `keep_default_na=False` protects the
    # ticker `NA`, so numerics are coerced explicitly afterwards.
    df = pd.read_csv(path, sep="\t", keep_default_na=False, na_values=[])
    for c in df.columns:
        if c not in ("date", "symbol", "flag"):
            df[c] = pd.to_numeric(df[c], errors="coerce")
    return df


def describe(name: str, df: pd.DataFrame) -> None:
    if df.empty:
        print(f"  {name:<10} (empty)")
        return
    w = df["n"].fillna(0)
    # Contract-weighted, because an expiry with 4 listed contracts and one with
    # 400 are not equal evidence about the surface.
    wa = (df["frac_in_band"] * w).sum() / w.sum() if w.sum() else float("nan")
    flagged = int((df.get("flag", pd.Series(dtype=str)) == "BELOWFLOOR").sum())
    print(f"  {name:<10}{df['symbol'].nunique():>8,d}{len(df):>9,d}"
          f"{df['frac_in_band'].median():>11.3f}{wa:>13.3f}"
          f"{df['frac_in_band'].quantile(0.10):>10.3f}"
          f"{df['avg_signed_hs'].median():>12.3f}{flagged:>10,d}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", type=pathlib.Path, required=True)
    ap.add_argument("--treat", type=pathlib.Path, required=True)
    args = ap.parse_args()

    for p in (args.base, args.treat):
        if not p.exists():
            print(f"missing {p}", file=sys.stderr)
            return 1
    b, t = load(args.base), load(args.treat)
    bs, ts = set(b["symbol"]), set(t["symbol"])

    print(f"{'population':<12}{'names':>8}{'expiries':>9}{'med frac':>11}"
          f"{'wtd frac':>13}{'p10 frac':>10}{'med s_hs':>12}{'BELOWFLR':>10}")
    print("baseline run")
    describe("all", b)
    describe("shared", b[b["symbol"].isin(ts)])
    describe("dropped", b[~b["symbol"].isin(ts)])
    print("treatment run")
    describe("all", t)
    describe("shared", t[t["symbol"].isin(bs)])
    describe("new", t[~t["symbol"].isin(bs)])

    shared_b = b[b["symbol"].isin(ts)]
    shared_t = t[t["symbol"].isin(bs)]
    if not shared_b.empty and not shared_t.empty:
        # The regression test: join the shared population expiry-by-expiry so the
        # comparison is like-for-like rather than two aggregates of different
        # expiry mixes.
        key = ["date", "symbol", "T"]
        j = shared_b.merge(shared_t, on=key, suffixes=("_b", "_t"))
        if not j.empty:
            d = j["frac_in_band_t"] - j["frac_in_band_b"]
            print(f"\nSHARED expiries matched on (date, symbol, T): {len(j):,d}")
            print(f"  frac_in_band change   median {d.median():+.4f}"
                  f"  mean {d.mean():+.4f}  p10 {d.quantile(0.10):+.4f}"
                  f"  p90 {d.quantile(0.90):+.4f}")
            print(f"  expiries WORSE by >0.01 : {int((d < -0.01).sum()):,d}"
                  f"   BETTER by >0.01: {int((d > 0.01).sum()):,d}"
                  f"   unchanged: {int((d.abs() <= 0.01).sum()):,d}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
