#!/usr/bin/env python3
"""Per-underlier chain census over one OPRA board — the input to fit tiering.

A whole-OPRA snapshot is not 6,189 equivalent fitting problems. Measured on the
2026-08-21 15:55 ET board (1,933,682 rows, 6,189 underliers, 1,547,547
two-sided contracts)::

    tier                     underliers    contracts  % of board  med relsp
    SPY only                          1       13,102        0.8%      0.022
    n_two >= 1000                   280      613,938       39.7%      0.095
    n_two >=  500                   685      899,498       58.1%      0.130
    n_two >=  200                 1,693    1,209,532       78.2%      0.179
    n_two >=  100                 3,024    1,399,793       90.5%      0.236
    n_two >=   50                 4,477    1,503,301       97.1%      0.298
    n_two >=   10                 5,897    1,546,506       99.9%      0.348
    any two-sided quote           6,139    1,547,547      100.0%      0.356

Two facts drive the whole pipeline design:

  * The top 280 underliers carry 40% of the contracts and the bottom ~3,000
    carry under 10%. Fit cost is contract-dominated, so tiering by chain
    density buys most of the coverage for a small fraction of the work, and
    makes "start small, increase incrementally" a measured ladder rather than
    a guess.
  * Quote quality collapses down the tail. Median relative spread is 2.2% for
    SPY, 9.5% across the top-280 cohort, and 34.8% across everything with ten
    or more two-sided contracts -- with a 99th percentile of 138% (bid at or
    near zero). A mid-based implied vol is close to meaningless out there, so
    the sparse-chain fitting work needs spread-aware weighting, not just a
    lower minimum-quote guard.

Expiry coverage is the other axis: 5,669 underliers quote four or more
expiries two-sided, but only 2,194 quote six or more and only 655 quote twelve
or more. A term-structure model with a per-expiry slice cannot be fit at all
below its own expiry-count floor, so that floor is a tier boundary too.

The census is deliberately cheap and provider-free: it reads a board already
in the hive and derives everything from the OSI symbol, so it can be re-run
per session to keep tier membership honest as chains list and delist.

Usage:
  python atx-vol/tools/opra_chain_census.py \
      --board C:/atx-data/opra-all/date=2026-08-21/data.parquet \
      --out atx-vol/data/universe/census_2026-08-21.csv

  # emit a tier roster the fitter can consume directly
  python atx-vol/tools/opra_chain_census.py --board <board.parquet> \
      --out <census.csv> --tier-roster <roster.txt> --min-two-sided 500
"""

from __future__ import annotations

import argparse
import pathlib
import sys

import numpy as np
import pandas as pd
import pyarrow.parquet as pq

INT64_MIN = np.iinfo(np.int64).min

# OSI symbol layout, fixed width: root padded to 6, then YYMMDD, then C/P, then
# an 8-digit strike in thousandths of a dollar. Slicing beats a regex here by an
# order of magnitude on a 1.9M-row board and cannot silently half-match.
OSI_EXP = slice(6, 12)
OSI_CP = 12
OSI_STRIKE = slice(13, 21)

CENSUS_COLUMNS = [
    "underlying", "n_rows", "n_two_sided", "n_expiries", "n_expiries_two_sided",
    "n_strikes", "n_calls", "n_puts", "med_rel_spread", "p90_rel_spread",
    "min_dte_days", "max_dte_days",
]


def load_census(path: pathlib.Path) -> pd.DataFrame:
    """Read a census CSV back without losing tickers to pandas' NA sniffing.

    THE TICKER `NA` IS REAL AND IS ON THE BOARD (29 rows on 2026-08-21,
    `NA    260821C00002500` and friends). `pd.read_csv` defaults to treating the
    literal string "NA" as a missing value, so a census round-tripped through
    the default reader comes back with a NaN in the `underlying` column. That is
    not a cosmetic problem: joining on it silently drops the ticker, and feeding
    the column into `"\\n".join(...)` raises `TypeError: sequence item 5655:
    expected str instance, float found` -- which is how this was found. The same
    trap eats `NAN`, `NULL`, `NONE`, `N/A` and `INF` if they ever list.

    Always read a census through here, never through a bare `pd.read_csv`."""
    return pd.read_csv(path, keep_default_na=False, na_values=[])


def load_board(path: pathlib.Path) -> pd.DataFrame:
    """Read a hive date file. ParquetFile.read(), never pq.read_table: the file
    lives under a ``date=YYYY-MM-DD/`` directory that pyarrow's dataset layer
    would treat as a Hive partition key and inject as a spurious ``date``
    column (same trap documented in pull_opra_hive.merge_date_file)."""
    return pq.ParquetFile(path).read().to_pandas()


