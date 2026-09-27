"""Construction audit: gross/net leverage, banded share and entry rate per NAV run (read-only).

T28 (mega-alpha v5). Re-derives, from the per-session NAV CSV of the primary scenario, the construction
mechanics of every existing v3/v4/v4.2 NAV output: mean gross/net leverage, held names, members banded per
rebalance, daily one-way GMV turnover, the deployment-decision entry filter, and bounds on the daily entry
rate. Cross-checks against each run's own summary.json. Reads only; writes one JSON (argv[1]).

Run from the repo root (C:/atx-wt/pool-2):
  "C:/Program Files/Python312/python.exe" .superpowers/sdd/mega-alpha-20260926/studies/construction_audit.py \
      .superpowers/sdd/mega-alpha-20260926/construction-audit-v4.json

Column adaptations vs the T28 brief sketch (all brief columns exist in every v3/v4 CSV; none renamed):
  * members = neutralize_used + neutralize_excluded (strategy_price_exposures.cpp neutralize_target: every
    member is either used or excluded). The brief's `banded / neutralize_used` share is kept as
    `banded_share_used`; `banded_share` divides by the true member count N_d that sets the band.
  * a missing column or non-numeric cell yields NaN for that field (never a skipped run).
"""
import csv
import glob
import json
import math
import os
import statistics as st
import sys

SCEN = "daily_modeled-1bn-stale5-v1+swap-fin-v1.csv"
PRIMARY = "modeled-1bn-stale5-v1+swap-fin-v1"
NAN = float("nan")


def num(r: dict, k: str) -> float:
    v = r.get(k, "")
    try:
        return float(v) if v not in ("", "nan", None) else NAN
    except ValueError:
        return NAN


def mean(xs) -> float:
    xs = [x for x in xs if not math.isnan(x)]
    return st.mean(xs) if xs else NAN


def truthy(v) -> bool:
    return v in ("1", "true", "True")


def summary_primary(run: str) -> dict:
    try:
        s = json.load(open(f"{run}/summary.json"))
    except (OSError, ValueError):
        return {}
    for e in s.get("scenarios", []):
        if e.get("scenario") == PRIMARY:
            return e
    return {}


