#!/usr/bin/env python3
"""Split coverage churn by the SHAPE of the hole, not by its size.

`served on 30 of 37 sessions` is two completely different facts wearing one
number. If those 30 sessions are one contiguous block the name listed late or
delisted early and the fit never failed; if they are scattered the fit is
flipping and a backtest drawn through that name has holes in the middle. Only
the second is ours. Ranking churn by session count alone therefore mixes the
calendar's work with the pipeline's and reports the sum as a defect rate, which
is both too large and pointed at the wrong owner.

THE SPLIT IS A HEURISTIC OVER THE SERVED PATTERN, NOT A LISTING FEED. This tool
sees one bit per name per session -- served or not -- and infers intent from
where the zeros sit. One contiguous served block anchored at the first or last
session of the window reads as a listing lifecycle, because a name that arrived
and stayed, or was there and went away, produces exactly that shape. One block
floating in the middle reads as a suspension. Anything in two or more blocks
reads as fit churn. The inference is not a fact: a name genuinely delisted and
re-listed INSIDE the window is two blocks and will be counted as scattered, and
a name whose fit happened to fail only over the opening or closing sessions is
one edge-anchored block and will be counted as a lifecycle. Both misreadings are
real; `served_vs_listed.py` is what settles them, because it holds the board
census this tool does not. Read the buckets as a triage order, not a verdict.

WINDOW EDGES ARE PART OF THE CLASSIFICATION, so the answer moves when the window
moves. Widening the window converts edge-anchored blocks into mid-window ones
and narrowing it does the reverse; a name reported as a lifecycle over July is a
suspension over July-and-August. Score the same window you intend to backtest,
and say which window you ran.

On prodv1 over 37 pooled sessions -- the reference run these numbers come from
-- 1,583 names (36.6%) were ALWAYS served, 133 (3.1%) formed one block at a
window edge, 195 (4.5%) one block mid-window, and 2,409 (55.8%) were scattered.
Better than half the churning population is therefore the fit's and not the
calendar's, which is the single claim this tool exists to support.

Usage:
  python atx-vol/tools/prodv1_coverage.py --db C:/atx-data/surface-db/prodv1 \
      --admin-exe C:/atx/build-rel/bin/atx-vol-surface-db.exe \
      --out-served C:/atx-data/logs/prodv1/served_pooled.csv
  python atx-vol/tools/churn_shape.py --served C:/atx-data/logs/prodv1/served_pooled.csv \
      [--top 8] [--month-split 2026-08-01]
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

import numpy as np
import pandas as pd

DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def runs(row):
    """(longest contiguous served run, number of separate served blocks)."""
    idx = np.flatnonzero(row)
    if idx.size == 0:
        return 0, 0
    brk = np.flatnonzero(np.diff(idx) > 1)
    starts = np.concatenate(([0], brk + 1))
    ends = np.concatenate((brk, [idx.size - 1]))
    lens = idx[ends] - idx[starts] + 1
    return int(lens.max()), int(starts.size)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Split coverage churn by the shape of the hole, not by its size.")
    ap.add_argument("--served", required=True, type=pathlib.Path,
                    help="per-symbol served matrix CSV, as written by "
                         "prodv1_coverage.py --out-served")
    ap.add_argument("--top", type=int, default=8, metavar="N",
                    help="how many most-fragmented names to list (default 8)")
    ap.add_argument("--month-split", default="2026-08-01", metavar="DATE",
                    help="YYYY-MM-DD boundary for the cross-month persistence "
                         "section; skipped when every session falls on one side")
    args = ap.parse_args()

    df = pd.read_csv(args.served)
    dates = [c for c in df.columns if DATE_RE.match(str(c))]
    if not dates:
        print(f"{args.served}: no YYYY-MM-DD session columns found (columns are: "
              f"{', '.join(map(str, df.columns[:8]))}"
              f"{', ...' if len(df.columns) > 8 else ''}). Expected the matrix written "
              f"by prodv1_coverage.py --out-served: symbol, n_served, then one 0/1 "
              f"column per session.", file=sys.stderr)
        return 1
    if len(df) == 0:
        print(f"{args.served}: {len(dates)} session column(s) but no symbol rows. "
              f"Expected one row per name ever served.", file=sys.stderr)
        return 1

    M = df[dates].to_numpy().astype(bool)
    N = len(dates)
    sym = df["symbol"].to_numpy()
    n_served = M.sum(axis=1)

    longest, blocks = zip(*(runs(M[i]) for i in range(len(M))))
    longest = np.array(longest)
    blocks = np.array(blocks)

    # A name whose served sessions form ONE block, anchored at the start or the end
    # of the window, is a listing lifecycle: it was there and went away, or it
    # arrived and stayed. A single block floating in the middle is a suspension.
    first_served = M.argmax(axis=1)
    last_served = N - 1 - M[:, ::-1].argmax(axis=1)
    one_block = blocks == 1
    edge_anchored = (first_served == 0) | (last_served == N - 1)

    always = n_served == N
    lifecycle = ~always & one_block & edge_anchored
    suspended = ~always & one_block & ~edge_anchored
    scattered = ~always & (blocks > 1)

    print(f"{len(df):,d} names ever served over {N} sessions "
          f"[{dates[0]} .. {dates[-1]}]\n")
    print(f"{'population':<44}{'names':>8}{'share':>9}{'med sessions':>14}{'med blocks':>12}")
    for label, mask in [
        ("ALWAYS served (no hole at all)", always),
        ("ONE block at a window edge (listing lifecycle)", lifecycle),
        ("ONE block mid-window (suspended, then back)", suspended),
        ("SCATTERED holes (FIT CHURN -- ours)", scattered),
    ]:
        if not mask.any():
            print(f"{label:<44}{0:>8,d}{0.0:>8.1f}%{'-':>14}{'-':>12}")
            continue
        print(f"{label:<44}{int(mask.sum()):>8,d}{100.0 * mask.mean():>8.1f}%"
              f"{np.median(n_served[mask]):>14.0f}{np.median(blocks[mask]):>12.0f}")
    print("  HEURISTIC: the split reads the served pattern, not a listing feed. A name")
    print("  delisted and re-listed inside the window reads as SCATTERED; run")
    print("  served_vs_listed.py against the board census to settle any single name.")

    print(f"\nwhat a backtest can actually tolerate")
    # Distinct percentages can round to the same session cutoff (ceil(.97*37) and
    # ceil(.95*37) are both 36), so de-duplicate on k and not on the percentage.
    seen: set[int] = set()
    for thresh in (1.00, 0.97, 0.95, 0.90):
        k = int(np.ceil(thresh * N))
        if k in seen:
            continue
        seen.add(k)
        print(f"  served on >= {k:>2d}/{N} sessions ({thresh:.0%}) : {int((n_served >= k).sum()):>6,d}")

    # Cross-month persistence: does July stability predict August stability?
    jul = [i for i, d in enumerate(dates) if d < args.month_split]
    aug = [i for i, d in enumerate(dates) if d >= args.month_split]
    if not jul or not aug:
        side = "on or after" if not jul else "before"
        print(f"\ncross-month persistence: skipped -- every session falls {side} "
              f"--month-split {args.month_split}, so the comparison has nothing to "
              f"compare. Pass a boundary inside [{dates[0]} .. {dates[-1]}].")
    else:
        jul_core = M[:, jul].all(axis=1)
        aug_core = M[:, aug].all(axis=1)
        both = jul_core & aug_core
        print(f"\ncross-month persistence ({len(jul)} sessions before {args.month_split}, "
              f"{len(aug)} on or after)")
        print(f"  stable before the split              : {int(jul_core.sum()):>6,d}")
        print(f"  stable after the split               : {int(aug_core.sum()):>6,d}")
        print(f"  stable in BOTH                       : {int(both.sum()):>6,d}")
        print(f"  of the before-stable, still stable   : {100.0 * both.sum() / max(jul_core.sum(), 1):>5.1f}%")
        print(f"  before-stable that BROKE after       : {int((jul_core & ~aug_core).sum()):>6,d}")
        print(f"  NOT before-stable that became stable : {int((~jul_core & aug_core).sum()):>6,d}")

    sc = scattered
    if sc.any():
        print(f"\nthe scattered population in detail ({int(sc.sum()):,d} names)")
        print(f"  separate served blocks : median {np.median(blocks[sc]):.0f}  "
              f"p90 {np.quantile(blocks[sc], 0.90):.0f}  max {blocks[sc].max()}")
        print(f"  sessions served        : median {np.median(n_served[sc]):.0f}  "
              f"p10 {np.quantile(n_served[sc], 0.10):.0f}  p90 {np.quantile(n_served[sc], 0.90):.0f}")
        worst = np.argsort(-blocks)[:max(args.top, 0)]
        print("  most-fragmented names  : "
              + ", ".join(f"{sym[i]}({n_served[i]}/{N},{blocks[i]}blk)" for i in worst))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
