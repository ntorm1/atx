#!/usr/bin/env python3
"""Split per-symbol coverage churn into LISTING churn and FIT churn.

`prodv1_coverage.py` reports that a name is served on some sessions and not
others. That single number conflates two completely different problems and only
one of them is ours:

  * NOT LISTED -- the OPRA board for that session carries no row for the symbol
    at all. Chains list and delist; a series that expires and is not re-listed
    is simply gone. Nothing is broken and nothing can be fixed.
  * LISTED, NOT SERVED -- the board carried the symbol and the pipeline produced
    no surface for it. THIS is the number that matters: it is a hole a backtest
    will hit in a name that was tradeable that day.

A third split matters as much, because the two have different fixes:

  * refused at LOAD -- put-call parity could not imply a spot, so the cell never
    reached the fitter (`n_load_errors`); the fix is a spot overlay.
  * refused at FIT -- the cell loaded and the fitter declined it.

Load failures are not individually named by the build log (it caps `failed_cell`
lines and elides the rest), so this derives the load-refusal set the same way
the loader does: a board with no strike carrying a two-sided call AND a
two-sided put on any expiry cannot imply a spot. That reproduces the loader's
own precondition rather than trusting a counter.

--from/--to BOUND THE SESSIONS SCORED, because "served on EVERY session" is a bar
whose harshness SCALES WITH THE SESSION COUNT: a figure pooled over the whole
corpus is not comparable to the ten-session numbers recorded in docs/LEDGER.md,
and two calendar months are two listing and volatility regimes, so a name listed
in one and delisted in the next is scored unstable when nothing about the fit
failed. Score each month alone and then pooled; that is what separates a fit
problem from a calendar one. With both unset every session the DB holds is used,
exactly as before.

Usage:
  python atx-vol/tools/served_vs_listed.py \
      --db C:/atx-data/surface-db/prodv1 \
      --admin-exe C:/atx/build-rel/bin/atx-vol-surface-db.exe \
      --board-root C:/atx-data/opra-all --out C:/atx-data/logs/prodv1/gap.csv \
      [--from 2026-07-01] [--to 2026-08-07]
"""

from __future__ import annotations

import argparse
import csv
import pathlib
import re
import subprocess
import sys

import numpy as np
import pandas as pd
import pyarrow.parquet as pq

PARTITION_RE = re.compile(r"^partition\s+(\d{4}-\d{2}-\d{2})\s+surfaces=(\d+)")
SURFACE_RE = re.compile(r"^surface\s+(\S+)\s+uid=")

# Same fixed-point convention as the loader: int64 1e-9 dollars, INT64_MIN UNSET.
INT64_MIN = np.iinfo(np.int64).min
OSI_EXP = slice(6, 12)
OSI_CP = 12


def run(exe: str, *args: str) -> str:
    proc = subprocess.run([exe, *args], capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"{exe} {' '.join(args)} exited {proc.returncode}\n{proc.stderr[-800:]}")
    return proc.stdout


