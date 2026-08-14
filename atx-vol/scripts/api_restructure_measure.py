#!/usr/bin/env python3
"""Emit placement.csv + rewrite_map.csv for the api/ restructure.

Public/private is MEASURED: a header is public iff transitively included
from any root TU outside the library core (options-engine, python
bindings, tools, examples, bench, test-package). Module assignment is
DECLARED in MODULE below (spec taxonomy). Spec rule: borderline -> private.
"""
import re, sys, csv
from pathlib import Path

REPO = Path(r"C:\atx")
VOL = REPO / "atx-vol"
INC = VOL / "include" / "atx" / "vol"

# External-only roots (user decision 2026-08-14): tools/examples/bench are internal
# programs and get the private include dir like tests.
ROOT_DIRS = [
    REPO / "atx-options-engine",
    VOL / "python",
    VOL / "test-package",
]

# Header stem -> module. Top-level headers (spec taxonomy).
MODULE = {
    # core
    "types": "core", "version": "core", "log": "core", "vol_time": "core",
    "market_env": "core", "chain": "core", "listed_quote_key": "core",
    "vol": "core",  # umbrella; lands at api/vol.hpp (module dir ignored for it)
    # pricing
    "black76": "pricing", "american": "pricing", "american_iv": "pricing",
    "american_batch": "pricing", "batch": "pricing", "implied_vol": "pricing",
    "greeks": "pricing", "adjusted_greeks": "pricing", "theo": "pricing",
    "derivatives": "pricing", "swap_leg": "pricing", "dividend": "pricing",
    "rates_curve": "pricing",
    # fitting
    "session": "fitting", "pricer_fitter": "fitting", "vol_curve": "fitting",
    "vol_surface": "fitting", "surface": "fitting", "fit_policy": "fitting",
    "svi_calib": "fitting", "essvi_calib": "fitting", "c8": "fitting",
    "c8_calib": "fitting", "cstar": "fitting", "cstar_calib": "fitting",
    "calib": "fitting", "curve_fit": "fitting", "curve_selector": "fitting",
    "arb": "fitting", "parity": "fitting", "surface_parity": "fitting",
    "deamer": "fitting", "profile": "fitting", "surface_policy": "fitting",
    "dense_slice": "fitting", "spline_curve": "fitting",
    "fit_metrics": "fitting", "sr_tenor_grid": "fitting",
    "correction": "fitting", "projection": "fitting", "spy_fixture": "fitting",
    # marketdata
    "listed_opra": "marketdata", "opra_batch": "marketdata",
    "opra_hive": "marketdata", "opra_panel": "marketdata",
    "corpus": "marketdata", "occ_ess": "marketdata",
    "universe": "marketdata", "catalog": "marketdata", "data": "marketdata",
    # storage
    "surface_db": "storage", "surface_archive": "storage",
    "backtest_db": "storage", "backtest_db_build": "storage",
    "research_db": "storage", "track_key": "storage", "track_store": "storage",
    "dispersion_surface_db": "storage", "s3": "storage",
    "snapshot_pool": "storage",
    # analytics
    "analytics": "analytics", "var": "analytics", "var_report": "analytics",
    "var_validation": "analytics", "realized_vol": "analytics",
    "scenario_grid": "analytics", "historical_projection": "analytics",
    "contract_projection": "analytics", "pnl_attribution": "analytics",
    "earnings_repro": "analytics", "earnings_repro_config": "analytics",
    "earnings_forecast_loader": "analytics", "earnings_term_fit": "analytics",
    "event_vol": "analytics", "breakeven": "analytics",
    # backtest
    "backtest": "backtest", "backtest_template": "backtest",
    "strategy": "backtest", "strategy_pipeline": "backtest",
    "sweep_driver": "backtest", "dispersion": "backtest",
    "dispersion_strangle": "backtest", "listed_dispersion": "backtest",
    "listed_dispersion_schedule": "backtest",
    "listed_dispersion_strategy": "backtest", "panel": "backtest",
    "structure_panel": "backtest", "vega_panel": "backtest",
    "quant_pipeline": "backtest", "portfolio_pricer": "backtest",
    "priced_surface": "backtest", "priced_surface_view": "backtest",
    "deriv_book": "backtest", "query_pricing": "backtest",
    "margin": "backtest", "research_validation": "backtest",
}

