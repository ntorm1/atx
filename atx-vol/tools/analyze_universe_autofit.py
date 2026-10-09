#!/usr/bin/env python3
"""Deep-dive analysis of a universe_autofit results CSV.

Produces: status/error taxonomy, curve-family and profile histograms with quality
stats per bucket, timing breakdown (where the CPU went), scaling fit_ms ~ n_rows,
quality red flags (calendar arb, chi2, in-band, NaN rates), and a weakness report
skeleton with the worst offenders per category.

With `--attempts <csv>` (the `universe_autofit --attempts-out` companion file) it
additionally answers the T1c question: when the routed PRIMARY curve family is
refused and the board is served by a ladder substitute instead, which constraint
did the refusal? Two independent gates can refuse, and the report separates them
because they imply different remedies:

  * FitAdmissionPolicy -- coverage/quality/structure over fit evidence. Reported
    as `SurfaceAdmissionReason` bits out of `admission_failed_checks`.
  * the independent oracle -- surface geometry on a sampled grid. Reported as
    `ValidationFailure` bits out of `att_probe_failures`.
"""

from __future__ import annotations

import argparse
import pathlib

import numpy as np
import pandas as pd

# fit_policy.hpp:119 SurfaceAdmissionReason. `surface_admission_reason_mask`
# (fit_policy.hpp:236) is `1u << (value - 1)`, so bit i below is enumerator i+1.
ADMISSION_REASONS = [
    "BuildFailed", "InsufficientFittedExpiries", "InsufficientExpiryCoverage",
    "InsufficientQuoteCoverage", "FrontExpiryMissing", "ConsecutiveExpiryGap",
    "NonFiniteDiagnostics", "CalendarArbitrage", "QualityBelowFloor",
    "ImpossibleEvidence", "DuplicateMaturity", "FiniteIvDomain",
    "EuropeanPriceBounds", "StrikeMonotonicity", "StrikeConvexity",
    "CalendarTotalVariance", "ForwardVariance", "RequiredTenorBucket",
    "DiagnosticsUnavailable",
]

# surface_policy.hpp:66 ValidationFailure -- already a 1<<n bitmask, so bit i
# below is literally `1u << i`.
VALIDATION_FAILURES = [
    "InvalidDomain", "NonFinite", "PriceBounds", "StrikeMonotonicity",
    "Butterfly", "Calendar", "Wing", "InversionResidual", "TimedOut",
    "StaleInput", "InsufficientData", "CarryGap",
]


def decode(mask: int, names: list[str]) -> list[str]:
    """Bit names set in `mask`; an unknown high bit is named, never dropped."""
    out = [n for i, n in enumerate(names) if mask & (1 << i)]
    rest = mask >> len(names)
    if rest:
        out.append(f"<unknown:{rest << len(names):#x}>")
    return out


def bit_histogram(masks, names, total, title):
    """Rank the individual bits set across `masks`. Bits co-occur, so the shares
    are per-bit incidence and deliberately do not sum to 100%."""
    counts = {}
    for m in masks:
        for name in decode(int(m), names):
            counts[name] = counts.get(name, 0) + 1
    print(f"\n  {title} (n={total}, bits co-occur so shares do not sum to 100%)")
    if not counts:
        print("    (no bits set)")
        return
    for name, c in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f"    {name:28s} {c:5d}  ({100*c/max(total,1):5.1f}%)")


def pct(series, q):
    return float(np.percentile(series.dropna(), q)) if len(series.dropna()) else float("nan")


