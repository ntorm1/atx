#!/usr/bin/env python3
"""Compare two Google Benchmark JSON runs and gate on regressions.

Reads a BASELINE and a NEW Google Benchmark JSON (``--benchmark_out_format=json``),
matches benchmarks by name, and fails on either:

  * a statistically significant regression: new/baseline median ratio > 1.10 AND
    the new run's coefficient of variation (the custom ``cv`` statistic this
    suite emits) is <= 5%. A noisy new run (CV > 5%, OR the ``cv`` statistic is
    missing entirely) is reported ``NOISY`` and never fails the gate on ratio
    alone — a 10% move under unknown or >20% noise is not signal, and a missing
    CV is not evidence of a CV of zero; or
  * a benchmark present in BASELINE but ABSENT from the new run — e.g. it
    crashed and produced no output. That is a regression until proven
    otherwise, so it fails loudly by name rather than silently passing as a
    bare count. Pass ``--allow-missing`` if a benchmark was deliberately removed.

Absolute nanoseconds are pinned to one host, so only RATIOS are gated. The host
metadata Google Benchmark records (num_cpus, mhz_per_cpu, library_build_type,
caches) is printed for both files. On any mismatch, a loud warning is raised and
ratio-based regressions are downgraded to ADVISORY ONLY (printed, exit 0) —
comparing ratios across different silicon or a debug-vs-release build is
invalid, so gating on them would be gating on noise. Missing-benchmark failures
are NOT downgraded by a host mismatch: whether a benchmark ran at all has
nothing to do with which host it ran on.

stdlib only. Exit code 0 = no gated regression; 1 = at least one regression on a
matching host, or a missing benchmark (without --allow-missing), or a
usage/parse error.

Usage:
    python compare_baseline.py BASELINE.json NEW.json [--threshold 0.10] [--cv-max 0.05] [--allow-missing]
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Dict, Optional, Tuple


def load(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def aggregates(doc: dict) -> Dict[str, Dict[str, float]]:
    """name -> {median, cv, mean, p95} pulled from the aggregate rows (real_time)."""
    out: Dict[str, Dict[str, float]] = {}
    for b in doc.get("benchmarks", []):
        if b.get("run_type") != "aggregate":
            continue
        base = b.get("run_name", b.get("name", ""))
        agg = b.get("aggregate_name", "")
        if not base or not agg:
            continue
        out.setdefault(base, {})[agg] = float(b.get("real_time", "nan"))
    return out


def host_meta(doc: dict) -> dict:
    ctx = doc.get("context", {})
    caches = ctx.get("caches", [])
    cache_sig = ";".join(
        f"{c.get('type')}:{c.get('level')}:{c.get('size')}" for c in caches
    )
    return {
        "host_name": ctx.get("host_name"),
        "num_cpus": ctx.get("num_cpus"),
        "mhz_per_cpu": ctx.get("mhz_per_cpu"),
        "library_build_type": ctx.get("library_build_type"),
        "caches": cache_sig,
    }


def check_host(base_meta: dict, new_meta: dict) -> bool:
    """Print both hosts; return True if they match on the gate-relevant fields."""
    print("== Host metadata ==")
    fields = ["host_name", "num_cpus", "mhz_per_cpu", "library_build_type", "caches"]
    ok = True
    for f in fields:
        b, n = base_meta.get(f), new_meta.get(f)
        flag = "" if b == n else "   <-- MISMATCH"
        if b != n:
            ok = False
        # caches can be long; keep it on its own compact line
        print(f"  {f:20s} baseline={b!r}  new={n!r}{flag}")
    if new_meta.get("library_build_type") not in (None, "release"):
        print("  WARNING: new run is NOT a release build — absolute numbers are not "
              "the SSE2 baseline the sprint gates against.")
    if not ok:
        print("  !! LOUD WARNING: host/build metadata differs — RATIOS ACROSS THESE "
              "RUNS ARE NOT COMPARABLE. Regenerate the baseline on this host, or treat "
              "the verdicts below as advisory only.")
    print()
    return ok


def median_cv(rec: Dict[str, float]) -> Tuple[Optional[float], Optional[float]]:
    return rec.get("median"), rec.get("cv")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("baseline", help="baseline benchmark JSON")
    ap.add_argument("new", help="new benchmark JSON")
    ap.add_argument("--threshold", type=float, default=0.10,
                    help="fractional regression that fails the gate (default 0.10 = 10%%)")
    ap.add_argument("--cv-max", type=float, default=0.05,
                    help="max new-run CV (fraction) to trust a regression (default 0.05 = 5%%)")
    ap.add_argument("--allow-missing", action="store_true",
                    help="do not fail the gate when a baseline benchmark is absent from "
                         "the new run (e.g. it was deliberately deleted)")
    args = ap.parse_args()

    try:
        base_doc, new_doc = load(args.baseline), load(args.new)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR reading input: {exc}", file=sys.stderr)
        return 1

    host_ok = check_host(host_meta(base_doc), host_meta(new_doc))

    base = aggregates(base_doc)
    new = aggregates(new_doc)

    # Column layout.
    hdr = f"{'benchmark':70s} {'base(med)':>13s} {'new(med)':>13s} {'ratio':>7s} {'cv%':>6s}  verdict"
    print(hdr)
    print("-" * len(hdr))

    regressions = 0
    noisy = 0
    compared = 0
    for name in sorted(set(base) & set(new)):
        b_med, _ = median_cv(base[name])
        n_med, n_cv = median_cv(new[name])
        if b_med is None or n_med is None or b_med <= 0.0:
            continue
        compared += 1
        ratio = n_med / b_med
        cv_pct = (n_cv * 100.0) if n_cv is not None else float("nan")
        # A missing/None CV is NOT a trustworthy zero — treat it exactly like a
        # too-high CV: flag NOISY rather than gating the ratio.
        if n_cv is None or n_cv > args.cv_max:
            verdict = "NOISY"
            noisy += 1
        elif ratio > 1.0 + args.threshold:
            verdict = "REGRESS"
            regressions += 1
        elif ratio < 1.0 - args.threshold:
            verdict = "IMPROVED"
        else:
            verdict = "ok"
        short = name.replace("/min_warmup_time:0.500/repeats:5", "")
        print(f"{short:70.70s} {b_med:13.1f} {n_med:13.1f} {ratio:7.3f} {cv_pct:6.2f}  {verdict}")

    only_base = sorted(set(base) - set(new))
    only_new = sorted(set(new) - set(base))
    missing = len(only_base)
    print()
    print(f"compared={compared}  regressions={regressions}  noisy={noisy}  "
          f"missing={missing}  only_in_new={len(only_new)}")
    if only_new:
        print("  note: benchmarks only in NEW run (not gated):",
              ", ".join(n.replace('/min_warmup_time:0.500/repeats:5', '') for n in only_new[:8]),
              "..." if len(only_new) > 8 else "")

    if only_base:
        print()
        print("!! MISSING: benchmark(s) present in BASELINE but ABSENT from the NEW run "
              "(crashed / produced no output?). A vanished benchmark is a regression "
              "until proven otherwise:")
        for n in only_base:
            print(f"    - {n.replace('/min_warmup_time:0.500/repeats:5', '')}")
        if args.allow_missing:
            print("  --allow-missing set: not gating on the above.")
        else:
            print("  Pass --allow-missing if these were deliberately removed.")

    if not host_ok:
        print("\n!! host/build metadata mismatch: ratio-based regressions above are "
              "ADVISORY ONLY (see loud warning above) — absolute numbers (and hence "
              "ratios) are not comparable across hosts/builds.")

    fail_reasons = []
    # Ratio regressions only gate on a matching host — the whole basis for a
    # ratio (the two runs being on comparable silicon/build) is absent otherwise.
    if regressions and host_ok:
        fail_reasons.append(
            f"{regressions} significant regression(s) > {args.threshold*100:.0f}% "
            f"at CV <= {args.cv_max*100:.0f}%")
    elif regressions and not host_ok:
        print(f"  ({regressions} regression(s) at trusted CV would gate on matching hardware)")
    # A missing benchmark gates regardless of host: whether it ran has nothing
    # to do with which host it ran on.
    if missing and not args.allow_missing:
        fail_reasons.append(f"{missing} benchmark(s) missing from the new run")

    if fail_reasons:
        print(f"\nRESULT: FAIL - {'; '.join(fail_reasons)}.")
        return 1
    suffix = " (host mismatch: ratio verdicts advisory)" if not host_ok else ""
    print(f"\nRESULT: PASS{suffix} - no gated regression.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