def board_facts(path: pathlib.Path) -> pd.DataFrame:
    """Per-underlier listing facts, including whether PCP can imply a spot.

    The loader implies spot from a strike that carries a two-sided CALL and a
    two-sided PUT on the SAME expiry; `can_imply` reproduces exactly that
    precondition, so a symbol failing it here is a symbol the loader refuses.
    """
    t = pq.ParquetFile(path).read(columns=["underlying", "symbol", "bid_px", "ask_px"])
    sym = t.column("symbol").to_pandas().str
    frame = pd.DataFrame({
        "underlying": t.column("underlying").to_pandas(),
        "exp": sym[OSI_EXP],
        "cp": sym[OSI_CP],
        "strike": sym[slice(13, 21)],
        "bid": t.column("bid_px").to_pandas(),
        "ask": t.column("ask_px").to_pandas(),
    })
    two = (frame["bid"] != INT64_MIN) & (frame["ask"] != INT64_MIN) & (frame["bid"] > 0)
    frame["two"] = two

    # A (underlying, exp, strike) that carries BOTH a two-sided call and a
    # two-sided put is a parity pair; one such pair anywhere is enough.
    tw = frame[two]
    key = ["underlying", "exp", "strike"]
    pivot = tw.groupby(key)["cp"].agg(lambda s: set(s))
    has_pair = pivot.apply(lambda s: "C" in s and "P" in s)
    can = has_pair.groupby(level=0).any()

    out = pd.DataFrame({
        "n_rows": frame.groupby("underlying").size(),
        "n_two": frame.groupby("underlying")["two"].sum().astype("int64"),
    })
    out["can_imply_spot"] = out.index.map(can).fillna(False).astype(bool)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", required=True)
    ap.add_argument("--admin-exe", required=True)
    ap.add_argument("--board-root", type=pathlib.Path, required=True)
    ap.add_argument("--out", type=pathlib.Path)
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--from", dest="from_date", metavar="DATE",
                    help="inclusive lower bound YYYY-MM-DD; unset = every session")
    ap.add_argument("--to", dest="to_date", metavar="DATE",
                    help="inclusive upper bound YYYY-MM-DD; unset = every session")
    args = ap.parse_args()

    all_dates = [m.group(1) for m in
                 (PARTITION_RE.match(l) for l in run(args.admin_exe, "partitions",
                                                     "--db", args.db).splitlines()) if m]
    if not all_dates:
        print("no partitions", file=sys.stderr)
        return 1
    dates = [d for d in all_dates
             if (not args.from_date or d >= args.from_date)
             and (not args.to_date or d <= args.to_date)]
    if not dates:
        print(f"window [{args.from_date or '-inf'} .. {args.to_date or '+inf'}] selects no "
              f"sessions; {args.db} holds: {', '.join(all_dates)}", file=sys.stderr)
        return 1
    if args.from_date or args.to_date:
        print(f"window [{args.from_date or min(dates)} .. {args.to_date or max(dates)}] -- "
              f"{len(dates)} of {len(all_dates)} session(s) scored")
    else:
        print(f"window unset -- all {len(dates)} session(s) scored")

    rows = []
    listed_by_day: dict[str, set[str]] = {}
    served_by_day: dict[str, set[str]] = {}
    noimply_by_day: dict[str, set[str]] = {}
    for d in dates:
        board = args.board_root / f"date={d}" / "data.parquet"
        if not board.exists():
            print(f"{d}: no board at {board}", file=sys.stderr)
            continue
        facts = board_facts(board)
        served = {m.group(1) for m in
                  (SURFACE_RE.match(l) for l in
                   run(args.admin_exe, "partitions", "--db", args.db, "--key", d).splitlines())
                  if m}
        listed = set(facts.index)
        noimply = set(facts.index[~facts["can_imply_spot"]])
        listed_by_day[d], served_by_day[d], noimply_by_day[d] = listed, served, noimply

        gap = listed - served
        rows.append((d, len(listed), len(served), len(gap),
                     len(gap & noimply), len(gap - noimply), len(served - listed)))

    print(f"{'date':<12}{'listed':>8}{'served':>8}{'gap':>8}"
          f"{'no-PCP':>9}{'fit-ref':>9}{'served!listed':>14}")
    for d, nl, ns, ng, nn, nf, extra in rows:
        print(f"{d:<12}{nl:>8,d}{ns:>8,d}{ng:>8,d}{nn:>9,d}{nf:>9,d}{extra:>14,d}")

    # The stability question, now on the LISTED population only: of the sessions
    # a name was actually on the board, how many did it get served on?
    universe = set().union(*listed_by_day.values())
    per = {}
    for s in universe:
        days_listed = [d for d in dates if s in listed_by_day.get(d, ())]
        if not days_listed:
            continue
        n_served = sum(1 for d in days_listed if s in served_by_day[d])
        per[s] = (len(days_listed), n_served)

    full = [s for s, (nl, ns) in per.items() if nl == ns and nl == len(dates)]
    never = [s for s, (_, ns) in per.items() if ns == 0]
    partial = [s for s, (nl, ns) in per.items() if 0 < ns < nl]
    print(f"\nover the LISTED sessions of each name ({len(per):,d} names, {len(dates)} sessions)")
    print(f"  served on every session it was listed, and listed all {len(dates)}: {len(full):,d}")
    print(f"  never served at all                                    : {len(never):,d}")
    print(f"  served on SOME listed sessions only (the real churn)    : {len(partial):,d}")

    # Split the real churn by whether the missing sessions were load refusals.
    load_driven = 0
    for s in partial:
        missing = [d for d in dates
                   if s in listed_by_day.get(d, ()) and s not in served_by_day[d]]
        if missing and all(s in noimply_by_day[d] for d in missing):
            load_driven += 1
    print(f"     of which EVERY missing session was a no-PCP load refusal: {load_driven:,d}"
          f"  ({100.0 * load_driven / len(partial):.1f}%)" if partial else "")

    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with args.out.open("w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(["symbol", "n_listed", "n_served", *(f"s_{d}" for d in dates)])
            for s in sorted(per):
                nl, ns = per[s]
                w.writerow([s, nl, ns, *("-" if s not in listed_by_day.get(d, ()) else
                                         ("1" if s in served_by_day[d] else "0")
                                         for d in dates)])
        print(f"\nper-symbol matrix -> {args.out}  ({len(per):,d} names; "
              f"1 served, 0 listed-not-served, - not listed)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
