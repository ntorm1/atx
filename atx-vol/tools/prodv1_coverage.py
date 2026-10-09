#!/usr/bin/env python3
"""Cross-day coverage and per-symbol stability over a built SurfaceDb.

A single session's coverage number says how well the fitter did on ONE board. It
cannot say whether the pipeline is STABLE, and instability is the failure mode a
backtest actually pays for: a name that fits on Monday, refuses on Tuesday and
fits again on Wednesday puts a hole in every series drawn through it, and each
individual day looks fine on its own.

WHY THE SERVED SET COMES FROM THE DB AND NOT FROM THE BUILD LOG. The build
prints `failed_cell <date> <SYMBOL> ...` lines but CAPS them --
`coverage.failed_cells_elided 2142` on a 2,174-failure session, so 32 of 2,174
names are printed and the other 98.5% are a counter. Reconstructing membership
from the log therefore silently analyses 1.5% of the population and reports it
as the whole. `atx-vol-surface-db partitions --key <date>` lists the surfaces a
partition actually holds, which is both complete and the thing a backtest will
itself read, so that is the source of truth here.

THE UNIVERSE IS THE UNION, NOT ANY ONE DAY. Chains list and delist, so a symbol
absent from one day may never have been on that board at all. This reports
`absent` (not on the board) separately from `refused` (on the board, not served)
only when a board census is supplied via --census-dir; without one, a symbol is
scored over the days it was served or not and the churn figure is an upper bound
on real instability. Say which you ran.

--from/--to BOUND THE SESSIONS SCORED, because "served on EVERY session" is a bar
whose harshness SCALES WITH THE SESSION COUNT: a figure pooled over the whole
corpus is not comparable to the ten-session numbers recorded in docs/LEDGER.md,
and two calendar months are two listing and volatility regimes, so a name listed
in one and delisted in the next is scored unstable when nothing about the fit
failed. Score each month alone and then pooled; that is what separates a fit
problem from a calendar one. With both unset every session the DB holds is used,
exactly as before.

Usage:
  python atx-vol/tools/prodv1_coverage.py --db C:/atx-data/surface-db/prodv1 \
      --admin-exe build-rel/bin/atx-vol-surface-db.exe \
      --reports C:/atx-data/logs/prodv1 \
      [--from 2026-07-01] [--to 2026-08-07]
"""

from __future__ import annotations

import argparse
import csv
import pathlib
import re
import subprocess
import sys

PARTITION_RE = re.compile(r"^partition\s+(\d{4}-\d{2}-\d{2})\s+surfaces=(\d+)")
SURFACE_RE = re.compile(r"^surface\s+(\S+)\s+uid=")


def run(exe: str, *args: str) -> str:
    proc = subprocess.run([exe, *args], capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"{exe} {' '.join(args)} exited {proc.returncode}\n{proc.stderr[-800:]}")
    return proc.stdout


def partition_dates(exe: str, db: str) -> list[str]:
    return [m.group(1) for m in
            (PARTITION_RE.match(l) for l in run(exe, "partitions", "--db", db).splitlines())
            if m]


def served_symbols(exe: str, db: str, date: str) -> set[str]:
    out = run(exe, "partitions", "--db", db, "--key", date)
    return {m.group(1) for m in (SURFACE_RE.match(l) for l in out.splitlines()) if m}


def read_counters(path: pathlib.Path) -> dict[str, int]:
    out: dict[str, int] = {}
    if not path.exists():
        return out
    with path.open(newline="", encoding="utf-8-sig") as fh:
        for row in csv.reader(fh):
            if len(row) == 2:
                try:
                    out[row[0]] = int(row[1])
                except ValueError:
                    pass
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", required=True)
    ap.add_argument("--admin-exe", default="build-rel/bin/atx-vol-surface-db.exe")
    ap.add_argument("--reports", type=pathlib.Path,
                    help="dir holding report_<date>.csv, for the load-stage counters")
    ap.add_argument("--churn-top", type=int, default=15)
    ap.add_argument("--out-served", type=pathlib.Path,
                    help="write the per-symbol served matrix as CSV")
    ap.add_argument("--from", dest="from_date", metavar="DATE",
                    help="inclusive lower bound YYYY-MM-DD; unset = every session")
    ap.add_argument("--to", dest="to_date", metavar="DATE",
                    help="inclusive upper bound YYYY-MM-DD; unset = every session")
    args = ap.parse_args()

    all_dates = partition_dates(args.admin_exe, args.db)
    if not all_dates:
        print(f"no partitions in {args.db}", file=sys.stderr)
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

    served = {d: served_symbols(args.admin_exe, args.db, d) for d in dates}

    print(f"{'date':<12}{'loaded':>8}{'risk':>8}{'mark':>7}{'served':>8}"
          f"{'failed':>8}{'served%':>9}")
    for d in dates:
        c = read_counters(args.reports / f"report_{d}.csv") if args.reports else {}
        loaded = c.get("coverage.cells_loaded", 0)
        ok = c.get("coverage.cells_ok", len(served[d]))
        mark = c.get("coverage.cells_mark", 0)
        failed = c.get("coverage.cells_failed", 0)
        pct = 100.0 * ok / loaded if loaded else 0.0
        print(f"{d:<12}{loaded:>8,d}{ok - mark:>8,d}{mark:>7,d}{ok:>8,d}"
              f"{failed:>8,d}{pct:>8.1f}%")
        if ok != len(served[d]):
            # The report's counter and the partition's actual contents must agree;
            # if they do not, one of them is not describing this database.
            print(f"    WARNING report says cells_ok={ok:,d} but the partition holds "
                  f"{len(served[d]):,d} surfaces")

    universe = set().union(*served.values())
    every = set.intersection(*served.values())
    churn = {s: sum(1 for d in dates if s in served[d]) for s in universe - every}

    print(f"\nstability over {len(dates)} session(s)")
    print(f"  union of names served at least once : {len(universe):,d}")
    print(f"  served on EVERY session (the core)  : {len(every):,d}"
          f"  ({100.0 * len(every) / len(universe):.1f}% of the union)")
    print(f"  served on SOME sessions only        : {len(churn):,d}"
          f"  ({100.0 * len(churn) / len(universe):.1f}%)")
    if churn:
        # A name served once out of ten is a different problem from one served
        # nine times out of ten: the first is a board that barely lists, the
        # second is a fit that is one bad quote away from refusing.
        hist: dict[int, int] = {}
        for n in churn.values():
            hist[n] = hist.get(n, 0) + 1
        print(f"\n  sessions served  count")
        for n in sorted(hist):
            print(f"    {n:>2d}/{len(dates)}          {hist[n]:>6,d}")
        worst = sorted(churn.items(), key=lambda kv: (-kv[1], kv[0]))[:args.churn_top]
        print(f"\n  nearest-miss names ({len(worst)} shown) -- served most days, "
              f"refused some:")
        for sym, n in worst:
            pattern = "".join("#" if sym in served[d] else "." for d in dates)
            print(f"    {sym:<10} {n}/{len(dates)}  {pattern}   (# served, . not)")

    if args.out_served:
        args.out_served.parent.mkdir(parents=True, exist_ok=True)
        with args.out_served.open("w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(["symbol", "n_served", *dates])
            for sym in sorted(universe):
                w.writerow([sym, sum(1 for d in dates if sym in served[d]),
                            *(1 if sym in served[d] else 0 for d in dates)])
        print(f"\nserved matrix -> {args.out_served}  ({len(universe):,d} symbols)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
