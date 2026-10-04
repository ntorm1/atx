"""WQ-explore: compare WQ-variant NAV replays with the primary (Release re-run, bit-identical to p9-b0-yf0-nav).
Reads summary.json / daily CSVs / capacity_curve.csv / composition weights; house paired statistics
(nav_summ.paired_stats: Memmel SE, circular-block-bootstrap CI, Ledoit-Wolf studentized p) on aligned daily nets;
house DSR (backtest_integrity.expected_max_sr / deflated_sharpe; V from the ledger, read only) at several N.
Exploratory, in-sample (TRAIN), off-ledger.

usage: wq_compare.py [NAME:NAVDIR ...]   (default: every nav* dir under build-equity/wq-explore/*)
"""
from __future__ import annotations
import csv
import json
import math
import sys
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[4]
for p in ("atx-impl/tools", "atx-engine/tools"):
    sys.path.insert(0, str(ROOT / p))
import backtest_integrity as BI  # noqa: E402
import dsr_total as DT  # noqa: E402
import nav_summ as NS  # noqa: E402
import research_window as rw  # noqa: E402
import fit_composition_weights as F  # noqa: E402

X = ROOT / "build-equity/wq-explore"
ANN = math.sqrt(252)
SCEN = ["modeled-1bn-stale5-v1+swap-fin-v1", "linear-6bps-stale5-v1+swap-fin-v1",
        "modeled-1bn-terminal-adverse-v1+swap-fin-v1", "modeled-1bn-stale5-v1+flat-300-v0",
        "modeled-1bn-stale5-v1+engine-tiers-v1"]


def nets(d, scen):
    daily = NS.load_daily(d, scen)
    return NS.net_series(daily), daily


def scen_row(summary, scen):
    return next(s for s in summary["scenarios"] if s["scenario"] == scen)


def blend_stats(name):
    """Admission-definition factor Sharpe / tau of the saved blend and the WQ theme's weight (fit dir)."""
    out = {}
    fit = X / name / "fit/composition_weights.json"
    if fit.exists():
        w = json.loads(fit.read_text())
        th = w["provenance"]["themes"]
        for t in ("price_volume", "reversal_seasonality"):
            if t in th:
                out[f"w_{t}"] = th[t]["theme_weight"]
                out[f"n_{t}"] = th[t]["admitted_count"]
        out["themes"] = len(th)
        out["lin_blend_sr"] = w["provenance"]["blend_in_sample_TRAIN_diagnostic"]["factor_sharpe_annualized"]
        out["wst"] = w["provenance"]["weighted_standalone_turnover"]
        if "price_volume" in th:
            out["wq_sleeve_vol"] = th["price_volume"]["sleeve_vol_annualised"]
    return out


