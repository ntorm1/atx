#!/usr/bin/env python3
"""Read an `ATX_VOL_CARRY_TRACE` dump and say WHICH carry gate rejected each expiry.

Carry is the whole thin tail: over the 10-session 2026-08 full-OPRA corpus,
22,181 of 22,411 failed cells (99.0%) have `carry_failed` accounting for every
chain of the symbol. An expiry reaches a surface only if its carry is usable,
and there are two ways for it to be:

  * TIER 1, `confident` -- n_retained >= min_confident_borrow_pairs (3) AND
    dispersion <= max_carry_dispersion (0.02) AND
    max_leave_one_out_shift <= max_carry_leave_one_out (0.005), all in
    annualized borrow-rate units.
  * TIER 2, `carry_moneyness_bounded` -- consulted only when the board has no
    confident expiry at all. Same pair-count floor, plus atm_sigma > 0, plus
    max_leave_one_out_moneyness <= max_carry_moneyness_shift (0.01) and
    dispersion_moneyness <= 4x that. Re-expressing the shift in moneyness units
    is what makes it survivable on a wide-spread name, where a borrow implied
    from any single pair is enormously noisy in RATE units.

A board with neither tier on any expiry fabricates nothing and stays dropped.
So the actionable question is not "did carry fail" but "which of the four
budgets bound, and on how many names" -- because a pair-count floor that cannot
be met is a data problem no tuning fixes, while a dispersion budget that a
30%-spread name can never clear is a calibration choice.

Produce a trace with:
  set ATX_VOL_CARRY_TRACE=C:/path/carry.csv
  atx-vol-surface-db-build --db ... --hive ... --from D --to D ...

Usage:
  python atx-vol/tools/carry_trace_report.py --trace C:/path/carry.csv \
      [--min-pairs 3] [--max-dispersion 0.02] [--max-loo 0.005] \
      [--max-moneyness-shift 0.005]
"""

from __future__ import annotations

import argparse
import pathlib
import sys

