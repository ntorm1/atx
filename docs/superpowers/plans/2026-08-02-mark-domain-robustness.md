# Mark-Domain Robustness (fitting → pricing → backtesting) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate silent tenor-domain extrapolation as a source of phantom one-day mean-reverting vega spikes in backtests, by making extrapolation visible at the surface API, policy-controlled in the backtest, observable in surface-db build reports, and rarer at fit time.

**Architecture:** Four independent layers, innermost out: (1) `PricedSurface`/`PricedSurfaceView` grow a tenor-domain query; (2) `run_backtest` grows a mark-domain policy (legacy Extrapolate default, opt-in CarryLastMark/Error) plus always-on extrapolated-mark counters; (3) the surface-db build report surfaces per-expiry slice-drop rows and per-date tenor coverage, and the db admin CLI gains a tenor-audit command; (4) the board-level carry fallback is fixed so long-dated expiries stop being dropped when a term-structure fallback carry is available.

**Tech Stack:** C++23 (clang-cl on Windows, Ninja, `-Werror` warnings config `atx_warnings`), GoogleTest, existing atx-vol build presets (`build/` dev dir in this worktree, configure with vcvars64).

## Background (evidence, all verified this session)

- Backtest daily PnL AC1 = −0.45; all 9 reversal-spike pairs are vega-driven. On each quiet-market spike day the SPY surface lost its long-dated pillars: e.g. 2023-10-25 last pillar T=1.40 (neighbors 2.15+), 2023-11-29 T=1.56, 2023-07-17 T=1.51, 2025-04-28 T=1.72, 2025-04-09 T=0.70 with only 12 slices. Census: 50/1887 dates have no pillar ≥ 1.95y; 19 have none ≥ 1.8y.
- `CurveSurface::w(k,T,bracket)` (atx-vol/src/vol_curve.cpp:323-347) returns the LAST slice's total variance unchanged for T past the last pillar (flat-TV extrapolation) and scaled `w_front*(T/T_front)` before the first. The header doc at atx-vol/include/atx/vol/vol_curve.hpp:337-341 claims a query past the last slice returns NaN — the doc is wrong, the code is silent.
- The backtest marks every lot by T-interpolation (never per-expiry lookup): chain `compute_step` (atx-vol/src/backtest.cpp:1261) → `PortfolioPricer::pnl_totals_with_target_marks_into` (atx-vol/src/portfolio_pricer.cpp:2328) → `solve_span` (:1728) → `PricedSurface::evaluate_batch` (atx-vol/src/priced_surface.cpp:1071) → `CurveSurface::bracket/w`. A dropped slice silently re-brackets; `n_unpriced` (backtest.cpp:1509) counts only NaN/absent, never wrong-but-finite marks.
- Fit side: expiries are dropped whole by the prepass carry gate (atx-vol/src/curve_fit.cpp:615-669, taxonomy CarryFailed/PrepStarved/PrepFailed/Skipped into `out.expiry_reports`), even when 100+ quotes pass the admission gates (verified from raw hive parquet on 2023-10-25: 114-160 LEAPS quotes pass spread/mid ≤ 0.60 and spread/vega ≤ 0.05). `expiry_reports` are consumed only by tests — the surface-db build report (tools/include/atx/vol/tools/surface_db_populate.hpp + src equivalents) prints date-level failures only.
- Repro harness exists: `spy_fit_rca` (atx-vol/examples/spy_fit_rca.cpp, target `spy_fit_rca`) now reads the hive via `--path-template "date={date}/data.parquet"` and `--preset populate`; on 2023-10-25 it shows the two longest expiries as `M0` (ExpiryBuildOutcome::Missing, n_used=0); on 2025-04-09 it shows 20/34 Missing. `atx-vol-spy-mark-continuity` (atx-vol/examples/spy_mark_continuity.cpp) probes per-date pillar coverage and marked IVs against a SurfaceDb.

## Global Constraints

