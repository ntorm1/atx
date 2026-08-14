# atx-vol API Restructure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restructure atx-vol so the public interface lives exclusively under `include/atx/vol/api/<module>/` and every internal header lives under `src/<module>/`, with all in-repo consumers cut over in one pass.

**Architecture:** Measurement-driven: a script builds the include graph from out-of-library roots, emits a placement table (header → api/<module> or src/<module>) and an old→new rewrite map; scripted `git mv` + one global include rewrite executes the cutover; the build system is reworked to per-module blocks with `api/`-only install; per-module passes then split private freight out of public headers. Verification is build + anchored targeted ctest only — never full suites.

**Tech Stack:** C++20, CMake, Python 3 (migration scripts), git, ctest/gtest.

**Spec:** `docs/superpowers/specs/2026-08-14-atx-vol-api-restructure-design.md`

## Global Constraints

- Pure physical restructure: no namespace changes, no signature changes, no semantic changes of any kind.
- Forbidden: touching `risk_surface_validation` semantics, `audit_fit_inversions`, any oracle tolerance, any admission gate or validation constant. If a task seems to need it, STOP and report.
- Build ONLY via `powershell -File C:\atx\scripts\atx-build.ps1 build <target>` run from `C:\atx`. The no-arg form is a silent no-op that still exits 0 — never trust it. Confirm exit 0 before believing any test result.
- Test ONLY via anchored ctest regexes (`^(Suite1|Suite2)\.`). Before trusting any regex, run `ctest -N -R "<regex>"` and require a nonzero match count (suite-name traps are real: `CurveNoarb` matches nothing; the real name is `CurveSurfaceNoArb`).
- Never run the full monorepo test suite.
- All work on branch `feat/vol-api-restructure` in `C:\atx`. Do not touch files under `atx-db/`, `atx-factor/` (they carry unrelated uncommitted changes). Commit only paths you changed.
- The rtk hook rewrites bare `git`; use `rtk proxy git ...` when output looks mangled or when generating patches.
- `git mv` for every file move (history preservation). Never delete+recreate.
- Satellite trees `atx-vol/tools/include/` and `atx-vol/research/include/` keep their current layout (out of scope).
- Include style after rewrite: `#include <atx/vol/api/<module>/<name>.hpp>` for public headers from any consumer; private headers are included relative to `src/` (the library and test targets get `atx-vol/src` as a private include dir), spelled `#include "<module>/<name>.hpp"`.

---

### Task 1: Branch, measurement script, placement table

**Files:**
- Create: `atx-vol/scripts/api_restructure_measure.py`
- Create: `tmp/api-restructure/placement.csv` (generated)
- Create: `tmp/api-restructure/rewrite_map.csv` (generated)
- Create: `atx-vol/docs/api-placement.md` (committed summary of the table)

**Interfaces:**
- Produces: `placement.csv` rows `old_path,new_path,visibility,module` for EVERY file under `atx-vol/include/` and every `.hpp` under `atx-vol/src/`; `rewrite_map.csv` rows `old_include,new_include` for every include spelling that changes. Task 2 consumes both verbatim.

- [ ] **Step 1: Create branch**

```bash
cd /c/atx && rtk proxy git checkout -b feat/vol-api-restructure
```

- [ ] **Step 2: Write the measurement script**

