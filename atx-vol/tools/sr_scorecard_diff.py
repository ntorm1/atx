"""Diff two sr_fit_scorecard receipts, so a change is reported as a movement.

A single scorecard says where we stand; it cannot say whether a change helped.
Run it before and after, then diff the two receipts here. The point is to make an
improvement claim falsifiable rather than impressionistic: every band prints its
own before/after, so a headline gain that is really one band improving while
three regress cannot hide inside an average.

DIRECTION IS NOT UNIFORM ACROSS THESE METRICS, and getting it wrong silently
inverts the verdict, so it is declared per metric rather than assumed:
  contain_pct     higher is better  (share inside the vendor's own quoted band)
  nerr / escape   lower  is better  (disagreement measured in band-widths)
  vol_mae_bp      lower  is better
  price_mae_tk    lower  is better
  vol_bias_bp     TOWARD ZERO is better -- it is signed, and a bias that flips
                  from +30 to -30 has not improved at all even though a naive
                  "lower is better" reading would call it a 60 bp win.
  tau_bias_pct    TOWARD ZERO, same reasoning.

`sr_contain_pct` is printed but never scored: it is the vendor's own containment
against its own quotes, a property of the data rather than of our fit, so it
should be nearly IDENTICAL in both receipts. If it moves, the two runs did not
score the same population and the whole comparison is void -- that is why it is
checked and loudly flagged instead of quietly displayed.
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib

# metric -> ("up" better, "down" better, "zero" = toward zero better)
DIRECTION = {
    "contain_pct": "up",
    "nerr": "down",
    "escape": "down",
    "vol_mae_bp": "down",
    "vol_p95_bp": "down",
    "price_mae_tk": "down",
    "vol_bias_bp": "zero",
    "vol_bias_rel_pct": "zero",
    "price_bias_tk": "zero",
    "tau_bias_pct": "zero",
}

HEADLINE = ["contain_pct", "nerr", "escape", "vol_bias_bp", "vol_mae_bp",
            "price_bias_tk", "price_mae_tk", "tau_bias_pct"]


def verdict(metric: str, before: float, after: float) -> str:
    """One of '++' better, '--' worse, '  ' negligible."""
    if before is None or after is None:
        return "  "
    if any(x is None or (isinstance(x, float) and math.isnan(x)) for x in (before, after)):
        return "  "
    d = DIRECTION.get(metric, "down")
    if d == "zero":
        gain = abs(before) - abs(after)
    elif d == "up":
        gain = after - before
    else:
        gain = before - after
    scale = max(abs(before), abs(after), 1e-9)
    if abs(gain) / scale < 0.01:      # under 1% of the level: call it noise
        return "  "
    return "++" if gain > 0 else "--"


def fmt(metric: str, b: dict, a: dict) -> str:
    bv, av = b.get(metric), a.get(metric)
    if bv is None and av is None:
        return f"{'':>26}"
    if bv is None or av is None:
        return f"{'n/a':>26}"
    return f"{bv:>+10.2f}{av:>+10.2f} {verdict(metric, bv, av):>3}"


def band_index(receipt: dict, key: str) -> dict:
    return {row["band"]: row for row in receipt.get(key, []) if "band" in row}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="diff two sr_fit_scorecard receipts")
    ap.add_argument("--before", required=True, type=pathlib.Path)
    ap.add_argument("--after", required=True, type=pathlib.Path)
    ap.add_argument("--metric", default="", help="show only this metric across all bands")
    a = ap.parse_args(argv)

    b = json.loads(a.before.read_text())
    aft = json.loads(a.after.read_text())

    print(f"before : {b.get('label') or a.before.name}")
    print(f"after  : {aft.get('label') or a.after.name}")
    print(f"scope  : {b.get('scope')}  |  rows scored {b.get('rows_scored')} -> "
          f"{aft.get('rows_scored')}")
    if b.get("rows_scored") != aft.get("rows_scored"):
        print("  NOTE: the scored population CHANGED between runs. Band-level moves "
              "may reflect membership, not fit quality.")
    bo, ao = b["overall"], aft["overall"]
    sc_b, sc_a = bo.get("sr_contain_pct"), ao.get("sr_contain_pct")
    if sc_b is not None and sc_a is not None and abs(sc_b - sc_a) > 0.05:
        print(f"  *** VOID: vendor self-containment moved {sc_b:.2f} -> {sc_a:.2f}. "
              f"The two runs did not score the same population. ***")

    metrics = [a.metric] if a.metric else HEADLINE
    print()
    print(f"{'metric':<18}{'before':>10}{'after':>10}{'':>4}")
    print("-" * 42)
    for m in metrics:
        print(f"{m:<18}{fmt(m, bo, ao)}")

    for key, title in [("by_dte", "BY TENOR"), ("by_z", "BY MONEYNESS"),
                       ("by_underlier", "BY NAME")]:
        bi, ai = band_index(b, key), band_index(aft, key)
        if not bi:
            continue
        shown = metrics if a.metric else ["contain_pct", "nerr", "vol_bias_bp",
                                          "vol_mae_bp", "price_mae_tk"]
        print(f"\n{title}")
        print(f"{'band':<16}" + "".join(f"{m[:12]:>24}" for m in shown))
        for band in bi:
            if band not in ai:
                continue
            row = f"{band:<16}"
            for m in shown:
                row += fmt(m, bi[band], ai[band]) + " "
            print(row)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