def audit(run: str) -> dict:
    with open(f"{run}/{SCEN}", newline="") as f:
        rows = list(csv.DictReader(f))
    recipe = json.load(open(f"{run}/recipe.json"))
    reb = [r for r in rows if truthy(r.get("rebalance"))]

    members = [num(r, "neutralize_used") + num(r, "neutralize_excluded") for r in reb]
    used = [num(r, "neutralize_used") for r in reb]
    banded = [num(r, "banded_names") for r in reb]
    share = [b / m for b, m in zip(banded, members) if m > 0 and not math.isnan(b)]
    share_used = [b / u for b, u in zip(banded, used) if u > 0 and not math.isnan(b)]

    # Deployment decision = first rebalance row: every member has current weight 0 there, so
    # banded_names counts exactly the members with |desired| <= band (the entry filter, measured).
    d0 = reb[0] if reb else {}
    d0_members = num(d0, "neutralize_used") + num(d0, "neutralize_excluded")
    d0_banded = num(d0, "banded_names")
    frac = recipe.get("trade_fraction")
    d0_planned_gross = num(d0, "planned_gross")

    # Entry-rate bounds (no per-name weight file exists in the NAV outputs). Decision at row d (after the
    # session's execution, so held_d = names with current != 0 at the decision), fills at row d+1.
    # entries(d->d+1) >= max(0, held_{d+1} - held_d)                (held rises only by entries)
    # entries(d->d+1) <= min(members_d - banded_d, fills_{d+1})     (an entry needs gap = |desired| > band
    #                                                               and a fill)
    # denominator: unheld members ~ members_d - held_d (held may include non-member forced exits, so this
    # under-counts unheld names and makes both rates conservative-high). Deployment pair excluded.
    lo, hi = [], []
    deployed = False
    for a, b in zip(rows, rows[1:]):
        if not truthy(a.get("rebalance")) or not truthy(b.get("executed")):
            continue
        if not deployed:  # first executed pair = deployment (current 0 everywhere)
            deployed = num(b, "fills") > 0
            continue
        m = num(a, "neutralize_used") + num(a, "neutralize_excluded")
        unheld = m - num(a, "held_names")
        if not (unheld > 0):
            continue
        dheld = num(b, "held_names") - num(a, "held_names")
        lo.append(max(0.0, dheld) / unheld)
        hi.append(min(m - num(a, "banded_names"), num(b, "fills"), unheld) / unheld)

    g = [x for x in (num(r, "gross_leverage") for r in rows) if not math.isnan(x)]
    sp = summary_primary(run)
    ex, co, dt = sp.get("exposure", {}), sp.get("construction", {}), sp.get("daily_turnover_gmv", {})
    return {
        "run": run.replace("\\", "/"),
        "label": os.path.basename(run),
        "rule": recipe.get("rule"),
        "band_multiple": recipe.get("band_multiple"),
        "trade_fraction": frac,
        "sessions": len(rows),
        "rebalance_rows": len(reb),
        "gross_mean": mean(num(r, "gross_leverage") for r in rows),
        "gross_median": st.median(g) if g else NAN,
        "gross_max": max(g) if g else NAN,
        "gross_last": g[-1] if g else NAN,
        "gmv_mean_dollars": mean(num(r, "pretrade_gross_dollars") for r in rows),
        "net_mean": mean(num(r, "net_leverage") for r in rows),
        "held_mean": mean(num(r, "held_names") for r in rows),
        "members_mean": mean(members),
        "used_mean": mean(used),
        "banded_mean": mean(banded),
        "banded_share": mean(share),
        "banded_share_used": mean(share_used),
        "held_over_members": mean(num(r, "held_names") for r in rows) / mean(members) if members else NAN,
        "tau_gmv_mean": mean(num(r, "one_way_turnover_gmv") for r in rows),
        "amplification_mean": mean(num(r, "neutralize_amplification") for r in reb),
        "deploy_members": d0_members,
        "deploy_banded": d0_banded,
        "deploy_entry_filter_pass": (1 - d0_banded / d0_members) if d0_members > 0 else NAN,
        "deploy_planned_gross": d0_planned_gross,
        "deploy_desired_gross_passing_band": d0_planned_gross / frac if frac else NAN,
        "entry_rate_lower_mean": mean(lo),
        "entry_rate_upper_mean": mean(hi),
        "entry_rate_pairs": len(lo),
        "summary": {
            "mean_gross_leverage": ex.get("mean_gross_leverage", NAN),
            "mean_held_names": ex.get("mean_held_names", NAN),
            "mean_banded_names_per_rebalance": co.get("mean_banded_names_per_rebalance", NAN),
            "net_sharpe": sp.get("net_sharpe", NAN),
            "gross_sharpe": sp.get("gross_sharpe", NAN),
            "tau_gmv_mean_official": dt.get("mean", NAN),
            "tau_gmv_p95_official": dt.get("p95", NAN),
        },
    }


if __name__ == "__main__":
    out = [audit(r) for r in sorted(glob.glob("build-equity/mega-nav-v[34]*"))
           if os.path.isfile(os.path.join(r, SCEN)) and os.path.isfile(os.path.join(r, "recipe.json"))]
    with open(sys.argv[1], "w") as f:
        json.dump(out, f, indent=1)
    for o in out:
        s = o["summary"]
        print(f"{o['label']:30s} b{str(o['band_multiple']):<5} f{str(o['trade_fraction']):<5} gross {o['gross_mean']:.3f} "
              f"net {o['net_mean']:+.3f} held {o['held_mean']:5.0f} banded {o['banded_mean']:5.0f}/"
              f"{o['members_mean']:5.0f} ({o['banded_share']:.3f}; /used {o['banded_share_used']:.3f}) "
              f"tau {o['tau_gmv_mean']:.4f} dep-pass {o['deploy_entry_filter_pass']:.3f} "
              f"dep-gross {o['deploy_desired_gross_passing_band']:.3f} "
              f"entry [{o['entry_rate_lower_mean']:.4f},{o['entry_rate_upper_mean']:.4f}] "
              f"SR {s['net_sharpe']:.3f}/{s['gross_sharpe']:.3f}")
