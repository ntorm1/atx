### Task T28: Construction audit of v4 NAV outputs (explorer, read-only)

**Lane:** EXP · **Model:** Opus 5.5 · **Depends on:** — · **Pool:** none (reads pool-2) · **Peak:** 0.3 GiB

**Files:**
- Create: `.superpowers/sdd/mega-alpha-20260926/construction-audit-v4.md`
- Create: `.superpowers/sdd/mega-alpha-20260926/studies/construction_audit.py`
- Read only: `build-equity/mega-nav-v4-*/daily_modeled-1bn-stale5-v1+swap-fin-v1.csv`, `recipe.json`, `atx-impl/src/strategy_target_replay.cpp:133-195`

**Interfaces:** Produces the table in §0 independently re-derived (gross/net leverage, held, banded share, entry rate), plus for v3
(`build-equity/mega-nav-v3-*` if present) the same columns, so the disclosure covers both validation trials.

- [ ] **Step 1:** Write `studies/construction_audit.py`:

```python
"""Construction audit: gross/net leverage, banded share and entry rate per NAV run (read-only)."""
import csv, glob, json, statistics as st, sys
SCEN = "daily_modeled-1bn-stale5-v1+swap-fin-v1.csv"
def audit(run: str) -> dict:
    rows = list(csv.DictReader(open(f"{run}/{SCEN}")))
    reb = [r for r in rows if r["rebalance"] in ("1", "true", "True")]
    col = lambda rs, k: [float(r[k]) for r in rs if r.get(k, "") not in ("", "nan")]
    recipe = json.load(open(f"{run}/recipe.json"))
    members = col(reb, "neutralize_used")  # used names ~ members (excluded share < 2%)
    banded = col(reb, "banded_names")
    return {"run": run, "rule": recipe["rule"], "band_multiple": recipe.get("band_multiple"),
            "trade_fraction": recipe.get("trade_fraction"),
            "gross_mean": st.mean(col(rows, "gross_leverage")), "net_mean": st.mean(col(rows, "net_leverage")),
            "held_mean": st.mean(col(rows, "held_names")),
            "banded_share": st.mean(b / m for b, m in zip(banded, members) if m > 0),
            "tau_gmv_mean": st.mean(col(rows, "one_way_turnover_gmv"))}
if __name__ == "__main__":
    out = [audit(r) for r in sorted(glob.glob("build-equity/mega-nav-v[34]*")) if glob.glob(f"{r}/{SCEN}")]
    json.dump(out, open(sys.argv[1], "w"), indent=1)
    for o in out: print(f"{o['run']:48s} gross {o['gross_mean']:.3f} net {o['net_mean']:+.3f} held {o['held_mean']:.0f} banded {o['banded_share']:.3f} tau {o['tau_gmv_mean']:.4f}")
```

- [ ] **Step 2:** Run it from `C:/atx-wt/pool-2` with `"C:/Program Files/Python312/python.exe" .superpowers/sdd/mega-alpha-20260926/studies/construction_audit.py .superpowers/sdd/mega-alpha-20260926/construction-audit-v4.json` (reads only; no bounded runner needed, < 5 s).
- [ ] **Step 3:** Write `construction-audit-v4.md`: the table, the mechanism (quote `strategy_target_replay.cpp:161-166` and `:175-184`), the desired-weight scale derivation (|w| ≤ 2/N, mean 1/N), and the sentence "validation trial #2 was a 24%-gross book".
- [ ] **Step 4:** Commit (`git add -f` the sprint-dir files): `docs(mega-alpha): construction audit of v4 NAV runs (T28)`.

**Acceptance:** numbers within ±0.01 of §0's table for the five v4 runs; v3 rows present if the v3 NAV dirs exist (else "absent" noted).

---