import numpy as np
import pandas as pd


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", type=pathlib.Path, required=True)
    # Defaults mirror DeAmOptions (include/atx/vol/api/fitting/deamer.hpp).
    ap.add_argument("--min-pairs", type=int, default=3)
    ap.add_argument("--max-dispersion", type=float, default=0.02)
    ap.add_argument("--max-loo", type=float, default=0.005)
    ap.add_argument("--max-moneyness-shift", type=float, default=0.01)
    args = ap.parse_args()

    if not args.trace.exists():
        print(f"no trace at {args.trace}", file=sys.stderr)
        return 1
    # keep_default_na=False would protect a ticker `NA`, but the trace's numeric
    # columns are legitimately blank on an errored solve, so read normally and
    # repair the ticker column afterwards.
    t = pd.read_csv(args.trace)
    t["ticker"] = t["ticker"].fillna("NA").astype(str)
    n = len(t)
    print(f"{n:,d} expiry carry resolutions over {t['ticker'].nunique():,d} underliers\n")

    solved = t[t["err"] == 0]
    errored = t[t["err"] != 0]
    print(f"solve ERRORED (no quotable co-terminal pair, or every pair's solve failed): "
          f"{len(errored):,d}  ({100.0 * len(errored) / n:.1f}%)")
    print(f"solve returned a carry                                                   : "
          f"{len(solved):,d}  ({100.0 * len(solved) / n:.1f}%)\n")

    if solved.empty:
        return 0

    # Of the solved carries, why did each miss tier 1?
    below_pairs = solved["n_retained"] < args.min_pairs
    disp_over = solved["dispersion"] > args.max_dispersion
    loo_over = solved["loo_shift"] > args.max_loo
    print("TIER 1 (confident) -- which condition failed, over the solved carries")
    print(f"  {'condition':<44}{'count':>9}{'share':>9}")
    for label, mask in [
        (f"n_retained < {args.min_pairs} (pairs absent, NOT tunable)", below_pairs),
        (f"dispersion > {args.max_dispersion} (rate units)", disp_over),
        (f"leave-one-out > {args.max_loo} (rate units)", loo_over),
    ]:
        print(f"  {label:<44}{int(mask.sum()):>9,d}{100.0 * mask.mean():>8.1f}%")
    print(f"  {'PASSED tier 1':<44}{int(solved['confident'].sum()):>9,d}"
          f"{100.0 * solved['confident'].mean():>8.1f}%")

    # The set that matters: enough pairs, but not confident. Tier 2 is their
    # only route, so what stops it?
    reach = solved[~below_pairs & (solved["confident"] == 0)]
    print(f"\nTHE ACTIONABLE SET -- {len(reach):,d} expiries with >= {args.min_pairs} pairs "
          f"that missed tier 1\n  (the pairs existed; a budget rejected them)")
    if not reach.empty:
        no_sigma = ~(reach["atm_sigma"] > 0)
        loo_m = reach["loo_mny"] > args.max_moneyness_shift
        disp_m = reach["disp_mny"] > 4.0 * args.max_moneyness_shift
        print(f"  {'tier 2 condition':<44}{'count':>9}{'share':>9}")
        for label, mask in [
            ("atm_sigma == 0 (European route: no slice width)", no_sigma),
            (f"leave-one-out moneyness > {args.max_moneyness_shift}", loo_m),
            (f"dispersion moneyness > {4.0 * args.max_moneyness_shift}", disp_m),
        ]:
            print(f"  {label:<44}{int(mask.sum()):>9,d}{100.0 * mask.mean():>8.1f}%")
        print(f"  {'RESCUED by tier 2 (bounded)':<44}{int(reach['bounded'].sum()):>9,d}"
              f"{100.0 * reach['bounded'].mean():>8.1f}%")

        # How far past each budget? A cohort sitting just outside is a different
        # decision from one an order of magnitude out.
        print(f"\n  how far past the budget (ratio actual/budget, over those that exceed it)")
        print(f"  {'budget':<30}{'median':>10}{'p90':>10}{'p99':>10}")
        for label, series, budget in [
            ("dispersion (rate)", reach["dispersion"], args.max_dispersion),
            ("leave-one-out (rate)", reach["loo_shift"], args.max_loo),
            ("leave-one-out (moneyness)", reach["loo_mny"], args.max_moneyness_shift),
            ("dispersion (moneyness)", reach["disp_mny"], 4.0 * args.max_moneyness_shift),
        ]:
            over = pd.to_numeric(series, errors="coerce")
            over = over[np.isfinite(over) & (over > budget)] / budget
            if over.empty:
                continue
            print(f"  {label:<30}{over.median():>10.1f}{over.quantile(0.90):>10.1f}"
                  f"{over.quantile(0.99):>10.1f}")

    # Per-underlier: does the BOARD have any usable anchor at all? That, not the
    # per-expiry rate, is what decides whether a symbol produces a surface.
    per = t.groupby("ticker").agg(
        n_exp=("T", "size"),
        any_confident=("confident", lambda s: bool(pd.to_numeric(s, errors="coerce").fillna(0).max())),
        any_bounded=("bounded", lambda s: bool(pd.to_numeric(s, errors="coerce").fillna(0).max())),
    )
    none = int((~per["any_confident"] & ~per["any_bounded"]).sum())
    print(f"\nper underlier ({len(per):,d} boards)")
    print(f"  has a TIER 1 anchor                 : {int(per['any_confident'].sum()):>7,d}"
          f"  ({100.0 * per['any_confident'].mean():.1f}%)")
    print(f"  no tier 1, but has a TIER 2 anchor  : "
          f"{int((~per['any_confident'] & per['any_bounded']).sum()):>7,d}")
    print(f"  NO anchor of either tier (dropped)  : {none:>7,d}"
          f"  ({100.0 * none / len(per):.1f}%)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