def main():
    args = sys.argv[1:]
    if args:
        items = [a.split(":") for a in args]
        items = [(n, X / n / d) for n, d in items]
    else:
        items = [(p.parent.name, p) for p in sorted(X.glob("*/nav*")) if p.is_dir()]
    ref_dir = X / "primary-rel/nav"
    ref_sum = NS.load_summary(ref_dir)
    recs = BI.ledger_read(ROOT / "build-equity/trials.jsonl")
    hv = BI.dsr_variance(recs, rw.WINDOW_ID)["variance_sr"]
    n_c = BI.ledger_n(recs, False)
    n_tot = DT.trial_breakdown(recs)["n_tot"] + 1
    rows = []
    for name, d in items:
        if not (d / "summary.json").exists():
            continue
        summ = NS.load_summary(d)
        rec = json.loads((d / "recipe.json").read_text())
        base = {"name": name, "nav": d.name, "aim_leverage": rec["aim_leverage"], **blend_stats(name)}
        for scen in SCEN:
            s, r = scen_row(summ, scen), scen_row(ref_sum, scen)
            a, daily = nets(d, scen)
            b, _ = nets(ref_dir, scen)
            xa, xb = NS.align(a, b)
            ps = NS.paired_stats(xa, xb, one_sided=True)
            diff = xa - xb
            cs = NS.construction_stats(daily, s)
            row = dict(base, scenario=scen, net_sharpe=s["net_sharpe"], gross_sharpe=s["gross_sharpe"],
                       ann_mean=s["ann_mean"], ann_vol=s["ann_vol"], cagr=s["cagr"], max_dd=s["max_drawdown"],
                       hac_t=s["hac_t"], tau_gmv=cs["tau_gmv_mean"], tau_p95=cs["tau_gmv_p95"],
                       gross_all_rows=cs["mean_gross_leverage_all_rows"], cost_bps_traded=cs["cost_bps_traded"],
                       cost_drag_ann=(s["gross_sharpe"] - s["net_sharpe"]),
                       ref_net_sharpe=r["net_sharpe"], d_sharpe=ps["dsr"], memmel_se=ps["memmel_se"],
                       cbb_lo=ps["cbb_ci95"][0], cbb_hi=ps["cbb_ci95"][1], lw_p=ps["lw"]["p_value"],
                       lw_p_one=ps["lw"].get("p_one_sided"), rho_ref=ps["rho"],
                       diff_mean_ann=float(diff.mean() * 252), diff_vol_ann=float(diff.std(ddof=1) * ANN),
                       diff_sharpe=NS.sharpe(diff), diff_hac_t=F.newey_west_t(diff), sessions=ps["sessions"])
            # annual trade + financing cost as return: gross ann mean - net ann mean
            daily_gross = daily["gross_return"][NS.return_mask(daily)]
            row["cost_ann_return"] = float(daily_gross.mean() * 252 - np.array(list(a.values())).mean() * 252)
            if scen == SCEN[0]:
                m = NS.net_moments(np.array(list(a.values())))
                for label, n in (("n2", 2), (f"nc{n_c}", n_c), (f"ntot{n_tot}", n_tot), (f"n{n_tot + 202}", n_tot + 202)):
                    sr0 = BI.expected_max_sr(hv, n)
                    row[f"dsr_{label}"] = BI.deflated_sharpe(m["sr_daily"], m["sessions"], m["skew"], m["kurtosis"], sr0)
                row["psr_sr0_0"] = BI.deflated_sharpe(m["sr_daily"], m["sessions"], m["skew"], m["kurtosis"], 0.0)
                cc = d / "capacity_curve.csv"
                if cc.exists():
                    for c in csv.DictReader(open(cc)):
                        row[f"cap_x{c['multiple']}"] = float(c["net_sharpe"])
            rows.append(row)
    cols = []
    for r in rows:
        for c in r:
            if c not in cols:
                cols.append(c)
    (X / "stats").mkdir(exist_ok=True)
    with open(X / "stats/nav_compare.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow({c: (f"{v:.6g}" if isinstance(v, float) else v) for c, v in r.items()})
    print(f"house DSR inputs: V {hv:.6g} per-session, N construction+1 {n_c}, N_tot+1 {n_tot}")
    print("name nav L | themes w_wq n_wq | netSR (ref) dSR se [cbb] lw_p | ann% vol% dd% tau gross cost%/y | diff: mean%/y SR t rho")
    for r in rows:
        if r["scenario"] != SCEN[0]:
            continue
        print(f"{r['name']:10s} {r['nav']:14s} {r['aim_leverage']:.4f} | {r.get('themes', '')} "
              f"{r.get('w_price_volume', float('nan')):.3f} {r.get('n_price_volume', '')} | "
              f"{r['net_sharpe']:.3f} ({r['ref_net_sharpe']:.3f}) {r['d_sharpe']:+.3f} {r['memmel_se']:.3f} "
              f"[{r['cbb_lo']:+.2f},{r['cbb_hi']:+.2f}] {(r['lw_p'] if r['lw_p'] is not None else float('nan')):.2f} | {100 * r['ann_mean']:.2f} {100 * r['ann_vol']:.2f} "
              f"{100 * r['max_dd']:.2f} {r['tau_gmv']:.4f} {r['gross_all_rows']:.4f} {100 * r['cost_ann_return']:.2f} | "
              f"{100 * r['diff_mean_ann']:+.2f} {r['diff_sharpe']:+.2f} {(r['diff_hac_t'] or 0.0):+.2f} {r['rho_ref']:.3f}")


if __name__ == "__main__":
    main()
