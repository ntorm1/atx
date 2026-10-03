"""The two data readers of a research wave, run by research_wave.py under the bounded runner (root only).

  wave_readers.py mechanics --nav NAME=DIR [--nav NAME=DIR ...] --output FILE
  wave_readers.py book      --nav NAME=DIR [--nav NAME=DIR ...] --output FILE

mechanics  what the integrator's scratch mech.py printed, as code: for each NAV dir, its primary scenario's S2 daily
           CSV loaded through nav_summ.load_daily (seal-checked: a session at or after the research seal refuses the
           file) and nav_summ.construction_stats; ONLY construction keys are written: all-rows / post-ramp gross and
           net leverage, tau mean / p95 / sessions, row counts, max gross, max |net|, gross by calendar year, the
           summary's turnover flags, aim leverage and accounting checks. No return, NAV, Sharpe or P&L figure is
           read into the output. This is what gross matching and the mechanics gate read, before any return.
book       after the mechanics passed (the judge stage): the book line of the scoreboard for each NAV dir from its
           summary.json primary scenario (net / gross Sharpe, net annual mean, CAGR, volatility, max drawdown), gross
           of cost = net annual mean + the annualised |trade cost|, |borrow| and |long financing| returns (252 x summed
           return / observations, the integrator's cellstats reading), construction_stats' turnover and cost per
           traded dollar, and the net Sharpe of the 4x book from capacity_curve.csv (null without the file).

The output file is new (never overwritten): {schema, kind, navs: {NAME: {dir, scenario, daily_csv_sha256, ...}}}.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent / "atx-impl" / "tools"
SCHEMA = "atx.wave-reader/v1"
ANNUAL = 252
MECHANICS_KEYS = ("mean_gross_leverage_all_rows", "mean_net_leverage_all_rows", "mean_gross_leverage_post_ramp",
                  "mean_net_leverage_post_ramp", "post_ramp_rows", "tau_gmv_mean", "tau_gmv_p95", "tau_gmv_sessions",
                  "csv_rows", "return_rows")
CAPACITY_MULTIPLE = 4.0


def nav_summ():
    """atx-impl/tools/nav_summ.py (the existing summariser), imported as the nav_summ shim imports the moved tools."""
    if str(TOOLS) not in sys.path:
        sys.path.insert(0, str(TOOLS))
    import nav_summ as NS  # noqa: PLC0415
    return NS


def _primary(d: Path):
    NS = nav_summ()
    summary = NS.load_summary(d)
    scen = NS.scenario_of(summary)
    daily = NS.load_daily(d, scen["scenario"])          # refuses a session at or after the seal (SystemExit)
    csv_path = d / f"daily_{scen['scenario']}.csv"
    return NS, summary, scen, daily, hashlib.sha256(csv_path.read_bytes()).hexdigest()


def mechanics(d: Path) -> dict:
    NS, _, scen, daily, sha = _primary(d)
    stats = NS.construction_stats(daily, scen)
    gross, net = daily["gross_leverage"], daily["net_leverage"]
    years: dict = {}
    for ns, g in zip(daily["session_ns"].tolist(), gross.tolist()):
        years.setdefault(str(NS.BI.session_date(int(ns)).year), []).append(g)
    acc = scen.get("accounting_checks") or {}
    v5 = (scen.get("construction") or {}).get("v5") or {}
    return {"dir": d.as_posix(), "scenario": scen["scenario"], "daily_csv_sha256": sha,
            **{k: stats.get(k) for k in MECHANICS_KEYS},
            "max_gross_leverage": float(gross.max()) if gross.size else None,
            "max_abs_net_leverage": float(abs(net).max()) if net.size else None,
            "gross_by_year": {y: sum(v) / len(v) for y, v in sorted(years.items())},
            "meets_daily_turnover_mean": scen.get("meets_daily_turnover_mean"),
            "meets_daily_turnover_p95": scen.get("meets_daily_turnover_p95"),
            "aim_leverage": v5.get("aim_leverage"),
            "accounting": {k: acc.get(k) for k in ("max_return_identity_error", "max_cash_book_relative_error",
                                                   "tolerance")}}


def capacity_x4(d: Path, scenario: str) -> float | None:
    """Net Sharpe of the 4x book in capacity_curve.csv (the row of multiple 4, the primary book's when several)."""
    p = d / "capacity_curve.csv"
    if not p.is_file():
        return None
    with p.open(newline="", encoding="utf-8") as f:
        rows = [r for r in csv.DictReader(f) if _float(r.get("multiple")) == CAPACITY_MULTIPLE]
    if len(rows) > 1:
        rows = [r for r in rows if r.get("book") == scenario] or rows[:0]
    return _float(rows[0].get("net_sharpe")) if len(rows) == 1 else None


def _float(x) -> float | None:
    try:
        v = float(x)
    except (TypeError, ValueError):
        return None
    return v if v == v and v not in (float("inf"), float("-inf")) else None


def book(d: Path) -> dict:
    NS, _, scen, daily, sha = _primary(d)
    stats = NS.construction_stats(daily, scen)
    obs = scen.get("observations") or 0
    costs, fin = scen.get("costs") or {}, scen.get("financing") or {}
    parts = {"trade_cost": costs.get("summed_trade_cost_return"), "borrow": costs.get("summed_borrow_return"),
             "long_financing": fin.get("summed_long_financing_return")}
    annual = {k: abs(v) * ANNUAL / obs if isinstance(v, (int, float)) and obs else None for k, v in parts.items()}
    net = scen.get("ann_mean")
    gross = net + sum(v for v in annual.values() if v is not None) if isinstance(net, (int, float)) else None
    g, tau = stats.get("mean_gross_leverage_all_rows"), stats.get("tau_gmv_mean")
    return {"dir": d.as_posix(), "scenario": scen["scenario"], "daily_csv_sha256": sha,
            "net_sharpe": scen.get("net_sharpe"), "gross_sharpe": scen.get("gross_sharpe"),
            "net_annual": net, "gross_annual": gross, "annual_costs": annual, "cagr": scen.get("cagr"),
            "ann_vol": scen.get("ann_vol"), "max_drawdown": scen.get("max_drawdown"),
            "tau_gmv_mean": tau, "mean_gross_leverage_all_rows": g,
            "tau_per_gross": tau / g if isinstance(tau, float) and isinstance(g, float) and g > 0 else None,
            "cost_bps_traded": stats.get("cost_bps_traded"), "x4_net_sharpe": capacity_x4(d, scen["scenario"])}


READERS = {"mechanics": mechanics, "book": book}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0], epilog=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kind", choices=tuple(READERS))
    ap.add_argument("--nav", action="append", required=True, metavar="NAME=DIR")
    ap.add_argument("--output", required=True, type=Path)
    a = ap.parse_args(argv)
    navs = {}
    for item in a.nav:
        name, _, d = item.partition("=")
        if not name or not d or name in navs:
            ap.error(f"--nav {item!r}: NAME=DIR with distinct names")
        navs[name] = READERS[a.kind](Path(d))
    doc = {"schema": SCHEMA, "kind": a.kind, "navs": navs}
    a.output.parent.mkdir(parents=True, exist_ok=True)
    with a.output.open("x", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(doc, indent=2, sort_keys=True) + "\n")
    print(f"wave_readers {a.kind}: {', '.join(navs)} -> {a.output}")   # names only: no value is printed
    return 0


if __name__ == "__main__":
    sys.exit(main())