# detail/ headers -> owning module (always private).
DETAIL_MODULE = {
    "adjoint_greeks": "pricing", "aggregate_arity": "fitting",
    "archive_util": "storage", "backtest_series_columns": "backtest",
    "calib_shared": "fitting", "convex_recovery": "fitting",
    "counters": "fitting", "deam_pass_counter": "fitting",
    "deriv_ref_bridge": "pricing", "fit_scheduler": "fitting",
    "legacy_c8_surface": "fitting", "legacy_cstar_surface": "fitting",
    "legacy_surface": "fitting", "log_emit": "core", "parallel_for": "core",
    "phase_profile": "core", "prepared_fitting": "fitting",
    "prepared_policy": "fitting", "prepared_portfolio": "backtest",
    "pricing_executor": "pricing", "quote_feasibility": "fitting",
    "resid_basis": "fitting", "risk_surface_validation": "fitting",
    "robust": "fitting", "run_archive_schema": "storage",
    "rv_lognormal": "analytics", "scalar_erfc": "pricing",
    "strip_grid": "pricing", "surface_archive_payload": "storage",
    "vector_math": "simd", "writer_lock": "storage",
}

# Always-private regardless of reachability (test fixtures, probes).
FORCE_PRIVATE = {"spy_fixture", "vector_math_probe"}

INC_RE = re.compile(r'#\s*include\s*[<"](atx/vol/[^>"]+)[>"]')

def includes_of(path: Path):
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    return INC_RE.findall(text)

def build_graph():
    """old include-spelling -> set of old include-spellings it includes"""
    graph = {}
    for h in INC.rglob("*.hpp"):
        key = h.relative_to(VOL / "include").as_posix()
        graph[key] = set(includes_of(h))
    return graph

def reachable_from_roots(graph):
    seeds = set()
    for d in ROOT_DIRS:
        if not d.exists():
            continue
        for f in list(d.rglob("*.cpp")) + list(d.rglob("*.hpp")) + list(d.rglob("*.h")):
            seeds.update(includes_of(f))
    seen, stack = set(), [s for s in seeds if s in graph]
    while stack:
        cur = stack.pop()
        if cur in seen:
            continue
        seen.add(cur)
        stack.extend(n for n in graph.get(cur, ()) if n not in seen)
    return seen

def place(rel: str, public: set):
    """rel like 'atx/vol/session.hpp' or 'atx/vol/detail/robust.hpp'
    or 'atx/vol/simd/iv_batch.hpp'. Returns (new_repo_path, visibility, module)."""
    parts = rel.split("/")
    stem = Path(parts[-1]).stem
    if parts[2] == "detail":
        mod = DETAIL_MODULE.get(stem)
        if mod is None:
            sys.exit(f"UNASSIGNED detail header: {rel}")
        return (f"atx-vol/src/{mod}/{parts[-1]}", "private", mod)
    if parts[2] == "simd":
        if stem in FORCE_PRIVATE or rel not in public:
            return (f"atx-vol/src/simd/{parts[-1]}", "private", "simd")
        return (f"atx-vol/include/atx/vol/api/simd/{parts[-1]}", "public", "simd")
    mod = MODULE.get(stem)
    if mod is None:
        sys.exit(f"UNASSIGNED header: {rel}")
    if stem == "vol":
        return ("atx-vol/include/atx/vol/api/vol.hpp", "public", "core")
    if stem in FORCE_PRIVATE or rel not in public:
        return (f"atx-vol/src/{mod}/{parts[-1]}", "private", mod)
    return (f"atx-vol/include/atx/vol/api/{mod}/{parts[-1]}", "public", mod)

def main():
    graph = build_graph()
    public = reachable_from_roots(graph)
    out = Path(r"C:\atx\tmp\api-restructure"); out.mkdir(parents=True, exist_ok=True)
    rows, rmap = [], []
    for rel in sorted(graph):
        old_repo = f"atx-vol/include/{rel}"
        new_repo, vis, mod = place(rel, public)
        rows.append((old_repo, new_repo, vis, mod))
        if vis == "public":
            new_inc = new_repo.split("include/", 1)[1]
        else:
            new_inc = new_repo.split("src/", 1)[1]
        rmap.append((rel, new_inc))
    with open(out / "placement.csv", "w", newline="") as f:
        csv.writer(f).writerows([("old_path", "new_path", "visibility", "module"), *rows])
    with open(out / "rewrite_map.csv", "w", newline="") as f:
        csv.writer(f).writerows([("old_include", "new_include"), *rmap])
    n_pub = sum(1 for r in rows if r[2] == "public")
    print(f"total={len(rows)} public={n_pub} private={len(rows)-n_pub}")
    if n_pub > 100:
        sys.exit("PUBLIC SET > 100 — spec says STOP and revisit with the user")

if __name__ == "__main__":
    main()
