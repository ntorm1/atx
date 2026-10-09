#!/usr/bin/env python3
"""Tally WHY cells and slices were refused, over a set of surface-db build reports.

The build LOG caps its `failed_cell` lines (32 by default, the rest counted in
`coverage.failed_cells_elided`) and the survivors are alphabetically first, so a
reason distribution read off the log is a sample of micro-caps presented as the
population. The `--report` CSV carries the complete lists instead, in FOUR
sections that a reader must not conflate -- they are at different granularities
and the second one is much larger, so a single flat parse silently mixes them:

  1. `key,value`                                  -- run counters
  2. `symbol,n_attempted,n_ok,...`                -- per-symbol outcome counts
  3. `date,symbol,code,detail`                    -- one row per FAILED CELL
  4. `slice_drop.date,symbol,T,outcome,n_used`    -- one row per DROPPED SLICE

Section 3 says which symbols produced no surface at all and why. Section 4 is
the finer and more useful one: it names every expiry that was dropped, INCLUDING
the expiries dropped from symbols that were nevertheless served -- so it measures
partial coverage, which no counter in section 1 reports.

A cell `detail` is a sentence plus a `key=value` tail, e.g.

    fit_curve_surface: no expiry produced a usable slice; chains=9 starved=0
    uncovered=0 carry_failed=9 prep_failed=0 fit_failed=0 calendar_refused=0
    skipped=0; kind=svi prep=configured ...

so the sentence is the STAGE that refused and the tail is the per-expiry
accounting behind it. Both are reported: the stage says where to look, the
accounting says which expiries drove it.

Usage:
  python atx-vol/tools/failure_reasons.py --reports C:/atx-data/logs/prodv1 \
      --census atx-vol/data/universe/census_2026-08-21.csv
"""

from __future__ import annotations

import argparse
import collections
import csv
import pathlib
import re
import sys

KV_RE = re.compile(r"(\w+)=([\w.-]+)")
CELL_HDR = ["date", "symbol", "code", "detail"]
SLICE_HDR = ["slice_drop.date", "symbol", "T", "outcome", "n_used"]
# Per-expiry counters in a cell detail's tail; `chains` is the denominator.
COUNT_KEYS = ("starved", "uncovered", "carry_failed", "prep_failed",
              "fit_failed", "calendar_refused", "skipped")


def sections(path: pathlib.Path) -> tuple[list[list[str]], list[list[str]]]:
    """(failed-cell rows, dropped-slice rows) from one report CSV."""
    with path.open(newline="", encoding="utf-8-sig") as fh:
        rows = list(csv.reader(fh))
    cell_at = slice_at = None
    for i, r in enumerate(rows):
        if r[:4] == CELL_HDR:
            cell_at = i
        elif r[:5] == SLICE_HDR:
            slice_at = i
    cells: list[list[str]] = []
    drops: list[list[str]] = []
    if cell_at is not None:
        end = slice_at if slice_at is not None and slice_at > cell_at else len(rows)
        cells = [r for r in rows[cell_at + 1:end] if len(r) >= 4]
    if slice_at is not None:
        drops = [r for r in rows[slice_at + 1:] if len(r) >= 5]
    return cells, drops


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--reports", type=pathlib.Path, required=True)
    ap.add_argument("--census", type=pathlib.Path,
                    help="census CSV; adds a chain-density breakdown per driver")
    ap.add_argument("--top", type=int, default=10)
    args = ap.parse_args()

    reports = sorted(args.reports.glob("report_*.csv"))
    if not reports:
        print(f"no report_*.csv under {args.reports}", file=sys.stderr)
        return 1

    stages: collections.Counter[str] = collections.Counter()
    drivers: collections.Counter[str] = collections.Counter()
    outcomes: collections.Counter[str] = collections.Counter()
    driver_by_symbol: dict[str, str] = {}
    drop_symbols: set[str] = set()
    n_cells = n_drops = 0

    for rep in reports:
        cells, drops = sections(rep)
        n_cells += len(cells)
        n_drops += len(drops)
        for _date, sym, _code, detail in (r[:4] for r in cells):
            stages[detail.split(":", 1)[0].strip() or "(none)"] += 1
            kv = dict(KV_RE.findall(detail))
            chains = int(kv["chains"]) if kv.get("chains", "").isdigit() else 0
            if chains <= 0:
                drivers["(no chain accounting)"] += 1
                continue
            # Which single counter accounts for EVERY chain of this symbol?
            full = [k for k in COUNT_KEYS
                    if kv.get(k, "").isdigit() and int(kv[k]) == chains]
            key = full[0] if len(full) == 1 else ("mixed" if full else "none")
            drivers[key] += 1
            driver_by_symbol.setdefault(sym, key)
        for _d, sym, _T, outcome, _n in (r[:5] for r in drops):
            outcomes[outcome] += 1
            drop_symbols.add(sym)

    print(f"{n_cells:,d} FAILED CELLS and {n_drops:,d} DROPPED SLICES "
          f"over {len(reports)} session(s)\n")

    print(f"failed cells -- refusing stage")
    print(f"  {'stage':<32}{'count':>10}{'share':>9}")
    for s, n in stages.most_common(args.top):
        print(f"  {s:<32}{n:>10,d}{100.0 * n / max(n_cells, 1):>8.1f}%")

    print(f"\nfailed cells -- the per-expiry counter accounting for EVERY chain")
    print(f"  {'driver':<32}{'count':>10}{'share':>9}")
    for d, n in drivers.most_common(args.top):
        print(f"  {d:<32}{n:>10,d}{100.0 * n / max(n_cells, 1):>8.1f}%")

    print(f"\ndropped slices -- outcome ({len(drop_symbols):,d} distinct symbols affected;"
          f" this INCLUDES symbols that were still served)")
    print(f"  {'outcome':<32}{'count':>10}{'share':>9}")
    for o, n in outcomes.most_common(args.top):
        print(f"  {o:<32}{n:>10,d}{100.0 * n / max(n_drops, 1):>8.1f}%")

    if args.census and args.census.exists():
        import pandas as pd
        cen = pd.read_csv(args.census, keep_default_na=False, na_values=[])
        for c in cen.columns:
            if c != "underlying":
                cen[c] = pd.to_numeric(cen[c], errors="coerce")
        dens = dict(zip(cen["underlying"], cen["n_two_sided"]))
        print(f"\nchain density by cell-failure driver")
        print(f"  {'driver':<24}{'names':>8}{'med n_two':>11}{'p90 n_two':>11}")
        grouped: dict[str, list[int]] = collections.defaultdict(list)
        for sym, key in driver_by_symbol.items():
            v = dens.get(sym)
            if v is not None and pd.notna(v):
                grouped[key].append(int(v))
        for key, vals in sorted(grouped.items(), key=lambda kv: -len(kv[1])):
            s = pd.Series(vals)
            print(f"  {key:<24}{len(vals):>8,d}{s.median():>11.0f}{s.quantile(0.90):>11.0f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
