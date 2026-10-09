#!/usr/bin/env python3
"""How much coverage is left on the table, and how much is unreachable from the board.

Carry anchoring accounts for the entire cell-failure set: a board with no
confident and no moneyness-bounded expiry fabricates nothing and stays dropped.
Both anchor tiers require `n_retained >= min_confident_borrow_pairs`, and
`n_retained` can never exceed the number of strikes on the expiry carrying a
two-sided CALL and a two-sided PUT. That upper bound is computable from the raw
board, so the remaining failures split cleanly into two populations that need
completely different answers:

  * UNREACHABLE -- no expiry on the board has `min_pairs` parity pairs. No budget
    change can help; the pairs do not exist. The only routes are a carry that
    does not come from this board's own parity (a borrow curve, a term-structure
    prior, a sector/ETF proxy) or accepting that the name is not fittable.
  * REACHABLE -- some expiry clears the pair floor, so a filter rejected it. This
    is the population a budget or a robustness change can still win.

Two-sidedness here mirrors `leg_quote_valid` (atx-vol/src/fitting/deamer.cpp:426)
as far as the board's four price columns allow: a strictly positive bid, a
strictly positive ask, a non-crossed `ask >= bid`, and a finite positive mid
derived the way the loader derives it. What it cannot mirror is the flag kill
mask, because the board carries no flags column and the bits are raised
downstream during chain construction. `WideSpread` keys off `wide_spread_pct`,
which is a per-symbol calibration and not a constant (0.50 to 3.00 across the
profiles in atx-vol/src/fitting/profile.cpp); `Stale` needs a per-quote timestamp
against a session cutoff; `LowVega` needs a vega column; `Halted` needs session
state; `Penny` needs the profile's `penny_floor`; and `Locked` (bid == ask) is
left unmodelled here as well. Every one of those bits can only REMOVE a leg and
never add one, so the pair count below is an UPPER BOUND on the pairs the fitter
actually sees. The residual bias is therefore signed and one-way: REACHABLE is an
upper bound and UNREACHABLE is a lower bound, and a name reported as reachable
may still turn out arithmetically unreachable once the flags land.

Reporting them separately is the point: quoting one number for "remaining
failures" invites tuning against a population that is 44% arithmetic-impossible.

Usage:
  python atx-vol/tools/carry_headroom.py --board C:/atx-data/opra-all/date=2026-08-21/data.parquet \
      --db C:/atx-data/surface-db/prodv1-carry \
      --admin-exe C:/atx/build-rel/bin/atx-vol-surface-db.exe --date 2026-08-21
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

import numpy as np
import pandas as pd
import pyarrow.parquet as pq

INT64_MIN = np.iinfo(np.int64).min
OSI_EXP = slice(6, 12)
OSI_CP = 12
OSI_STRIKE = slice(13, 21)
SURFACE_RE = re.compile(r"^surface\s+(\S+)\s+uid=")


def served(admin_exe: str, db: str, date: str) -> set[str]:
    proc = subprocess.run([admin_exe, "partitions", "--db", db, "--key", date],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"partitions --key {date} exited {proc.returncode}\n{proc.stderr[-500:]}")
    return {m.group(1) for m in (SURFACE_RE.match(l) for l in proc.stdout.splitlines()) if m}


def parity_profile(board: pathlib.Path) -> pd.DataFrame:
    t = pq.ParquetFile(board).read(columns=["underlying", "symbol", "bid_px", "ask_px"])
    sym = t.column("symbol").to_pandas().str
    f = pd.DataFrame({
        "u": t.column("underlying").to_pandas(),
        "exp": sym[OSI_EXP],
        "cp": sym[OSI_CP],
        "k": sym[OSI_STRIKE],
        "bid": t.column("bid_px").to_pandas(),
        "ask": t.column("ask_px").to_pandas(),
    })
    # Mirrors `leg_quote_valid` (atx-vol/src/fitting/deamer.cpp:426) minus the
    # flag kill mask, which is not recoverable from the board's columns. The
    # sentinel is filtered BEFORE any arithmetic -- INT64_MIN in a sum is
    # nonsense -- and mid is derived as the loader derives it, 0.5*(bid + ask)
    # (atx-vol/src/fitting/arb.cpp:1764).
    #
    # STRICT `ask > bid`, NOT `ask >= bid`, and the difference is not a nit.
    # `leg_quote_valid` itself accepts `ask == bid`, but it checks the kill mask
    # FIRST, and `arb.cpp:1769` raises `QuoteFlag::Locked` on exactly `bid >= ask`
    # with `Locked` in that mask. So a locked quote is rejected before the price
    # check ever runs, and the effective predicate is the strict one. This is the
    # one kill-mask bit that IS recoverable from bid and ask alone; the rest
    # (WideSpread, Penny, Stale, Halted, LowVega) are not, because they need a
    # per-symbol profile constant, a quote timestamp or a vega.
    present = (f["bid"] != INT64_MIN) & (f["ask"] != INT64_MIN)
    bid = f["bid"].where(present, 0).astype("float64")
    ask = f["ask"].where(present, 0).astype("float64")
    mid = 0.5 * (bid + ask)
    two = present & (bid > 0) & (ask > 0) & (ask > bid) & np.isfinite(mid) & (mid > 0)
    tw = f[two]
    pair = tw.groupby(["u", "exp", "k"])["cp"].agg(
        lambda s: ("C" in set(s)) and ("P" in set(s)))
    pairs = pair[pair].reset_index()
    per_exp = pairs.groupby(["u", "exp"]).size().rename("n_pairs").reset_index()

    out = pd.DataFrame(index=pd.Index(sorted(f["u"].unique()), name="u"))
    out["n_two"] = two.groupby(f["u"]).sum().astype("int64")
    out["best_exp_pairs"] = per_exp.groupby("u")["n_pairs"].max()
    out["total_pairs"] = pairs.groupby("u").size()
    return out.fillna(0).astype({"best_exp_pairs": int, "total_pairs": int})


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", type=pathlib.Path, required=True)
    ap.add_argument("--db", required=True)
    ap.add_argument("--admin-exe", required=True)
    ap.add_argument("--date", required=True)
    ap.add_argument("--min-pairs", type=int, default=3,
                    help="DeAmOptions::min_confident_borrow_pairs (default 3)")
    args = ap.parse_args()

    prof = parity_profile(args.board)
    have = served(args.admin_exe, args.db, args.date)
    prof["served"] = prof.index.isin(have)
    missing = prof[~prof["served"]]

    # A board with ZERO parity pairs cannot imply a spot at all, so it never
    # reaches the fitter and is not a fit failure -- keep it in its own bucket
    # rather than letting it inflate the "unreachable" count.
    no_spot = missing[missing["total_pairs"] == 0]
    rest = missing[missing["total_pairs"] > 0]
    unreachable = rest[rest["best_exp_pairs"] < args.min_pairs]
    reachable = rest[rest["best_exp_pairs"] >= args.min_pairs]

    print(f"board {args.date}: {len(prof):,d} underliers, {int(prof['served'].sum()):,d} served, "
          f"{len(missing):,d} not served\n")
    print(f"{'population':<46}{'names':>8}{'share':>9}{'n_two med':>11}")
    for label, s in [
        ("no parity pair at all (never LOADS, no spot)", no_spot),
        (f"best expiry < {args.min_pairs} pairs (UNREACHABLE, arithmetic)", unreachable),
        (f"best expiry >= {args.min_pairs} pairs (REACHABLE, a filter said no)", reachable),
    ]:
        share = 100.0 * len(s) / max(len(missing), 1)
        med = s["n_two"].median() if len(s) else float("nan")
        print(f"{label:<46}{len(s):>8,d}{share:>8.1f}%{med:>11.0f}")
    print("pairs are counted without the flag kill-mask (no flags column on the board), so"
          " REACHABLE is an UPPER bound and UNREACHABLE a LOWER bound")

    print(f"\nthe REACHABLE population is the headroom a budget or robustness change can win:"
          f" {len(reachable):,d} names")
    if len(reachable):
        print(f"  best-expiry pairs   median {reachable['best_exp_pairs'].median():.0f}"
              f"  p90 {reachable['best_exp_pairs'].quantile(0.90):.0f}")
        print(f"  two-sided contracts median {reachable['n_two'].median():.0f}"
              f"  p90 {reachable['n_two'].quantile(0.90):.0f}")

    # Where the SERVED population sits, for contrast: if the reachable-but-failing
    # names look like the served ones on every board statistic, the difference is
    # in the fit and not in the data.
    ok = prof[prof["served"]]
    print(f"\nfor contrast, SERVED names ({len(ok):,d}):")
    print(f"  best-expiry pairs   median {ok['best_exp_pairs'].median():.0f}"
          f"  p10 {ok['best_exp_pairs'].quantile(0.10):.0f}")
    print(f"  two-sided contracts median {ok['n_two'].median():.0f}"
          f"  p10 {ok['n_two'].quantile(0.10):.0f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