- Production data is READ-ONLY: never write `C:/atx-data/opra-hive` or `C:/atx-data/surface-db/spy-*`. Writable clones live at `C:/atx-data/surface-db-r2/spy-<year>`. Scratch dbs go in the session scratchpad.
- NEVER loosen risk-admission / oracle tolerances (butterfly, calendar, no-arb gates, `max_spread_vol`, `max_spread_to_mid_pct`, admission masks). Producer-side robustness fixes are in scope; tolerance relaxation is not.
- NO full-suite test runs. Build and run only the targeted gtest binaries/filters named in each task.
- Hot-path performance must not regress: no new work inside per-quote or per-query inner loops beyond O(1) comparisons; anything heavier goes behind existing per-step or per-fit boundaries.
- Default behavior of existing runs must be preserved: new backtest policy defaults to legacy behavior; new columns append to the right of existing TSV columns.
- Build: `cmd /c "call \"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat\" >nul 2>&1 && cd /d C:\atx\.claude\worktrees\strangle-backtest && cmake --build build --target <target> -j 6"`. The `build/` dir is configured Debug with `ATX_BUILD_EXAMPLES=ON`, `ATX_BUILD_TOOLS=ON`. `-Werror` is on: unused params etc. must be handled.
- Commit style: conventional commits, `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` trailer, explicit-path `git add`.
- The worktree carries ANOTHER session's uncommitted WIP (atx-vol var.* / contract_projection.* files). Never stage or revert those files.

---

### Task 1: Tenor-domain query on the priced surface + honest extrapolation docs

