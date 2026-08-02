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

Aggregate rows (median + ``cv``) come from repeated benchmarks. The corpus
``fit/e2e/{spy_real,100name}`` rows run ``Iterations(1)`` with no repeats (a
single cold fit is ~0.2 s of real work; repeating it would make the canonical
gate impractical), so they emit NO aggregate row at all — only a lone
``iteration`` row. Historically that made them invisible here: never compared,
never flagged when absent, so a regression OR a crash on the two headline fit
rows passed silently. We now FALL BACK to the iteration ``real_time`` for any
benchmark that emitted no aggregate, marking it CV-UNGUARDED (``*`` verdict): it
is compared and, crucially, gates as MISSING if it vanishes (crash), but a ratio
move alone is advisory — a single un-repeated iteration has no CV, and per this
tool's contract a ratio without a trustworthy CV is not signal. A row whose JSON
carries ``error_occurred`` is treated as no data (→ MISSING → fail).

Absolute nanoseconds are pinned to one host, so only RATIOS are gated. The host
metadata Google Benchmark records (num_cpus, mhz_per_cpu, library_build_type,
caches) plus atx-vol's explicit ``atx_build_isa`` context are printed for both
files. SSE2 and AVX2 measurements are different executable contracts: a missing
or mismatched build ISA is refused outright. Other host mismatches raise a loud
warning and downgrade ratio-based regressions to ADVISORY ONLY (printed, exit
0). Missing-benchmark failures are NOT downgraded by a host mismatch: whether a
benchmark ran at all has nothing to do with which host it ran on.

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


def collect_rows(doc: dict) -> Dict[str, Dict[str, float]]:
    """name -> record ({median, cv, mean, p95, ...} in ``real_time`` units).

    Prefer the aggregate rows (they carry median + the custom ``cv`` statistic).
    For any benchmark that emitted NO aggregate row (a single-iteration corpus
    row such as ``fit/e2e/*``), fall back to the lone ``iteration`` row's
    ``real_time`` as the median and flag the record ``unguarded`` — there is a
    value to compare and to gate crashes against, but no CV, so a ratio alone is
    advisory. Rows flagged ``error_occurred`` contribute no data (so an errored
    benchmark reads as MISSING and fails, rather than as a bogus 0 ns).
    """
    aggs: Dict[str, Dict[str, float]] = {}
    iters: Dict[str, list] = {}
    for b in doc.get("benchmarks", []):
        if b.get("error_occurred"):
            continue
        name = b.get("run_name", b.get("name", ""))
        if not name:
            continue
        run_type = b.get("run_type")
        if run_type == "aggregate":
            agg = b.get("aggregate_name", "")
            if not agg:
                continue
            aggs.setdefault(name, {})[agg] = float(b.get("real_time", "nan"))
        elif run_type == "iteration" or run_type is None:
            # ``run_type`` may be absent in older/edge Google Benchmark JSON.
            iters.setdefault(name, []).append(float(b.get("real_time", "nan")))

    out: Dict[str, Dict[str, float]] = dict(aggs)
    for name, values in iters.items():
        if name in out:
            # A repeated benchmark: its iteration rows share the aggregate's
            # run_name and are already represented by the aggregate record.
            continue
        finite = sorted(v for v in values if v == v)  # drop NaN
        if not finite:
            continue
        median = finite[len(finite) // 2]
        # ``unguarded`` marker rides alongside the aggregate-name keys; median_cv
        # only reads ``median``/``cv`` so the extra key is inert there.
        out[name] = {"median": median, "unguarded": 1.0}
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
        "atx_build_isa": ctx.get("atx_build_isa"),
        "caches": cache_sig,
    }


def check_host(base_meta: dict, new_meta: dict) -> bool:
    """Print both hosts; return True if they match on the gate-relevant fields."""
    print("== Host metadata ==")
    fields = ["host_name", "num_cpus", "mhz_per_cpu", "library_build_type",
              "atx_build_isa", "caches"]
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


def check_build_isa(base_meta: dict, new_meta: dict) -> bool:
    """Require a recognized, identical build ISA before comparing any rows."""
    baseline = base_meta.get("atx_build_isa")
    candidate = new_meta.get("atx_build_isa")
    recognized = {"sse2", "avx2"}
    if baseline == candidate and baseline in recognized:
        return True
    baseline_label = baseline if baseline is not None else "missing"
    candidate_label = candidate if candidate is not None else "missing"
    print("ERROR: BUILD ISA MISMATCH - benchmark comparison refused: "
          f"baseline={baseline_label!r}, new={candidate_label!r}. Regenerate both "
          "runs with matching atx_build_isa context (sse2 or avx2).")
    return False


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

    base_meta = host_meta(base_doc)
    new_meta = host_meta(new_doc)
    host_ok = check_host(base_meta, new_meta)
    if not check_build_isa(base_meta, new_meta):
        return 1

    base = collect_rows(base_doc)
    new = collect_rows(new_doc)

    # Column layout.
    hdr = f"{'benchmark':70s} {'base(med)':>13s} {'new(med)':>13s} {'ratio':>7s} {'cv%':>6s}  verdict"
    print(hdr)
    print("-" * len(hdr))

    regressions = 0
    noisy = 0
    compared = 0
    unguarded_moves = 0
    for name in sorted(set(base) & set(new)):
        b_med, _ = median_cv(base[name])
        n_med, n_cv = median_cv(new[name])
        if b_med is None or n_med is None or b_med <= 0.0:
            continue
        compared += 1
        ratio = n_med / b_med
        cv_pct = (n_cv * 100.0) if n_cv is not None else float("nan")
        unguarded = bool(base[name].get("unguarded")) or bool(new[name].get("unguarded"))
        if unguarded:
            # Single-iteration corpus row (no CV). A ratio move is surfaced
            # LOUDLY with a ``*`` so it can never regress silently, but stays
            # advisory — a lone iteration has no CV to justify hard-failing CI on
            # laptop noise (crashes gate via the MISSING path, not here).
            if ratio > 1.0 + args.threshold:
                verdict = "REGRESS?*"
                unguarded_moves += 1
            elif ratio < 1.0 - args.threshold:
                verdict = "IMPROVED*"
            else:
                verdict = "ok*"
        # A missing/None CV on an otherwise-aggregated row is NOT a trustworthy
        # zero — treat it exactly like a too-high CV: flag NOISY, do not gate.
        elif n_cv is None or n_cv > args.cv_max:
            verdict = "NOISY"
            noisy += 1
        elif ratio > 1.0 + args.threshold:
            verdict = "REGRESS"
            regressions += 1
        elif ratio < 1.0 - args.threshold:
            verdict = "IMPROVED"
        else:
            verdict = "ok"
        short = name.replace("/min_warmup_time:0.500/repeats:5", "").replace(
            "/iterations:1/real_time", "")
        print(f"{short:70.70s} {b_med:13.1f} {n_med:13.1f} {ratio:7.3f} {cv_pct:6.2f}  {verdict}")

    only_base = sorted(set(base) - set(new))
    only_new = sorted(set(new) - set(base))
    missing = len(only_base)
    print()
    print(f"compared={compared}  regressions={regressions}  noisy={noisy}  "
          f"unguarded_moves={unguarded_moves}  missing={missing}  "
          f"only_in_new={len(only_new)}")
    if unguarded_moves:
        print(f"  note: {unguarded_moves} CV-unguarded (*) move(s) > {args.threshold*100:.0f}% "
              "on single-iteration corpus row(s) — surfaced, ADVISORY (no CV). "
              "Re-run best-of-3 to confirm before treating as a real regression.")
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