def analyse_attempts(path: pathlib.Path, top: int) -> None:
    df = pd.read_csv(path, keep_default_na=False, na_values=[""])
    print(f"\n\n=== {path.name}: {len(df)} attempt rows over "
          f"{df.symbol.nunique()} boards ===")

    # ── Integrity first. Every counter in this file is zero on a digest that was
    # never computed, which is byte-identical to a validated-clean one; only
    # `att_probe_ran == 1` distinguishes them. Nothing below reads a counter off
    # a row that fails this guard.
    print("\n-- probe provenance --")
    print(df.att_probe_source.value_counts().to_string())
    ran = df.att_probe_ran == 1
    probed = df.att_probe_source == "probe"
    print(f"  probe_source==probe        {int(probed.sum()):5d}")
    print(f"  att_probe_ran==1           {int(ran.sum()):5d}")
    print(f"  probed but probe_ran==0    {int((probed & ~ran).sum()):5d}   "
          "(digest never populated -- counters unreadable)")

    print("\n-- export fidelity (the probe must reproduce what production did) --")
    for col, scope in (("att_probe_id_matches_served",
                        "published attempt vs served risk_health.validation digest"),
                       ("att_probe_admission_matches",
                        "every probed attempt vs its recorded admission mask")):
        s = df[col]
        n_set, n_ok = int(s.notna().sum()), int((s == 1).sum())
        print(f"  {col:30s} match={n_ok}/{n_set}  mismatch={n_set-n_ok}   [{scope}]")

    # ── Primary vs substitute. Attempt 0 is what routing actually wanted; the
    # primary survived iff the published family is attempt 0's family.
    #
    # Two caveats, both of which bias ONLY the convex-dense row and neither of
    # which touches the parametric families this report exists to explain:
    #   * this is a family comparison, so a convex-dense primary that was refused
    #     and then re-served by strict convex repair counts as "survived";
    #   * a convex-dense probe re-runs that same strict repair (see `probe_health`),
    #     so its geometry counters are a floor, not a total.
    first = df[df.attempt_index == 0].set_index("symbol")
    published_kind = first.published_kind.where(first.report_published == 1)
    survived = published_kind == first.curve_kind
    print(f"\n-- primary curve family, per board (n={len(first)}) --")
    fam = pd.DataFrame({"primary": first.curve_kind, "survived": survived})
    tab = fam.groupby("primary").agg(boards=("survived", "size"),
                                     survived=("survived", "sum"))
    tab["rejected"] = tab.boards - tab.survived
    tab["reject_pct"] = (100 * tab.rejected / tab.boards).round(1)
    print(tab.to_string())

    for family in tab.index:
        sel = first[(first.curve_kind == family) & ~survived]
        if not len(sel):
            continue
        n = len(sel)
        print(f"\n\n### REJECTED {family} PRIMARIES: n={n} "
              f"({100*n/int(tab.boards[family]):.1f}% of {int(tab.boards[family])} "
              f"boards routed to {family}) ###")

        built = sel.build_succeeded == 1
        print(f"\n  build_succeeded=0 (never reached a gate) {int((~built).sum()):5d}  "
              f"({100*(~built).mean():5.1f}%)")
        print(f"  build_succeeded=1 (refused by a gate)    {int(built.sum()):5d}  "
              f"({100*built.mean():5.1f}%)")
        print("\n  stage at refusal:")
        print(sel.stage.value_counts().to_string().replace("\n", "\n    ").rjust(4))

        gated = sel[built]
        if len(gated):
            print(f"\n  -- GATE 1: FitAdmissionPolicy (n={len(gated)} built candidates) --")
            print(f"  admitted=1 (policy passed, refused by the oracle instead) "
                  f"{int((gated.admitted == 1).sum()):5d}")
            print(f"  admitted=0 (policy refused)                               "
                  f"{int((gated.admitted == 0).sum()):5d}")
            print("\n  primary_reason:")
            for k, v in gated.admission_reason.value_counts().items():
                print(f"    {k:28s} {v:5d}  ({100*v/len(gated):5.1f}%)")
            bit_histogram(gated.admission_failed_checks, ADMISSION_REASONS, len(gated),
                          "admission_failed_checks bits")

            g_ran = gated[gated.att_probe_ran == 1]
            print(f"\n  -- GATE 2: independent oracle (n={len(g_ran)} rows with "
                  f"att_probe_ran==1) --")
            if len(g_ran):
                print("\n  att_probe_state:")
                for k, v in g_ran.att_probe_state.value_counts().items():
                    print(f"    {k:28s} {v:5d}  ({100*v/len(g_ran):5.1f}%)")
                bit_histogram(g_ran.att_probe_failures, VALIDATION_FAILURES, len(g_ran),
                              "att_probe_failures bits (ValidationFailure)")

                # CarryGap is the ONE bit that publishes rather than rejects
                # (pricer_fitter.cpp:105 "combined with any other failure it
                # still rejects"), so the set of bits that actually cost the
                # candidate its publish is `failures & ~CarryGap`. Ranking those
                # combinations -- not the marginal bits, which co-occur -- is
                # what names the constraint to fix.
                carry = 1 << VALIDATION_FAILURES.index("CarryGap")
                sig = (g_ran.att_probe_failures.astype(int) & ~carry).map(
                    lambda m: "+".join(decode(m, VALIDATION_FAILURES)) or "(CarryGap only)")
                print(f"\n  rejecting-cause combination, CarryGap excluded "
                      f"(n={len(g_ran)}, exhaustive and mutually exclusive)")
                for k, v in sig.value_counts().items():
                    print(f"    {k:44s} {v:5d}  ({100*v/len(g_ran):5.1f}%)")
                geo = ["NonFinite", "PriceBounds", "StrikeMonotonicity", "Butterfly",
                       "Calendar", "Wing"]
                geo_mask = sum(1 << VALIDATION_FAILURES.index(g) for g in geo)
                hit = (g_ran.att_probe_failures.astype(int) & geo_mask) != 0
                print(f"\n  ANY geometry bit ({'|'.join(geo)}):"
                      f" {int(hit.sum())}/{len(g_ran)} ({100*hit.mean():.1f}%)")
                print("  geometry counters, non-zero rows:")
                for col in ("att_n_non_finite", "att_n_price_bound_violations",
                            "att_n_strike_monotonicity_violations",
                            "att_n_butterfly_violations", "att_n_calendar_violations",
                            "att_n_wing_violations"):
                    nz = (g_ran[col].astype(float) > 0)
                    print(f"    {col:38s} {int(nz.sum()):5d}  ({100*nz.mean():5.1f}%)"
                          f"   max={g_ran[col].max()}")
                print(f"  oracle grid actually sampled: strike med="
                      f"{g_ran.att_n_strike_samples.median():.0f} "
                      f"calendar med={g_ran.att_n_calendar_samples.median():.0f} "
                      f"slices med={g_ran.att_n_slices.median():.0f}")

            qbf = gated[gated.admission_failed_checks.astype(int)
                        & (1 << ADMISSION_REASONS.index("QualityBelowFloor")) != 0]
            if len(qbf):
                print(f"\n  -- QualityBelowFloor detail (n={len(qbf)}) --")
                s = qbf.ev_worst_in_band.astype(float)
                print(f"  ev_worst_in_band: min={s.min():.4f} p25={pct(s,25):.4f} "
                      f"med={s.median():.4f} p75={pct(s,75):.4f} max={s.max():.4f}")
                q = qbf.ev_fitted_quotes.astype(float) / qbf.ev_attempted_quotes.astype(float).clip(lower=1)
                print(f"  fitted/attempted quotes: min={q.min():.4f} med={q.median():.4f} "
                      f"max={q.max():.4f}")
                e = qbf.ev_fitted_expiries.astype(float) / qbf.ev_attempted_expiries.astype(float).clip(lower=1)
                print(f"  fitted/attempted expiries: min={e.min():.4f} med={e.median():.4f} "
                      f"max={e.max():.4f}")
                print(f"  ev_calendar_arb_free==1: {int((qbf.ev_calendar_arb_free==1).sum())}"
                      f"/{len(qbf)}")

            print(f"\n  -- worst {top} by ev_worst_in_band --")
            cols = ["symbol", "curve_kind", "admission_reason", "admission_failed_checks",
                    "ev_worst_in_band", "ev_fitted_expiries", "ev_attempted_expiries",
                    "att_probe_state", "att_probe_failures"]
            print(gated.reset_index().nsmallest(top, "ev_worst_in_band")[cols]
                  .round(4).to_string(index=False))

        nb = sel[~built]
        if len(nb):
            print(f"\n  -- build failures (n={len(nb)}), first 100 chars --")
            print(nb.failure.astype(str).str[:100].value_counts().head(10).to_string())


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", type=pathlib.Path)
    ap.add_argument("--attempts", type=pathlib.Path,
                    help="companion CSV from `universe_autofit --attempts-out`")
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--attempts-only", action="store_true",
                    help="skip the per-board report; only answer the T1c question")
    args = ap.parse_args()

    if args.attempts_only:
        if args.attempts is None:
            ap.error("--attempts-only requires --attempts")
        analyse_attempts(args.attempts, args.top)
        return 0

    df = pd.read_csv(args.csv)
    n = len(df)
    ok = df[df.status == "ok"]
    print(f"=== {args.csv.name}: {n} symbols, {len(ok)} ok ({100*len(ok)/max(n,1):.1f}%) ===\n")

    print("-- status --")
    print(df.status.value_counts().to_string())

    bad = df[df.status != "ok"]
    if len(bad):
        print("\n-- error taxonomy (first 100 chars) --")
        print(bad.error.astype(str).str[:100].value_counts().head(25).to_string())

    if not len(ok):
        return 0

    print("\n-- chosen curve family (ok) --")
    fam = ok.groupby("chosen_kind").agg(
        n=("symbol", "size"),
        fit_ms_med=("fit_ms", "median"),
        fit_ms_p90=("fit_ms", lambda s: pct(s, 90)),
        in_band_med=("mean_in_band", "median"),
        chi2_med=("mean_chi2", "median"),
        cal_ok=("calendar_arb_free", "mean"),
    )
    print(fam.round(3).to_string())

    print("\n-- profile (ok) --")
    prof = ok.groupby("profile").agg(
        n=("symbol", "size"),
        rows_med=("n_rows", "median"),
        fit_ms_med=("fit_ms", "median"),
        in_band_med=("mean_in_band", "median"),
    )
    print(prof.round(3).to_string())

    print("\n-- decision source (ok) --")
    print(ok.decision_source.value_counts().to_string())

    print("\n-- timing (ok) --")
    for col in ("load_ms", "chain_ms", "fit_ms", "value_ms"):
        s = ok[col]
        print(f"  {col:9s} sum={s.sum()/1e3:8.1f}s  med={s.median():8.1f}  "
              f"p90={pct(s,90):8.1f}  p99={pct(s,99):8.1f}  max={s.max():9.1f}")
    total = ok[["load_ms", "chain_ms", "fit_ms", "value_ms"]].sum().sum()
    for col in ("load_ms", "chain_ms", "fit_ms", "value_ms"):
        print(f"  {col:9s} share={100*ok[col].sum()/total:5.1f}%")

    # cost model: fit_ms per quote row
    with np.errstate(divide="ignore", invalid="ignore"):
        per_row = ok.fit_ms / ok.n_rows.replace(0, np.nan)
    print(f"\n  fit_ms/row: med={per_row.median():.3f} p90={pct(per_row,90):.3f}")

    print("\n-- quality red flags (ok boards) --")
    flags = {
        "calendar_arb (not free)": (ok.calendar_arb_free == 0),
        "mean_chi2 > 5": (ok.mean_chi2 > 5),
        "mean_in_band < 0.5": (ok.mean_in_band < 0.5),
        "worst_in_band < 0.1": (ok.worst_in_band < 0.1),
        "price NaN > 1%": (ok.n_price_nan > 0.01 * ok.n_valued.clip(lower=1)),
        "bid_iv NaN > 50%": (ok.n_bidiv_nan > 0.5 * ok.n_valued.clip(lower=1)),
        "selector ran (CV path)": (ok.selector_ran == 1),
        "used_fallback": (ok.used_fallback == 1),
    }
    for name, mask in flags.items():
        print(f"  {name:26s} {int(mask.sum()):5d}  ({100*mask.mean():5.1f}%)")

    print(f"\n-- slowest {args.top} fits --")
    cols = ["symbol", "n_rows", "chosen_kind", "profile", "decision_source",
            "fit_ms", "value_ms", "mean_in_band", "mean_chi2"]
    print(ok.nlargest(args.top, "fit_ms")[cols].round(2).to_string(index=False))

    print(f"\n-- worst quality {args.top} (by mean_in_band) --")
    print(ok.nsmallest(args.top, "mean_in_band")[cols].round(2).to_string(index=False))

    small = ok[ok.n_rows < 50]
    print(f"\n-- tiny boards (<50 rows): {len(small)} ok; "
          f"of all {int((df.n_rows < 50).sum())} --")

    if args.attempts is not None:
        analyse_attempts(args.attempts, args.top)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
