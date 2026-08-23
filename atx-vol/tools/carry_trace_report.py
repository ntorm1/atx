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

`--by-pairs` asks a second, separate question, and leaves the report above it
untouched. The tier-1 leave-one-out shift is a jackknife over a pair count that
lives in a narrow band -- floored at 3 by `min_confident_borrow_pairs`, capped
at 5 by `max_borrow_pairs` -- yet its budget is a CONSTANT. Dropping one of
three retained pairs removes 33% of the sample; one of five removes 20%, so a
fixed budget MIGHT be holding thin chains to a harsher standard than wide ones.
That is UNTESTED SPECULATION. The section is built to refute it as readily as
to confirm it: a thin chain is thin because its quotes are bad, so a larger raw
shift at n=3 is equally consistent with noisier quotes, and only the pass rate
compared across n AT EQUAL DISPERSION can tell those two apart.

Produce a trace with:
  set ATX_VOL_CARRY_TRACE=C:/path/carry.csv
  atx-vol-surface-db-build --db ... --hive ... --from D --to D ...

Usage:
  python atx-vol/tools/carry_trace_report.py --trace C:/path/carry.csv \
      [--min-pairs 3] [--max-dispersion 0.02] [--max-loo 0.005] \
      [--max-moneyness-shift 0.01] [--by-pairs] [--n-ref 5]