Create `atx-vol/scripts/api_restructure_measure.py`. Core logic (complete the MODULE dict from the spec's taxonomy — the assignments below are the authoritative starting point; every header under `include/atx/vol/` must end up with an entry or the script must fail loudly listing the unassigned):

```python
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

ROOT_DIRS = [
    REPO / "atx-options-engine",
    VOL / "python",
    VOL / "tools",
    VOL / "examples",
    VOL / "bench",
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
```

- [ ] **Step 3: Run it; sanity-check the split**

Run: `python atx-vol/scripts/api_restructure_measure.py`
Expected: prints `total=… public=… private=…`, exits 0, both CSVs exist. If it exits with `UNASSIGNED …`, add the missing stem to the dict (module per spec taxonomy) and re-run. If it exits with `PUBLIC SET > 100`, STOP per spec and report to the user.

- [ ] **Step 4: Spot-check 10 rows against ground truth**

Pick 5 expected-public (`session`, `theo`, `surface_db`, `black76`, `backtest`) and 5 expected-private (`spy_fixture`, any `detail/`, `corpus`?) rows; verify with a direct grep that each public one really is included from a root dir and each private one is not. Any mismatch = script bug; fix before proceeding.

- [ ] **Step 5: Write the committed summary**

Create `atx-vol/docs/api-placement.md`: counts per module (public/private), the full public list grouped by module, and one line explaining the measurement rule. Generate it from placement.csv (10-line python snippet or by hand from the printout).

- [ ] **Step 6: Commit**

```bash
cd /c/atx && rtk proxy git add atx-vol/scripts/api_restructure_measure.py atx-vol/docs/api-placement.md && rtk proxy git commit -m "feat(vol): api-restructure measurement — placement table and rewrite map generator"
```

---

### Task 2: Execute moves + global include rewrite + build-fix

**Files:**
- Create: `atx-vol/scripts/api_restructure_apply.py`
- Modify: every path in `placement.csv` (git mv), every includer repo-wide, `atx-vol/CMakeLists.txt` + `atx-vol/tests/CMakeLists.txt` + `atx-vol/python/CMakeLists.txt` (path fixes only — structural rework is Task 3)

**Interfaces:**
- Consumes: `tmp/api-restructure/placement.csv`, `rewrite_map.csv` (Task 1 formats).
- Produces: tree in final physical shape; every consumer compiles. Src `.cpp` files also move to `src/<module>/` (same module as their primary header; unmatched ones resolved by the explicit dict below).

- [ ] **Step 1: Write the apply script**

`atx-vol/scripts/api_restructure_apply.py`:
1. Read placement.csv; `git mv` each header (create dirs first).
2. Move each `atx-vol/src/*.cpp` and src-local `.hpp` to `src/<module>/`: module = MODULE/DETAIL_MODULE entry of its stem after stripping suffixes (`_avx2`, `_batch` keeps its own entry); explicit dict for the known non-matching TUs:
   `{"instrumentation_abi":"core","boundary_interp":"pricing","analytics_density":"analytics","snapshot_cache":"storage","track_gc":"storage","track_compact_reconcile":"storage","tearsheet":"backtest","dispersion_run":"backtest","dispersion_backtest":"backtest","dispersion_workflow":"backtest","listed_dispersion_pipeline":"backtest","listed_dispersion_reconciliation":"backtest","listed_definitions_cache":"marketdata","run_report":"storage","run_diagnostics":"storage","run_archive":"storage","backtest_driver":"backtest","corpus_board_fit":"marketdata","surface_db_seed":"storage","step_mark_memo":"backtest","laned_greek_run":"backtest","slice_payload_padding":"storage","american_boundary":"pricing","surface_db_populate":"storage","surface_db_build":"storage","surface_db_admin":"storage","convex_recovery":"fitting","risk_surface_validation":"fitting","prepared_portfolio":"backtest","prepared_fitting":"fitting","deriv_book":"backtest"}` — any stem still unmatched: fail loudly listing it; add to dict; re-run.
3. Rewrite includes repo-wide: for every `.cpp/.hpp/.h/.in` under the repo (excluding `.git`, `build*`, `atx-db`, `atx-factor`), apply rewrite_map exact-string replacements of both `<atx/vol/X>` and `"atx/vol/X"` forms; private targets rewrite to `"<module>/<name>.hpp"`.
4. Idempotent: running twice is a no-op.

- [ ] **Step 2: Run it**

Run: `python atx-vol/scripts/api_restructure_apply.py`
Expected: exit 0; `git status` shows renames (R) not delete+add pairs (spot-check `rtk proxy git status --short | head`).

- [ ] **Step 3: Patch CMake source paths (mechanical)**

In `atx-vol/CMakeLists.txt` and `atx-vol/tests/CMakeLists.txt` and `atx-vol/python/CMakeLists.txt`: update every `src/foo.cpp` reference to `src/<module>/foo.cpp` per the move map (scriptable: same dict). Add `target_include_directories(<lib> PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)` and the same PRIVATE dir on the test target so `#include "<module>/<name>.hpp"` resolves. Do NOT restructure lists yet.

- [ ] **Step 4: Build until green**

Run: `powershell -File C:\atx\scripts\atx-build.ps1 build atx-vol-tests` (from `C:\atx`)
Expected: exit 0. Iterate on stragglers (relative includes the script missed, PCH lists, generated-file templates). Every fix must be a path fix, never a code change.

- [ ] **Step 5: Targeted smoke suites**

Run: `cd C:\atx\build; ctest -j 8 --output-on-failure -R "^(SurfaceV2Provenance|SurfaceParity|VolaSession|VolCurve|QuoteFeasibility|CurveSurfaceNoArb|SimdEssviBatch|EssviTotal|SviEval)\."`
First `ctest -N` the regex; require > 100 matches. Expected: 0 failures (data-dependent skips OK).

- [ ] **Step 6: Repo-wide old-path scan**

Run: `rtk grep "atx/vol/(?!api/)" C:\atx --max 50` (or Grep tool pattern `atx/vol/(?!api/)` on `*.cpp,*.hpp,*.h,*.in` excluding docs/md). Expected: zero hits in code outside `tools/include` and `research/include` satellite trees (their own spellings `atx/vol/tools/`, `atx/vol/research/` are exempt and must be excluded from the rewrite map by construction).

- [ ] **Step 7: Commit**

```bash
cd /c/atx && rtk proxy git add -A -- atx-vol atx-options-engine cmake && rtk proxy git commit -m "refactor(vol): move headers to api/<module> public tree and src/<module> private tree

Placement measured from the out-of-library include graph (see
atx-vol/docs/api-placement.md). Pure physical restructure: git mv +
deterministic include rewrite; no code changes."
```

---

### Task 3: Build-system restructure and install interface

**Files:**
- Modify: `atx-vol/CMakeLists.txt` (per-module source blocks, install rules)
- Modify: `cmake/atx-volConfig.cmake.in`, `cmake/atx-vol-install.cmake`, `cmake/atx-vol-version.hpp.in`
- Modify: `atx-vol/test-package/smoke.cpp`, `atx-vol/test-package/CMakeLists.txt`
- Modify: `atx-vol/python/CMakeLists.txt` (only if paths remain stale)

**Interfaces:**
- Consumes: final tree shape from Task 2.
- Produces: `install(DIRECTORY include/atx/vol/api/ …)` as the only header install; per-module `set(ATX_VOL_<MODULE>_SOURCES …)` blocks composing the target; smoke package compiling against install tree with api/ includes only.

- [ ] **Step 1: Restructure source lists** — group the library target's sources into one `set(ATX_VOL_<MODULE>_SOURCES …)` block per module (core, pricing, simd, fitting, marketdata, storage, analytics, backtest), then `target_sources(<lib> PRIVATE ${ATX_VOL_CORE_SOURCES} …)`. No file added or removed — pure regrouping; verify with `cmake --build`-free reconfigure diff of the generated file list if in doubt.
- [ ] **Step 2: Install rules** — header install becomes exactly `install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/atx/vol/api/ DESTINATION include/atx/vol/api)`; adjust `atx-volConfig.cmake.in` interface include dirs; version template path updated if it emits into the old tree.
- [ ] **Step 3: Update smoke** — `test-package/smoke.cpp` includes only `<atx/vol/api/...>` headers (pick 3: `core/version.hpp`, `pricing/black76.hpp`, `fitting/session.hpp`).
- [ ] **Step 4: Verify** — reconfigure + `atx-build.ps1 build atx-vol-tests` exit 0; then the Task 2 Step 5 smoke regex again, 0 failures.
- [ ] **Step 5: Commit** — `refactor(vol): per-module build blocks; install ships api/ tree only`.

---

### Task 4: Private-element split — fitting

**Files:**
- Modify: public headers under `include/atx/vol/api/fitting/`
- Create: `atx-vol/src/fitting/<name>_detail.hpp` per split
- Modify: includers of split symbols (src + tests)

**Interfaces:**
- Consumes: final tree. Produces: fitting public headers free of `detail::` blocks and single-TU internals; all moved decls land in `src/fitting/<name>_detail.hpp` with identical content (verbatim cut/paste; includes adjusted).

- [ ] **Step 1: Enumerate candidates** — for each `api/fitting/*.hpp`: grep for `namespace detail`, and for each top-level symbol grep repo-wide use count; a symbol used by exactly one TU (its own .cpp) or inside a `detail` namespace is split freight. Record the list in the task report; skip headers with none.
- [ ] **Step 2: Split, one header at a time** — cut the freight verbatim into `src/fitting/<name>_detail.hpp` (same include guards style, minimal includes to stand alone); the moving symbols' `.cpp` and any test that used them include the new detail header via `"fitting/<name>_detail.hpp"`. Public header must not include the detail header.
- [ ] **Step 3: Build after each header** — `atx-build.ps1 build atx-vol-tests` exit 0 before the next header.
- [ ] **Step 4: Fitting suites** — `ctest -N` then run `-R "^(SurfaceParity|SurfaceParityCarry|SurfaceParityReportContract|SurfaceV2Provenance|SurfaceV2Fallback|SurfaceV2FailClosed|SurfaceV2LegacyCompat|VolCurve|CurveSurfaceNoArb|QuoteFeasibility|SviEval|EssviTotal|EssviGrad|EssviReparam|EssviPhiMax|EssviBackbone|EssviRhoBlend|EssviResidual|ParseCurveKind|VolSurfaceSlices|VolSurfaceInterp|FitPreset)\."`; 0 failures.
- [ ] **Step 5: Commit** — `refactor(vol): split private freight out of fitting public headers`.

---

### Task 5: Private-element split — pricing, core, simd

Same procedure as Task 4 over `api/pricing/`, `api/core/`, `api/simd/`.
Suites: `ctest -N` then `-R "^(SimdEssviBatch|SviQeBasisBatch|ProfileClassifier|ProfileRegistry|TickerSeedProfile)\."` plus the pricing/american/greeks suites discovered via `ctest -N -R "American|Black76|Greeks|ImpliedVol|Theo|ForwardVar|Derivatives"` (verify names first; require nonzero matches).
Commit: `refactor(vol): split private freight out of pricing/core/simd public headers`.

### Task 6: Private-element split — marketdata, storage, analytics, backtest

Same procedure over the remaining modules.
Suites: verify names via `ctest -N -R "SurfaceDb|SurfaceArchive|TrackKey|TrackStore|OpraBatch|OpraPanel|OpraHive|Backtest|Strategy|Dispersion|Var|RealizedVol|ScenarioGrid|Panel"` then run the verified set; 0 new failures (the documented `SurfaceDbPopulate` pre-existing red stays red — it is not yours to fix; anything else red = STOP).
Commit: `refactor(vol): split private freight out of marketdata/storage/analytics/backtest public headers`.

---

### Task 7: Final gate and merge

**Files:** none new (fixes only if the gate finds stragglers).

- [ ] **Step 1: Clean configure** — delete `C:\atx\build` CMake cache for atx-vol targets (or full reconfigure per `atx-build.ps1`'s convention); rebuild `atx-vol-tests` from scratch; exit 0. This defeats PCH masking.
- [ ] **Step 2: The 36-suite targeted set** — run the merge-verification regex (`^(ProfileClassifier|ProfileRegistry|TickerSeedProfile|QuoteFeasibility|SurfacePolicy|RiskSurfaceValidation|RiskSurfaceAdmission|SessionInputsContract|VolaSession|FitPreset|DeAmFitCache|Session|VolaSessionCacheBox|SimdEssviBatch|SviQeBasisBatch|SurfaceArchiveV2Adversarial|SurfaceV2Fallback|SurfaceV2FailClosed|SurfaceV2LegacyCompat|SurfaceV2Provenance|SurfaceParityReportContract|SurfaceParity|SurfaceParityCarry|VolCurve|ParseCurveKind|EssviBackbone|EssviRhoBlend|EssviResidual|EssviTotal|EssviGrad|EssviReparam|EssviPhiMax|SviEval|VolSurfaceSlices|VolSurfaceInterp|CurveSurfaceNoArb)\.`): expect the pre-restructure counts (280 pass / 5 data-dependent skips at last run), zero new failures.
- [ ] **Step 3: Install smoke** — build the install tree + `test-package` compile against it; exit 0.
- [ ] **Step 4: Diff audit** — `rtk proxy git diff main...HEAD --stat` review: only moves/renames, include rewrites, `_detail.hpp` splits, build-system files, the two scripts, `api-placement.md`. Any semantic-looking hunk = investigate before merge.
- [ ] **Step 5: Old-path scan repeat** — Task 2 Step 6 grep; zero hits.
- [ ] **Step 6: Merge to main** — `rtk proxy git checkout main && rtk proxy git merge --no-ff feat/vol-api-restructure`; rebuild once on main (`atx-build.ps1 build atx-vol-tests`, exit 0 — the stale-binary lesson); run the Step 2 regex once more; report final counts.
