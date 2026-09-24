"""Iteration-16 per-family finite-admitted coverage across IC cells.

Purpose
-------
Each IC cell (``<data-root>/<prefix>_ic_<year>_<cut>_<stamp>``, year
2013..2019, cut in {t1000, t3000}; missing cells are skipped and reported)
records in ``request.json`` the dict ``families.finite_admitted_cells``
({family: number of admitted (date, name) cells with a finite value}). This
tool sums that count per family across all cells and reports

    coverage_pct = 100 * sum_cells(finite[family]) / max_family(sum_cells(finite))

i.e. the denominator is the maximum summed count over ALL families present
(close-only families are finite on every admitted cell, so the max equals the
admitted-cell total). This is the definition used for the cp20 coverage numbers.

Companion of iteration16_identity_check.py (which applies ruling R18-3 -- CI
columns excluded from the identity contract -- and a last-ulp numeric
tolerance). Coverage is a pure integer count and needs no tolerance.

Usage
-----
python iteration16_family_coverage.py --prefix <p> --stamp <yyyymmdd>
    [--data-root C:/atx/data] [--families a,b,...] [--out <json>] [--verbose]
"""
from __future__ import annotations

import argparse
import json
import os
import sys

YEARS = range(2013, 2020)
CUTS = ("t1000", "t3000")


def finite_counts(request: dict) -> dict[str, int]:
    fin = (request.get("families") or {}).get("finite_admitted_cells")
    if isinstance(fin, dict):
        return {k: int(v) for k, v in fin.items()}
    if isinstance(fin, list):  # tolerate a list-of-objects layout
        return {(it.get("family") or it.get("name")): int(it.get("finite_admitted_cells", it.get("count")))
                for it in fin}
    raise ValueError("request.json has no families.finite_admitted_cells")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Per-family finite-admitted coverage across IC cells.")
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--stamp", required=True)
    ap.add_argument("--data-root", default="C:/atx/data")
    ap.add_argument("--families", default=None, help="comma list to display; default all present")
    ap.add_argument("--out", default=None)
    ap.add_argument("--verbose", action="store_true", help="also print the per-cell breakdown")
    args = ap.parse_args(argv)

    cells: dict[str, dict[str, int]] = {}
    skipped = []
    for year in YEARS:
        for cut in CUTS:
            cell = f"{year}_{cut}"
            path = os.path.join(args.data_root, f"{args.prefix}_ic_{cell}_{args.stamp}", "request.json")
            if not os.path.isfile(path):
                skipped.append(cell)
                continue
            with open(path) as f:
                cells[cell] = finite_counts(json.load(f))
    if not cells:
        print("no cells found", file=sys.stderr)
        return 1

    totals: dict[str, int] = {}
    for counts in cells.values():
        for fam, v in counts.items():
            totals[fam] = totals.get(fam, 0) + v
    denom = max(totals.values())  # over ALL families, regardless of --families
    shown = (sorted(s.strip() for s in args.families.split(",") if s.strip())
             if args.families else sorted(totals))
    unknown = [f for f in shown if f not in totals]
    if unknown:
        print(f"unknown families: {unknown}", file=sys.stderr)
        return 1
    pct = {f: 100.0 * totals[f] / denom for f in shown}

    w = max(len(f) for f in shown + ["family"])
    print(f"cells={len(cells)} skipped={','.join(skipped) or 'none'} denominator={denom}")
    print(f"{'family':<{w}}  {'finite_admitted':>15}  {'coverage_pct':>12}")
    for f in shown:
        print(f"{f:<{w}}  {totals[f]:>15}  {pct[f]:>11.2f}%")
    if args.verbose:
        print()
        print(f"{'cell':<11} " + " ".join(f"{f[:14]:>14}" for f in shown))
        for cell, counts in cells.items():
            print(f"{cell:<11} " + " ".join(f"{str(counts.get(f)):>14}" for f in shown))
    if args.out:
        with open(args.out, "w") as f:
            json.dump({"prefix": args.prefix, "stamp": args.stamp, "denominator": denom,
                       "skipped_cells": skipped, "totals": {f: totals[f] for f in shown},
                       "coverage_pct": pct, "per_cell": cells}, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
