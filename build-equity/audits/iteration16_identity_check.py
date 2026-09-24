"""Iteration-16 IC-cell identity check: old checkpoint vs new checkpoint.

Purpose
-------
When a new scorecard checkpoint adds signals/families, the point series of the
signals retained from the previous checkpoint must be unchanged. This tool
compares, per per-year x cut IC cell
(``<data-root>/<prefix>_ic_<year>_<cut>_<stamp>``, year 2013..2019,
cut in {t1000, t3000}), for each requested signal:

* ``ic.csv``              -- all columns;
* ``quantile_spread.csv`` -- all columns EXCEPT those ending ``_lo`` / ``_hi``
  (bootstrap CI bounds). Ruling R18-3: engine-level CIs are not part of the
  identity contract (they depend on the resampling stream, which legitimately
  changes when the signal set changes), so they are dropped before comparing;
* ``signal_autocorr.csv`` -- all columns.

Rows are matched by key (ic: date_index/signal/horizon/variant/restriction;
quantile_spread: signal/horizon/variant/restriction/quantile; autocorr:
signal/lag). Values are compared numerically with a last-ulp tolerance
(default rel 1e-9 / abs 1e-12): a reordered floating-point reduction can move
the last few ulps without any semantic change, so "identical" means within
tolerance; the byte-identical count is reported beside it. NaN == NaN;
non-numeric fields must match exactly.

A (cell, signal, file) is identical when the kept headers are equal, both sides
have the same non-zero row count, the same key set, and every row is within
tolerance. Cells whose directory is missing on either side are skipped and
reported.

Output: per cell and file,
``<cell>: <file>: identical <k>/<n> (byte <b>/<n>, maxrel <x>)`` then
``IDENTITY PASS`` or ``IDENTITY FAIL`` with the failing (cell, signal, file)
triples. Exit code 0 on PASS, 1 on FAIL. ``--out`` writes full JSON detail.

Usage
-----
python iteration16_identity_check.py --old-prefix <p> --new-prefix <p> --stamp <yyyymmdd>
    [--new-stamp <yyyymmdd>] [--signals a,b,...] [--out <json>]
    [--data-root C:/atx/data] [--rel-tol 1e-9] [--abs-tol 1e-12]
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys

# The 18 signals retained from checkpoint 19 (default comparison set).
DEFAULT_SIGNALS = [
    "momentum_252", "momentum_126", "blend_equal", "reversal_21", "reversal_5",
    "high52_proximity", "low_vol_63", "idio_vol_63", "amihud_21", "volume_shock_63",
    "continuation_5", "high_vol_63", "volume_shock_neg_63", "momentum_volscaled_252",
    "residual_momentum_252", "max_ret_21", "low_dollar_volume_21", "mom_resid_vs_blend",
]
YEARS = range(2013, 2020)
CUTS = ("t1000", "t3000")
KEY_COLUMNS = ("date_index", "signal", "horizon", "variant", "restriction", "quantile", "lag")
# (file name, drop CI columns per R18-3)
FILES = (("ic.csv", False), ("quantile_spread.csv", True), ("signal_autocorr.csv", False))


def value_same(a: str, b: str, rel_tol: float, abs_tol: float) -> tuple[bool, float]:
    """Return (within tolerance, relative diff)."""
    if a == b:
        return True, 0.0
    try:
        x, y = float(a), float(b)
    except ValueError:
        return False, math.inf
    if math.isnan(x) and math.isnan(y):
        return True, 0.0
    if math.isnan(x) or math.isnan(y):
        return False, math.inf
    d = abs(x - y)
    r = d / max(abs(x), abs(y), 1e-300)
    return (d <= abs_tol or r <= rel_tol), r


def row_same(ra, rb, rel_tol: float, abs_tol: float) -> tuple[bool, float]:
    if ra is None or rb is None or len(ra) != len(rb):
        return False, math.inf
    worst = 0.0
    for a, b in zip(ra, rb):
        ok, r = value_same(a, b, rel_tol, abs_tol)
        if not ok:
            return False, r
        worst = max(worst, r)
    return True, worst


def load(path: str, drop_ci: bool, signals: set[str]):
    """Return ({signal: {key: row_tuple}}, kept_header, duplicate_key_count)."""
    with open(path, newline="") as f:
        # Materialise inside the with-block (never a generator over a closed
        # file); skip '#' comment lines (quantile_spread.csv has one).
        table = list(csv.reader([line for line in f if not line.startswith("#")]))
    hdr = table[0]
    keep = [i for i, h in enumerate(hdr) if not (drop_ci and (h.endswith("_lo") or h.endswith("_hi")))]
    key_idx = [i for i in keep if hdr[i] in KEY_COLUMNS]
    sig = hdr.index("signal")
    out: dict[str, dict[tuple, tuple]] = {}
    dups = 0
    for row in table[1:]:
        if row[sig] not in signals:
            continue
        key = tuple(row[i] for i in key_idx)
        bucket = out.setdefault(row[sig], {})
        if key in bucket:
            dups += 1
        bucket[key] = tuple(row[i] for i in keep)
    return out, [hdr[i] for i in keep], dups


def compare_file(old_path, new_path, drop_ci, signals, rel_tol, abs_tol) -> dict:
    a, ha, da = load(old_path, drop_ci, set(signals))
    b, hb, db = load(new_path, drop_ci, set(signals))
    header_equal = ha == hb
    per = {}
    for s in signals:
        ra, rb = a.get(s, {}), b.get(s, {})
        extra = sum(1 for k in rb if k not in ra)
        byte_mism = sum(1 for k, v in ra.items() if rb.get(k) != v) + extra
        mism, worst, worst_fail = 0, 0.0, 0.0
        for k, v in ra.items():
            ok, r = row_same(v, rb.get(k), rel_tol, abs_tol)
            if ok:
                worst = max(worst, r)
            else:
                mism += 1
                worst_fail = max(worst_fail, r)
        mism += extra
        n_old, n_new = len(ra), len(rb)
        identical = header_equal and mism == 0 and n_old == n_new > 0
        per[s] = {
            "rows_old": n_old, "rows_new": n_new, "mismatch_rows": mism,
            "byte_mismatch_rows": byte_mism, "max_rel_diff_within_tol": worst,
            "max_rel_diff_failing": worst_fail if mism else None,
            "identical": identical, "byte_identical": identical and byte_mism == 0,
        }
    return {"header_equal": header_equal, "header_old": ha, "header_new": hb,
            "duplicate_keys_old": da, "duplicate_keys_new": db, "signals": per}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Identity check of retained-signal IC outputs between two checkpoints.")
    ap.add_argument("--old-prefix", required=True)
    ap.add_argument("--new-prefix", required=True)
    ap.add_argument("--stamp", required=True, help="old-side stamp (and new-side unless --new-stamp)")
    ap.add_argument("--new-stamp", default=None)
    ap.add_argument("--signals", default=None, help="comma list; default = the 18 cp19-retained signals")
    ap.add_argument("--out", default=None, help="write full per-cell JSON detail here")
    ap.add_argument("--data-root", default="C:/atx/data")
    ap.add_argument("--rel-tol", type=float, default=1e-9)
    ap.add_argument("--abs-tol", type=float, default=1e-12)
    args = ap.parse_args(argv)

    new_stamp = args.new_stamp or args.stamp
    signals = ([s.strip() for s in args.signals.split(",") if s.strip()]
               if args.signals else list(DEFAULT_SIGNALS))
    n = len(signals)
    report = {"old_prefix": args.old_prefix, "new_prefix": args.new_prefix, "old_stamp": args.stamp,
              "new_stamp": new_stamp, "signals": signals, "rel_tol": args.rel_tol,
              "abs_tol": args.abs_tol, "cells": {}, "skipped_cells": [], "failures": []}

    for year in YEARS:
        for cut in CUTS:
            cell = f"{year}_{cut}"
            od = os.path.join(args.data_root, f"{args.old_prefix}_ic_{cell}_{args.stamp}")
            nd = os.path.join(args.data_root, f"{args.new_prefix}_ic_{cell}_{new_stamp}")
            if not (os.path.isdir(od) and os.path.isdir(nd)):
                report["skipped_cells"].append({"cell": cell, "old_exists": os.path.isdir(od),
                                                "new_exists": os.path.isdir(nd)})
                continue
            rep = {}
            for fn, drop_ci in FILES:
                op, np_ = os.path.join(od, fn), os.path.join(nd, fn)
                if not (os.path.isfile(op) and os.path.isfile(np_)):
                    rep[fn] = {"missing_file": True}
                    report["failures"].extend([cell, s, fn] for s in signals)
                    print(f"{cell}: {fn}: MISSING FILE")
                    continue
                r = compare_file(op, np_, drop_ci, signals, args.rel_tol, args.abs_tol)
                sig = r["signals"]
                k = sum(1 for v in sig.values() if v["identical"])
                b = sum(1 for v in sig.values() if v["byte_identical"])
                maxrel = max((v["max_rel_diff_within_tol"] for v in sig.values()), default=0.0)
                r.update(identical=k, byte_identical=b, n=n, max_rel_diff_within_tol=maxrel)
                rep[fn] = r
                report["failures"].extend([cell, s, fn] for s, v in sig.items() if not v["identical"])
                print(f"{cell}: {fn}: identical {k}/{n} (byte {b}/{n}, maxrel {maxrel:.1e})")
            report["cells"][cell] = rep

    for sk in report["skipped_cells"]:
        print(f"SKIPPED {sk['cell']}: old_exists={sk['old_exists']} new_exists={sk['new_exists']}")
    ok = not report["failures"] and bool(report["cells"])
    report["ok"] = ok
    report["n_cells"] = len(report["cells"])
    summary = f"cells={report['n_cells']} skipped={len(report['skipped_cells'])}"
    if ok:
        print(f"IDENTITY PASS {summary}")
    else:
        fails = "; ".join(f"({c}, {s}, {f})" for c, s, f in report["failures"]) or "no cells compared"
        print(f"IDENTITY FAIL {summary} failures: {fails}")
    if args.out:
        with open(args.out, "w") as f:
            json.dump(report, f, indent=1)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
