#!/usr/bin/env python3
"""Deep-dive analysis of a universe_autofit results CSV.

Produces: status/error taxonomy, curve-family and profile histograms with quality
stats per bucket, timing breakdown (where the CPU went), scaling fit_ms ~ n_rows,
quality red flags (calendar arb, chi2, in-band, NaN rates), and a weakness report
skeleton with the worst offenders per category.
"""

from __future__ import annotations

import argparse
import pathlib

import numpy as np
import pandas as pd


def pct(series, q):
    return float(np.percentile(series.dropna(), q)) if len(series.dropna()) else float("nan")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", type=pathlib.Path)
    ap.add_argument("--top", type=int, default=20)
    args = ap.parse_args()

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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