**Files:**
- Modify: `atx-vol/include/atx/vol/priced_surface.hpp` (near the Introspection block, ~:404-435)
- Modify: `atx-vol/src/priced_surface.cpp`
- Modify: `atx-vol/include/atx/vol/priced_surface_view.hpp` (~:136 introspection area)
- Modify: `atx-vol/src/priced_surface_view.cpp`
- Modify: `atx-vol/include/atx/vol/vol_curve.hpp:330-345` (fix the false NaN claim)
- Test: `atx-vol/tests/priced_surface_test.cpp` (append; follow the file's existing surface-construction helpers)

**Interfaces:**
- Produces (exact):
  ```cpp
  // priced_surface.hpp — public, in the Introspection section
  struct TenorDomain {
    double min_T{0.0};   // first fitted pillar (0 when empty)
    double max_T{0.0};   // last fitted pillar  (0 when empty)
    [[nodiscard]] bool empty() const noexcept { return !(max_T > 0.0); }
    // A maturity the surface can serve without tenor extrapolation.
    [[nodiscard]] bool contains(double T) const noexcept {
      return !empty() && T >= min_T && T <= max_T;
    }
  };
  [[nodiscard]] TenorDomain tenor_domain() const noexcept; // from ctx_ front/back
  // True when a query at T would be served by the flat-TV long-end branch or
  // the scaled short-end branch of CurveSurface::w (vol_curve.cpp:323-347),
  // i.e. T falls outside [min_T, max_T] or the surface is empty.
  [[nodiscard]] bool extrapolates_tenor(double T) const noexcept;
  ```
  Same two members on `PricedSurfaceView` (from `col_T_[0]` / `col_T_[n_slices_-1]`; empty view → empty domain, `extrapolates_tenor` returns true).
- Consumed by: Task 2 (`PortfolioPricer` counts extrapolated marks), Task 3 (tenor audit may use it).

**Steps:**
- [ ] **Step 1: Write failing tests** in `priced_surface_test.cpp`: build a 2-slice surface with the file's existing fixture (pillars T1 < T2); assert `tenor_domain().min_T==T1`, `.max_T==T2`, `contains(T1)`, `contains((T1+T2)/2)`, `contains(T2)`, `!contains(T2*1.5)`, `!contains(T1*0.5)`; `extrapolates_tenor(T2*1.5)==true`, `extrapolates_tenor((T1+T2)/2)==false`; empty/moved-from surface → `tenor_domain().empty()`, `extrapolates_tenor(anything)==true`. Mirror the same assertions for `PricedSurfaceView` using its existing archive-backed fixture.
- [ ] **Step 2: Run** `ctest`-free: build target `atx-vol-tests` (or the file's own binary per CMake) and run with `--gtest_filter=*TenorDomain*`; expect compile failure (methods missing).
- [ ] **Step 3: Implement** the two methods on both classes (impl in the .cpp files; `ctx_` for `PricedSurface`, `col_T_`/`n_slices_` for the view). No allocation, noexcept, O(1).
- [ ] **Step 4: Fix the doc** at `vol_curve.hpp:337-341`: replace the "returns NaN past the last slice" claim with the actual contract (short end: `w_front * T/T_front`; long end: flat total variance of the last slice; both silent) and cross-reference `PricedSurface::extrapolates_tenor`.
- [ ] **Step 5: Run tests** — `--gtest_filter=*TenorDomain*` green, plus the file's pre-existing filter (e.g. `--gtest_filter=PricedSurface*`) to prove no regression.
- [ ] **Step 6: Commit** `feat(vol): tenor-domain introspection on priced surfaces; honest extrapolation docs`.

### Task 2: Backtest mark-domain policy + extrapolated-mark accounting

**Files:**
- Modify: `atx-vol/include/atx/vol/backtest.hpp` (RunConfig ~:714-825; BacktestStep/BacktestResult column structs; validate())
- Modify: `atx-vol/src/backtest.cpp` (`compute_step` :1261, `RetainedBookPricer` :132, unpriced handling :1495-1560, row push :2590-2625, book-greeks/hedge/NAV sites :3040-3270)
- Modify: `atx-vol/src/portfolio_pricer.cpp` + `atx-vol/include/atx/vol/portfolio_pricer.hpp` (surface the per-step count of lots whose base or target query extrapolated; `solve_span` :1728 already touches both surfaces per unique contract)
- Modify: `tools/include/atx/vol/tools/tearsheet.hpp` (both TSV emitters: new columns appended)
- Test: `atx-vol/tests/backtest_test.cpp` (or the dedicated suite the file layout uses — find the existing synthetic-snapshot backtest fixture and extend it)

**Interfaces:**
- Produces (exact):
  ```cpp
  // backtest.hpp
  enum class MarkDomainPolicy : std::uint8_t {
    Extrapolate,    // legacy: silent flat-TV tenor extrapolation (default)
    CarryLastMark,  // hold the lot's last in-domain mark & greeks; catch up on return
    Error,          // abort the run with date/uid/K/T/domain detail
  };
  // RunConfig:
  MarkDomainPolicy mark_domain{MarkDomainPolicy::Extrapolate};
  ```
  - New per-row counters (always populated, all policies): `n_extrapolated_marks` — lots whose base-or-target tenor query fell outside `tenor_domain()` that step; under CarryLastMark additionally `n_carried_marks`. Both appended as rightmost TSV columns in both emitters and covered by `BacktestResult::validate()` shape checks.
- Consumes: Task 1 `extrapolates_tenor` / `tenor_domain`.

**Semantics (CarryLastMark, exact):**
1. Maintain per-lot `last_good` = {mark price, greeks} keyed by lot id, updated every step the lot's base surface serves its T in-domain.
2. On a step where the shifted (target) surface extrapolates at the lot's residual T: the lot's target mark := carried `last_good.price`; its per-step PnL contribution is 0 across ALL components; greeks contributions to `gross_*` and the hedge come from `last_good.greeks`; `n_carried_marks` increments.
3. On the first later step where the surface serves the lot in-domain again: price normally. The base-side mark for the catch-up day must also be the carried mark (NOT a fresh query against the previous day's truncated surface), so the full gap books that day; attribution components use that day's observed `dvol`/`dS`/`dt`, residual lands in `pnl_unexplained` (document this in the RunConfig comment).
4. NAV reconciliation, liquidation marks, and `book_greeks` must all see the same carried values (no divergent mark sources; `reconcile_nav` stays on).
5. A lot that is BORN into an extrapolated domain (entry day) is not carried — entry is refused for that step under CarryLastMark (strategy sees no fill; count it in `n_carried_marks`? No: refuse + count under a dedicated `n_refused_entries` only if an existing counter fits; otherwise document that entries are only taken in-domain and assert in tests).
6. Roll/close of a carried lot books at the carried mark.
- Error policy: abort message includes step date, uid, K, T, and `tenor_domain().max_T`, following the existing unpriced-abort message pattern (backtest.cpp:1546-1556).

**Steps:**
- [ ] **Step 1: Locate the synthetic backtest fixture** in the test file (snapshot-building helpers used by existing `run_backtest` tests). Write failing tests:
  - `MarkDomain.ExtrapolateCountsButMatchesLegacy`: 5-step corpus, middle date's surface truncated (drop the long slice when building that snapshot); policy Extrapolate → per-row `n_extrapolated_marks` is {0,0,>0,0,0} and every PnL column is bit-identical to a control run built from the same snapshots with the counter columns ignored.
  - `MarkDomain.CarryHoldsAndCatchesUp`: same corpus, CarryLastMark → on the truncated step every pnl component is 0 for the carried book, `n_carried_marks>0`, NAV flat for the option book; cumulative `pnl_total` over the whole window equals the Extrapolate... **No** — equals the sum of true marks: assert final NAV equals the control run's final NAV computed on the SAME corpus where the truncated date is simply absent (build a 4-step control clock skipping that date). Assert `reconcile_nav` never trips.
  - `MarkDomain.ErrorAborts`: policy Error → run returns the error, message contains the date and max_T.
- [ ] **Step 2: Run the new filter** — expect failures/compile errors.
- [ ] **Step 3: Implement** counters first (PortfolioPricer surfaces per-step extrapolated-lot count via its existing totals struct; backtest plumbs to rows/TSV/validate). Keep the Extrapolate path byte-identical elsewhere.
- [ ] **Step 4: Implement CarryLastMark + Error** per semantics above, inside `compute_step`/`RetainedBookPricer` prepare: partition alive lots into servable vs carried BEFORE building the pricer batch, so the pricer never sees carried lots that step.
- [ ] **Step 5: Run** the `MarkDomain.*` filter plus the file's existing backtest filters (the suite the fixture belongs to) — green, no regression.
- [ ] **Step 6: Commit** `feat(vol): mark-domain policy (extrapolate/carry/error) + extrapolated-mark accounting in backtests`.

### Task 3: Slice-drop rows + tenor coverage in build reports; db tenor-audit command

**Files:**
- Modify: `atx-vol/src/surface_db_populate.cpp` (+ its header `tools/include/atx/vol/tools/surface_db_populate.hpp`) — plumb per-expiry outcomes from the fit into the populate result. The fit-side data already exists: `SurfaceBuildReport::attempts[].expiries` (ExpiryBuildReport: outcome/n_used, printed by spy_fit_rca.cpp:240-248) and/or `CurveSurfaceReport::expiry_reports` (curve_fit.hpp:103). Investigate which one the populate call path can reach without holding extra state; prefer the fitter's `last_attempt_report()`.
- Modify: the build-report writer (the CSV emitted via `--report`, format seen in `C:/atx-data/logs/spy-backfill/build_*.csv`: `key,value` header sections then `date,symbol,code,detail` failure rows).
- Modify: `atx-vol/tools/surface_db_main.cpp` + `tools/include/atx/vol/tools/surface_db_admin.hpp` — new `tenor-audit` subcommand.
- Test: `atx-vol/tests/surface_db_build_test.cpp` (or the closest existing tools test) for report rows; admin test alongside existing surface-db admin tests.

**Interfaces:**
- Report additions (exact formats):
  - New section after the failure rows: header line `slice_drop.date,symbol,T,outcome,n_used` followed by one row per non-Fitted expiry of every WRITTEN date (outcome spelled as the enum name: `Missing`/`CarryFailed`/`PrepStarved`/`PrepFailed`/`Skipped`/`FitFailed`). Dates with a fully fitted board emit nothing.
  - New per-date summary keys in the key,value section: `coverage.dates_with_slice_drops,<n>` and `coverage.max_T_min,<smallest per-date last-pillar T across written dates>`.
- `tenor-audit` (exact CLI): `atx-vol-surface-db tenor-audit --db <root> [--symbol SPY]` → TSV to stdout: `date\tn_slices\tmin_T\tmax_T\tflag` where `flag` is `TRUNCATED` when `max_T < rolling_median(max_T of the 5 nearest neighbors) - 0.25` else empty. Exit 0 always (audit, not gate); `--fail-on-truncated` optional flag → exit 3 when any TRUNCATED row exists.
- Consumes: `SurfaceDb::partitions()/load_surface()` and Task 1's `tenor_domain()`.

**Steps:**
- [ ] **Step 1: Failing test for the report**: use the existing surface-db build test fixture (synthetic hive/panel) and force one expiry to drop (thin its quotes below `min_obs_per_slice` so prep starves — do NOT touch tolerances); assert the report file contains a `slice_drop.` row for that date with outcome `PrepStarved`/`Missing` and the `coverage.` keys.
- [ ] **Step 2: Failing test for tenor-audit**: build a 3-partition fixture db where the middle date lacks the long slice; run the admin entry function directly (the tools expose testable entry points — follow the existing admin tests' pattern); assert the middle row carries `TRUNCATED` and `--fail-on-truncated` exits 3.
- [ ] **Step 3: Implement report plumbing** (fitter report → populate result → CSV writer). Bound the cost: only non-Fitted expiries emit rows.
- [ ] **Step 4: Implement tenor-audit** in the admin CLI.
- [ ] **Step 5: Run** both new test filters + the existing surface-db build/admin filters touched.
- [ ] **Step 6: Commit** `feat(vol): slice-drop + tenor-coverage observability in surface-db build report; tenor-audit admin command`.

### Task 4: Stop dropping long-dated expiries the carry fallback can serve

**Files:**
- Investigate/Modify: `atx-vol/src/curve_fit.cpp` (Phase 1.5 board-level fallback :541-613; prepass carry gating in `run_deam_prepass` and `prepare_fit_slice_into_slot`), `atx-vol/src/carry.cpp` / `atx-vol/include/atx/vol/carry.hpp` (`term_structure_fallback_borrow`, `CarryAnchor`, confidence gate `needs_carry_repair` producer), possibly `atx-vol/src/corpus_board_fit.cpp`.
- Test: `atx-vol/tests/curve_fit_carry_fallback_test.cpp` (extend; existing patterns at :184-283 assert per-expiry `carry_source` / outcome).

**Repro (start here):**
`build\bin\spy_fit_rca.exe --opra-root C:/atx-data/opra-hive --date 2023-10-25 --symbols SPY --index-symbol SPY --preset populate --r 0.0545 --path-template "date={date}/data.parquet"` → the last two expiries (2025-12-19 T=2.15, 2026-01-16 T=2.23) print `M0` while 114-160 of their quotes pass both admission gates (verified externally). 2025-04-09 (`--r 0.0433`) shows 20/34 `M0` including mid-tenors. Also reproduce via the scratch-db build CLI if a full populate context matters.

**The question:** Decision B (curve_fit.cpp:541-613) exists precisely to rescue carry-deferred expiries from confident anchors — `TermStructureExtrap` is a legal `carry_source` (curve_fit_carry_fallback_test.cpp:220). Why does it not fire (or fire and still starve) for these boards? Candidate hypotheses to test, in order: (a) the expiries never set `needs_carry_repair` (they fail some earlier gate and are marked unusable outright); (b) anchors exist but `term_structure_fallback_borrow` extrapolation is clamped/refused beyond the last confident anchor; (c) fallback fires but de-Am/prep then starves the slice (check `n_used` after repair); (d) the drop happens in a different layer (corpus_board_fit / session) before `fit_curve_surface` sees the chains.

**Deliverable:** root cause written into the task report + the MINIMAL producer-side fix that lets a carry-deferred long-dated expiry with admissible quotes fit via fallback carry, with:
- risk-admission tolerances untouched (Global Constraints),
- fallback carry never laundered as Solved/confident (preserve the existing taxonomy comments at curve_fit.cpp:593-596),
- hot path unchanged for boards that need no repair (guard everything behind the existing `needs_carry_repair` branch),
- a new gtest in `curve_fit_carry_fallback_test.cpp` that reconstructs the failing shape synthetically (long-dated expiry, admissible quotes, carry solve unconfident, confident shorter anchors) and asserts the expiry FITS with `carry_source == TermStructureExtrap` (this test must FAIL before the fix),
- acceptance on real data: after the fix, the RCA run on 2023-10-25 fits a slice with T ≥ 2.1, and 2025-04-09 fits ≥ 25 of 34 expiries; fit wall-time on one populate date within 10% of before (spy_fit_rca prints nothing about time — time the scratch-db build CLI run before/after).

**Steps:**
- [ ] **Step 1: Reproduce + instrument** (temporary prints or debugger are fine; remove before commit). Identify which hypothesis (a)-(d) holds. Write the finding down first.
- [ ] **Step 2: Write the failing synthetic test** capturing the mechanism.
- [ ] **Step 3: Minimal fix** behind the repair branch.
- [ ] **Step 4: Run** `--gtest_filter=*CarryFallback*` + the file's full filter + `--gtest_filter=*CurveFit*` targeted suites; RCA acceptance runs on 2023-10-25 and 2025-04-09; timing check.
- [ ] **Step 5: Commit** `fix(vol): serve long-dated expiries via term-structure carry fallback instead of dropping them`.

### Task 5 (orchestrator-owned, not a subagent task): data repair + end-to-end validation

Refit the 50 truncated dates into the r2 clones with the fixed fitter (repair_r2.py pattern, one lane per year root), re-run the census probe (expect ~0 truncated), re-run the strangle backtest under Extrapolate (counter now reports residual exposure) and CarryLastMark, compare spike statistics (AC1, |z|≥3 reversal pairs), update the artifact and final report.
