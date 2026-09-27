"""Compact one-line-per-scenario summary of a NAV replay output dir (root helper)."""
import json, sys

d = sys.argv[1]
s = json.load(open(f"{d}/summary.json"))
print(f"rule={s.get('rule')} status={s.get('status')} primary={s.get('primary_scenario')}")
for v in s["scenarios"]:
    yrs = v.get("calendar_year_returns") or []
    ys = " ".join(
        f"{y.get('year')}:{(y.get("net_compounded_return") or 0):+.3f}" if isinstance(y, dict) else str(y)
        for y in yrs
    )
    t = v.get("daily_turnover_gmv") or {}
    c = v.get("costs", {})
    fin = v.get("financing") or {}
    print(
        f"{v['scenario'][:34]:34s} net {v['net_sharpe']:+.3f} gross {v['gross_sharpe']:+.3f} "
        f"hac {v['hac_t']:+.2f} mu {v['ann_mean']:+.4f} vol {v['ann_vol']:.4f} mdd {v['max_drawdown']:.3f} "
        f"tau_gmv mean {t.get('mean', float('nan')):.4f} p95 {t.get('p95', float('nan')):.4f} "
        f"tc {c.get('summed_trade_cost_return', 0):.4f} brw {c.get('summed_borrow_return', 0):.4f} "
        f"meets_d {v.get('meets_daily_turnover_mean')}/{v.get('meets_daily_turnover_p95')} | {ys}"
    )
    if fin:
        print("   financing:", json.dumps(fin)[:400])
    nz = v.get("neutralize") or v.get("neutralization")
    if nz:
        print("   neutralize:", json.dumps(nz)[:300])