def census(board: pd.DataFrame, asof: pd.Timestamp) -> pd.DataFrame:
    sym = board["symbol"].str
    exp = pd.to_datetime(sym[OSI_EXP], format="%y%m%d", errors="coerce")
    frame = pd.DataFrame({
        "underlying": board["underlying"],
        "exp": exp,
        "cp": sym[OSI_CP],
        "strike": pd.to_numeric(sym[OSI_STRIKE], errors="coerce") / 1000.0,
        "bid_px": board["bid_px"],
        "ask_px": board["ask_px"],
    })
    # "Two-sided" means BOTH sides quoted AND the bid is a real price. A zero
    # bid with a live offer is the dominant shape in the tail and carries no
    # usable mid, so counting it as coverage would overstate every tier.
    two = ((frame["bid_px"] != INT64_MIN) & (frame["ask_px"] != INT64_MIN)
           & (frame["bid_px"] > 0))
    frame["two_sided"] = two
    mid = np.where(two, (frame["bid_px"] + frame["ask_px"]) / 2e9, np.nan)
    frame["rel_spread"] = np.where(
        two & (mid > 0), (frame["ask_px"] - frame["bid_px"]) / 1e9 / np.where(mid > 0, mid, 1.0), np.nan)
    frame["dte"] = (frame["exp"] - asof).dt.days

    grp = frame.groupby("underlying", sort=True)
    tw = frame[two].groupby("underlying", sort=True)
    out = pd.DataFrame({
        "n_rows": grp.size(),
        "n_two_sided": grp["two_sided"].sum(),
        "n_expiries": grp["exp"].nunique(),
        "n_expiries_two_sided": tw["exp"].nunique(),
        "n_strikes": grp["strike"].nunique(),
        "n_calls": grp["cp"].apply(lambda s: int((s == "C").sum())),
        "n_puts": grp["cp"].apply(lambda s: int((s == "P").sum())),
        "med_rel_spread": tw["rel_spread"].median(),
        "p90_rel_spread": tw["rel_spread"].quantile(0.90),
        "min_dte_days": tw["dte"].min(),
        "max_dte_days": tw["dte"].max(),
    })
    # An underlier with no two-sided row at all has NaN in every tw-derived
    # column; the counts are genuinely zero, the spreads genuinely undefined.
    for c in ("n_expiries_two_sided", "min_dte_days", "max_dte_days"):
        out[c] = out[c].fillna(0).astype("int64")
    out["n_two_sided"] = out["n_two_sided"].astype("int64")
    return out.reset_index()[CENSUS_COLUMNS].sort_values(
        "n_two_sided", ascending=False).reset_index(drop=True)


def print_tiers(cen: pd.DataFrame) -> None:
    total = int(cen["n_two_sided"].sum())
    print(f"\nboard: underliers={len(cen):,d} two_sided_contracts={total:,d}")
    print(f"{'tier':<24}{'underliers':>11}{'contracts':>13}{'% board':>9}{'med relsp':>11}")
    tiers = [("n_two >= 1000", 1000), ("n_two >=  500", 500), ("n_two >=  200", 200),
             ("n_two >=  100", 100), ("n_two >=   50", 50), ("n_two >=   10", 10),
             ("any two-sided", 1)]
    for label, k in tiers:
        s = cen[cen["n_two_sided"] >= k]
        if s.empty:
            continue
        print(f"{label:<24}{len(s):>11,d}{int(s['n_two_sided'].sum()):>13,d}"
              f"{100 * s['n_two_sided'].sum() / total:>8.1f}%{s['med_rel_spread'].median():>11.3f}")
    print(f"\n{'expiries two-sided':<24}{'underliers':>11}")
    for k in (1, 2, 3, 4, 6, 8, 12):
        print(f"  >= {k:<20d}{int((cen['n_expiries_two_sided'] >= k).sum()):>11,d}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", type=pathlib.Path, required=True,
                    help="hive date file, e.g. <root>/date=YYYY-MM-DD/data.parquet")
    ap.add_argument("--out", type=pathlib.Path, required=True, help="census CSV")
    ap.add_argument("--tier-roster", type=pathlib.Path,
                    help="also write a one-symbol-per-line roster for the tier")
    ap.add_argument("--min-two-sided", type=int, default=0)
    ap.add_argument("--min-expiries", type=int, default=0)
    args = ap.parse_args()

    board = load_board(args.board)
    # The board's own stamp is the as-of; taking today's date would silently
    # produce negative DTEs on any historical session.
    asof = pd.Timestamp(board["ts"].iloc[0]).normalize()
    cen = census(board, asof)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    cen.to_csv(args.out, index=False)
    print(f"census -> {args.out}  ({len(cen):,d} underliers, as-of {asof.date()})")
    print_tiers(cen)

    if args.tier_roster:
        sel = cen[(cen["n_two_sided"] >= args.min_two_sided)
                  & (cen["n_expiries_two_sided"] >= args.min_expiries)]
        args.tier_roster.parent.mkdir(parents=True, exist_ok=True)
        args.tier_roster.write_text("\n".join(sel["underlying"]) + "\n", encoding="utf-8")
        print(f"\nroster -> {args.tier_roster}  ({len(sel):,d} underliers, "
              f"{int(sel['n_two_sided'].sum()):,d} two-sided contracts, "
              f"min_two_sided={args.min_two_sided} min_expiries={args.min_expiries})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