"""

from __future__ import annotations

import argparse
import pathlib
import sys

import numpy as np
import pandas as pd


def report_by_pairs(solved: pd.DataFrame, args: argparse.Namespace) -> None:
    """Test whether the CONSTANT tier-1 leave-one-out budget penalises thin chains.

    The raw shift by pair count settles NOTHING on its own: chains retain few
    pairs precisely because their quotes are bad, so a larger loo_shift at n=3
    is as consistent with noise as with a sample-size artifact. Two things
    separate them. `loo_shift / dispersion` divides the noise level out, leaving
    the n effect. And the stratified pass rate holds dispersion fixed inside a
    decile and compares n within it -- if those rates are FLAT, the hypothesis
    is dead and the raw table was only ever measuring quote quality.

    The pass column throughout is the leave-one-out condition ALONE, not all of
    tier 1: the pair-count floor and the dispersion budget are attributed by the
    report above, and folding them in here would confound the very comparison.
    """
    d = pd.DataFrame(
        {c: pd.to_numeric(solved[c], errors="coerce")
         for c in ("n_retained", "loo_shift", "dispersion")}
    )
    usable = (np.isfinite(d["n_retained"]) & np.isfinite(d["loo_shift"])
              & np.isfinite(d["dispersion"]))
    incomplete = int((~usable).sum())
    d = d[usable].copy()
    print(f"\n\nBY RETAINED PAIR COUNT -- is the constant leave-one-out budget n-biased?")
    if d.empty:
        print(f"  no solved row carries a complete (n_retained, loo_shift, dispersion) triple; "
              f"nothing to test")
        return
    d["n_retained"] = d["n_retained"].astype(int)
    d["pass_loo"] = d["loo_shift"] <= args.max_loo
    print(f"  {len(d):,d} solved expiries with a complete triple "
          f"({incomplete:,d} dropped: field missing or non-finite on an errored solve)")
    print(f"  'passes LOO' is loo_shift <= {args.max_loo} ALONE, not all of tier 1")

    print(f"\n  {'n_retained':>10}{'expiries':>11}{'med loo':>11}{'p90 loo':>11}"
          f"{'med disp':>11}{'passes LOO':>13}")
    for n_val, grp in d.groupby("n_retained"):
        print(f"  {n_val:>10,d}{len(grp):>11,d}{grp['loo_shift'].median():>11.5f}"
              f"{grp['loo_shift'].quantile(0.90):>11.5f}{grp['dispersion'].median():>11.5f}"
              f"{100.0 * grp['pass_loo'].mean():>12.1f}%")

    # loo_shift / dispersion: same jackknife, measured in units of the chain's
    # own quote noise. dispersion == 0 is legitimate (every retained pair
    # agreed) and simply has no ratio -- excluded, and SAID so.
    print(f"\n  loo_shift / dispersion -- divides the quote-noise level OUT, leaving the n effect")
    print(f"  {'n_retained':>10}{'with disp>0':>13}{'med ratio':>12}{'p90 ratio':>12}"
          f"{'disp==0 excl':>14}")
    for n_val, grp in d.groupby("n_retained"):
        ok = grp[grp["dispersion"] > 0.0]
        excl = len(grp) - len(ok)
        if ok.empty:
            print(f"  {n_val:>10,d}{0:>13,d}{'-':>12}{'-':>12}{excl:>14,d}")
            continue
        ratio = ok["loo_shift"] / ok["dispersion"]
        print(f"  {n_val:>10,d}{len(ok):>13,d}{ratio.median():>12.3f}"
              f"{ratio.quantile(0.90):>12.3f}{excl:>14,d}")
    zero_disp = int((d["dispersion"] <= 0.0).sum())
    print(f"  {zero_disp:,d} of {len(d):,d} rows carry dispersion == 0 and are excluded from the "
          f"ratio only;\n  they remain in every other table on this page")

    # The discriminating test. Equal-count deciles by dispersion RANK rather
    # than qcut, so a heavy tie mass (dispersion 0) cannot collapse the bins.
    min_cell = 25
    rank = d["dispersion"].rank(method="first", pct=True)
    d["decile"] = np.minimum((rank * 10.0).astype(int), 9)
    ns = sorted(int(v) for v in d["n_retained"].unique())
    grouped = d.groupby(["decile", "n_retained"])["pass_loo"].agg(["size", "mean"])
    cnt = grouped["size"].unstack(fill_value=0).reindex(columns=ns, fill_value=0)
    rate = grouped["mean"].unstack().reindex(columns=ns)
    print(f"\n  THE DISCRIMINATING TEST -- pass rate by n_retained WITHIN a dispersion decile")
    print(f"  equal-count deciles by dispersion rank; each cell is 'pass% (expiries)'. "
          f"A cell under\n  {min_cell} rows is marked '?' and carries no weight in the verdict")
    head = f"  {'decile':>7}{'dispersion range':>26}"
    for n_val in ns:
        head += f"{'n=' + str(n_val):>16}"
    print(head)
    for dec in sorted(cnt.index.tolist()):
        grp = d[d["decile"] == dec]
        rng = f"[{grp['dispersion'].min():.5f}, {grp['dispersion'].max():.5f}]"
        line = f"  {int(dec):>7,d}{rng:>26}"
        for n_val in ns:
            c = int(cnt.at[dec, n_val])
            if c == 0:
                line += f"{'-':>16}"
                continue
            cell = f"{100.0 * rate.at[dec, n_val]:.0f}% ({c:,d}){'' if c >= min_cell else '?'}"
            line += f"{cell:>16}"
        print(line)

    # Verdict from the numbers: inside each decile with two adequately populated
    # n values, how much more often does the WIDEST pass than the thinnest?
    flat_pp = 5.0
    spreads = []
    for dec in sorted(cnt.index.tolist()):
        big = [n_val for n_val in ns if int(cnt.at[dec, n_val]) >= min_cell]
        if len(big) < 2:
            continue
        lo, hi = big[0], big[-1]
        spreads.append((int(cnt.at[dec, lo]) + int(cnt.at[dec, hi]),
                        100.0 * (rate.at[dec, hi] - rate.at[dec, lo]), lo, hi))
    supported = False
    if not spreads:
        print(f"\n  VERDICT: UNDECIDED -- no dispersion decile holds >= {min_cell} expiries at two or "
              f"more distinct\n  n_retained values, so no cell in this table can support EITHER "
              f"reading. The hypothesis\n  is neither confirmed nor refuted by this trace.")
    else:
        agg = (sum(w * pp for w, pp, _, _ in spreads)
               / float(sum(w for w, _, _, _ in spreads)))
        up = sum(1 for _, pp, _, _ in spreads if pp > 0.0)
        legs = ", ".join(sorted({f"n={lo}->{hi}" for _, _, lo, hi in spreads}))
        where = f"{agg:+.1f} pp ({legs}), weighted over {len(spreads)} usable decile(s)"
        if abs(agg) < flat_pp:
            print(f"\n  VERDICT: REFUTED -- at equal dispersion the pass rate is FLAT across n: "
                  f"{where},\n  inside the {flat_pp:.0f} pp flatness bar. Whatever spread the raw "
                  f"table shows by pair count is\n  quote noise, NOT the gate's constant budget.")
        elif agg >= flat_pp and 3 * up >= 2 * len(spreads):
            supported = True
            print(f"\n  VERDICT: SUPPORTED -- at equal dispersion the THIN chains pass LESS often: "
                  f"{where},\n  same sign in {up}/{len(spreads)} usable deciles. The constant budget "
                  f"IS n-biased.")
        elif agg <= -flat_pp and 3 * (len(spreads) - up) >= 2 * len(spreads):
            print(f"\n  VERDICT: REFUTED, SIGN REVERSED -- at equal dispersion the thin chains pass "
                  f"MORE often:\n  {where}. Whatever binds these expiries, it is not a sample-size "
                  f"penalty.")
        else:
            print(f"\n  VERDICT: INCONCLUSIVE -- magnitude is {where}, but the sign FLIPS across "
                  f"deciles\n  ({up} favour the wide chains, {len(spreads) - up} the thin). The data "
                  f"supports neither reading.")

    # Sizing only. What a sqrt(n) budget would move, in both directions.
    pos = d[d["n_retained"] > 0]
    scaled = args.max_loo * np.sqrt(float(args.n_ref) / pos["n_retained"])
    rejected = pos["loo_shift"] > args.max_loo
    rescued = rejected & (pos["loo_shift"] <= scaled)
    newly = ~rejected & (pos["loo_shift"] > scaled)
    n_rej, n_res = int(rejected.sum()), int(rescued.sum())
    print(f"\n  COUNTERFACTUAL -- budget scaled as {args.max_loo} * sqrt({args.n_ref} / n_retained); "
          f"this sizes the\n  prize and is NOT a recommendation")
    if not supported:
        # A sqrt(n) rescaling loosens the budget at low n BY CONSTRUCTION, so it
        # rescues expiries whether or not the n-bias is real. On a refuted trace the
        # identical count is pure coverage-buying with no correctness argument under
        # it, so the number is only actionable against a SUPPORTED verdict.
        print("  the verdict above is not SUPPORTED, so these rows are rescued by LOOSENING"
              "\n  the budget at low n, not by correcting a bias -- coverage bought, nothing"
              " fixed")
    print(f"  {f'rejected today (loo_shift > {args.max_loo})':<46}{n_rej:>9,d}")
    if n_rej:
        print(f"  {'of those, passing under the scaled budget':<46}{n_res:>9,d}"
              f"  ({100.0 * n_res / n_rej:.1f}% of rejections, "
              f"{100.0 * n_res / len(pos):.1f}% of solved expiries)")
    print(f"  {f'newly rejected at n_retained > {args.n_ref} (the cost)':<46}"
          f"{int(newly.sum()):>9,d}")
    if len(pos) < len(d):
        print(f"  {len(d) - len(pos):,d} row(s) with n_retained <= 0 excluded (no scale factor exists)")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", type=pathlib.Path, required=True)
    # Defaults mirror DeAmOptions (include/atx/vol/api/fitting/deamer.hpp).
    ap.add_argument("--min-pairs", type=int, default=3)
    ap.add_argument("--max-dispersion", type=float, default=0.02)
    ap.add_argument("--max-loo", type=float, default=0.005)
    ap.add_argument("--max-moneyness-shift", type=float, default=0.01)
    ap.add_argument("--by-pairs", action="store_true")
    ap.add_argument("--n-ref", type=int, default=5)
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

    if args.by_pairs:
        report_by_pairs(solved, args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
