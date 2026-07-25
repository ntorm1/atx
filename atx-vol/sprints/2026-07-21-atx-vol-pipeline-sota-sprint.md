# atx-vol Pipeline SOTA Sprint — 2026-07-21 (post 7-lane deep review)

> **For agentic workers:** executed by **parallel implementation subagents, one per
> workstream, each in its own git worktree** (§8 dispatch protocol). REQUIRED
> SUB-SKILL per subagent: `superpowers:executing-plans` (task-by-task, TDD: failing
> test first → verify fail → minimal fix → verify pass → commit). Every subagent
> re-reads §0 (sequencing mandate), §3 (global constraints), §5 (ownership /
> disjointness), §8 (dispatch protocol + traps), and §10 (do-not-relitigate dead
> ends) before touching a file. **Line numbers below are from the 2026-07-21 review
> @ main `0e480a8` and WILL drift — re-grep the cited symbol before editing.**

**Goal:** Close the correctness, product-completeness, and throughput gaps that the
2026-07-21 seven-lane deep review found across the whole pipeline
(fitting → serialization → pricing → greeks → analytics → backtesting → Python), so
atx-vol is credibly the fastest and most feature-complete American-equity-options
analytics library (Vola Dynamics class) rather than a fast engine with silent
wrong-number cliffs and unreachable features.

**Architecture:** One serial keystone (WS-M: land the stranded `feat/disp-hotpath`
57-commit reconciliation into main and re-pin goldens) unblocks eight parallel
workstreams with disjoint file ownership. Correctness clusters land before feature
and perf work in each lane. Every fix ships with a test that fails without it.

**Evidence base (read your lane's report in full before starting):**
`atx-vol/docs/reviews/2026-07-21-pipeline-sota-review/`
— `review-fitting.md` (findings FT-C1…C9), `review-pricing-iv.md` (PR-C1…C6),
`review-greeks-risk.md` (GR-P1-1…P3), `review-serialization.md` (SE-P1-1…P3),
`review-analytics.md` (AN-P1-1…P3-15), `review-backtest.md` (BT-P1-1…P2-10),
`review-python.md` (PY-1…12).

**Base:** `main @ 0e480a8`. Stranded branch: `feat/disp-hotpath @ 30809a0`
(complete, gated, byte-identical to its own base `8cb4576`; see
`sprints/2026-07-19-spy-dispersion-hotpath-sprint-STATUS.md` FINISH REPORT).

---

## 0. Mandate & sequencing (read first)

1. **WS-M is the keystone and goes first, alone.** Three of the seven reviews
   independently found their top risk stranded on `feat/disp-hotpath`: the
   friction/impact + risk-limit execution layer (BT-P1-1), byte-reproducible
   corpus stamps + zero-copy Sealed backing (SE scope note), and the
   regime-first reporting contract (PY-4). No other workstream forks until WS-M
   lands and the post-merge serial gate is green, because WS-M touches ~48 engine
   files and would conflict with everything.
2. **Correctness before features before perf, per lane.** Each workstream's tasks
   are ordered; do not reorder to chase a perf win past an open P1.
3. **Every fix lands with a test that fails without it.** A vacuous test
   (see STATUS §5.1 — "the check never had a way to observe the difference") is a
   sprint defect. Prove the failing state first.
4. **Count-then-cut carries forward** for perf tasks: gate on the deterministic
   solve ledger (`tests/solve_ledger_test.cpp`, `counters.hpp`) where possible,
   quiet-window benches (`rel-avx2`, P-core pin, best-of-N, CV ≤ 5%) for timing.
5. **No speculative optimization.** Every perf task cites review evidence
   (file:line). If the evidence turns out wrong on re-grep, STOP and report; do
   not pivot to a different optimization in the same lane.

## 1. Scoreboard & exit criteria

| # | Exit criterion | Verification |
|---|---|---|
| 1 | `feat/disp-hotpath` reconciled into main; new golden replay SHAs pinned; serial gate ≤ known-failure set (target 0 after M3) | WS-M gate log + updated STATUS doc |
| 2 | Zero known silent-wrong-number paths: PR-C1 (cache box), PR-C3 (bulk), FT-C1 (NM clamp), FT-C2/C7 (butterfly), FT-C3 (Lee), GR-P2-1 (NaN gate), SE-P1-1/P1-3 (view OOB), AN-P1-1 (vega units), BT-P1-3 (spot-0.0 hedge) each closed by a named test | grep test names in §4 gates; all pass serially |
| 3 | Archive round-trip contract true: `n_quad_price` persisted (SE-P1-2), salt bumped, migration note written | `SurfaceArchiveV2.RoundTripsNQuadPrice` passes |
| 4 | Backtest artifacts carry a mandatory friction regime end-to-end (engine → TSV → Python report) | `test_report.py` regime-refusal test + C++ emitter test |
| 5 | Canonical risk frame exposes bucketed vega (per-underlier, per-expiry) + dP/dq column | `portfolio_pricer_test` bucket tests |
| 6 | Python can fit a surface from quotes and batch-price American with NaN+status semantics | `test_atxvol.py` fit-lifecycle + batch tests |
| 7 | Fitting bench re-pointed at `PricerFitter::fit`; corpus fan-out worker utilization ≥ 14/16 on the 11-name golden corpus (from ~9/16) or a written explanation | quiet-window bench row + phase-split log |
| 8 | No stale contract docs: README SIMD paragraph, `american_boundary_batch.cpp` Auto comment, `created_ts_ns` semantics, `atxvsa2-format.md` | doc-drift checklist in WS-M5/A6/C5 all checked |

## 2. Findings ledger (sprint-scoped index)

Task IDs below cite these. Full detail + failure scenarios live in the review files.

| Ledger ID | One-liner | Where |
|---|---|---|
| FT-C1 | SVI Nelder–Mead returns box-inconsistent vertex; b flips sign/blows up; laundered to flat slice served Ok | `svi_calib.cpp:316-434,884-892` |
| FT-C2 | SVI butterfly gate = necessary conditions only; density backstop scans only \|k\|≤0.6 | `arb.cpp:458-514`, `vol_curve.cpp:23-37` |
| FT-C3 | Two wrong "Lee bound" conventions (eSSVI over-tight T>1; SVI-MM vacuous short-T) | `essvi_calib.cpp:362-392`, `svi_calib.cpp:470-480` |
| FT-C5 | `svi_jw_to_raw` symmetric branch accepts σ=0 | `svi_calib.cpp:1262-1271` |
| FT-C6 | noise_sigma 0.0-vs-1.0 builder disagreement → 1e8 weight blowup path | `prepared_fitting.cpp:523` vs `calib.cpp:198` |
| FT-C7 | Pinned SplineVol serves butterfly-violating slices (diagnostic-only) | `spline_curve.cpp:165-218`, `vol_curve.cpp:574-589` |
| FT-C8 | Canonical eSSVI fits filter-free Legacy population, serial across expiries | `surface_parity.cpp:298-313` |
| FT-C9a | Alternate eSSVI driver defaults to quality-destroying θ-bump calendar projection | `essvi_calib.cpp:1215-1217` |
| FT-P | LM heap alloc in inner loops; selector cold per-row holdout solves; bench times dead driver | `essvi_calib.cpp:313-327`, `svi_calib.cpp:67-85`, `curve_selector.cpp:528-561`, `bench/fitting_throughput_bench.cpp` |
| PR-C1 | Session serve path never checks CorrectionCache box; σ>1.5 / T<T_min served clamped, no flag | `session.cpp:666-670`, `correction.cpp:658-660` |
| PR-C2 | Auto-ON AVX2 marks vs stale "bit-reproducible scalar default" docs; no non-AVX2 CI leg | `american_boundary_batch.cpp:83-88,153`, README:328-331 |
| PR-C3 | Legacy `bulk_price`: no intrinsic floor; gamma forward-space in corr branch, spot in no-corr branch | `bulk.cpp:171-227` |
| PR-C4 | σ→0 guard returns spot intrinsic before regime classification (carry-dominant limit wrong) | `american.cpp:1786-1789,2027-2031` |
| PR-P1 | σ-node builds + correction row sampler not packed through shipped 4-lane AVX2 solver | `boundary_interp.cpp:273-288`, `correction.cpp:536-560` |
| PR-P2 | F6: barycentric weights recomputed per quad point per sweep (~10-15% of sweep) | `american.cpp:1004-1025` |
| GR-P1-1 | Legacy plan: American price + European greeks on B76AlCache route | `portfolio_risk.cpp:556-597` |
| GR-P1-2 | Legacy scenario engine drops EEP entirely | `portfolio_risk.cpp:229-244` |
| GR-P2-1 | Ok gate checks price finiteness only; NaN greek poisons book totals | `portfolio_pricer.cpp:782-789` |
| GR-P2-3 | No baked-carry staleness assert on cached greeks | `american.cpp:1869-1898` |
| GR-F1 | No bucketed risk (per-underlier/per-expiry/key-rate); dP/dq & dDiv kernels exist, unsurfaced | `portfolio_pricer.hpp` totals |
| GR-P3-S | Scenario Exact cells: cold solves, per-cell alloc in parallel loop | `scenario_grid.cpp:236,251-252` |
| SE-P1-1 | View skips semantic validation; NaN T column ⇒ OOB read served silently | `priced_surface_view.cpp:165-347,365-408` |
| SE-P1-2 | `n_quad_price` not persisted; round-trip contract false for decoupled premium | `surface_archive.cpp:483-485,1036-1039` |
| SE-P1-3 | `col_borrow_off` (+nused/ndropped) unvalidated in view | `priced_surface_view.cpp:193-217` |
| SE-P2-1/2 | No fsync-before-rename; Windows rename fails under readers, no retry | `surface_archive.cpp:638-670`, `surface_db.cpp:769-794` |
| SE-P2-3/7 | validate_all never called in production; v2 lacks lookup↔directory cross-check | `surface_archive.cpp:1264-1297,755-759` |
| AN-P1-1 | 100× vega-unit mismatch between dispersion routes | `dispersion.cpp:490-495` vs `listed_dispersion_schedule.cpp:140,169` |
| AN-P1-2 | Var strip truncates wings by fixed ±1.5 span, no flag on parametric surfaces; two divergent MFIV impls | `derivatives.cpp:39-51` vs `analytics_density.cpp:85-89` |
| AN-P1-3 | Production eMove is naive two-pillar; joint `fit_earnings_term` unwired (AAPL +173% fitted-vs-truth case) | `event_vol.cpp:75-106`, `analytics_aggregate.cpp:39-60` |
| AN-P2-5 | Projection spine NaN for Wing/C8/CStar — event-aware serving dead for event families | `projection.cpp:44-83` |
| AN-P2-6 | Three delta conventions (American / B76-forward / American-carry-seeded) | `analytics.hpp:37-39`, `projection.cpp:31-40`, `contract_projection.cpp:132-270` |
| AN-W | `analytics.hpp` + 10 headers missing from `vol.hpp` umbrella; derivatives.hpp on legacy surface types | `vol.hpp:82-183` |
| BT-P1-1 | Decision+fill on same snapshot at model mid; raw_bid/raw_ask recorded, never used as fill | `backtest.cpp:1977-2008`, `listed_dispersion_strategy.cpp:118-140` |
| BT-P1-2 | `ExcludeAndReport` default silently truncates NAV | `backtest.hpp:283-319`, `backtest.cpp:950-985` |
| BT-P1-3 | Hedge trades at spot 0.0 on missing surface; shares silently unmarked | `backtest.cpp:573-585,1823-1829,1956-1961` |
| BT-P1-4 | No assignment/early-exercise sim; hedge shares use q_eff(0.25y) proxy, no discrete divs | `backtest.cpp:828-841,1966-1973` |
| BT-P1-5 | PIT universe cannot express removals (survivorship by construction) | `dispersion_workflow.cpp:232-245` |
| BT-P2-8 | Zero-bid/locked quotes accepted; no staleness check | `listed_dispersion.cpp:198-201` |
| BT-W | Frictions/financing/provenance-gate implemented but wired by zero CLIs; `hedge_slippage_bps` never set | `examples/spy_dispersion_backtest.cpp:482-484` |
| BT-T1 | Corpus worker cap ~9/16 structural: per-date fan-out boundary + inner workers forced 1 | `corpus.cpp:585-586,1075-1078`, `fit_scheduler.cpp:182` |
| BT-T2 | Strategy-overload replay loads whole board; schedule enumerates uids (subset unused) | `backtest.cpp:1424-1434,1836-1838` |
| PY-1 | `AtxError` loses ErrorCode; regex-on-message is the only dispatch | `module.cpp:21-30`, `result.hpp:11-20` |
| PY-2 | ts_ns int64 through float() → ±128 ns corruption | `report/io.py:74` |
| PY-3 | `implied_vol_batch` aborts whole batch on first bad lane | `bindings/pricing.cpp:88-95` |
| PY-4 | Regime-first reporting contract stranded on unmerged branch; renderer accepts regime-less tracks | `report/dispersion.py:70-86` |
| PY-5 | `AloPricer.price` releases GIL around mutating call | `bindings/pricing.cpp:204` |
| PY-F | Fitting 0% bound; no numpy batch American path; ~25/90 headers covered | bindings vs `vol.hpp` |

## 3. Global constraints (every subagent obeys, verbatim discipline)

- **Standing authorization (user, prior sprint):** *"allowed to break byte identical
  or tolerance tests if the performance gains are real, algorithmically correct,
  and differences are not economically meaningful as long as you document them."*
  Documentation is the price of that permission: measured old value, new value,
  delta, economic-materiality argument, in the task's commit + this doc's tracker.
- **Never mutate `C:\atx-data\...`** — reference data. Copy to scratch first.
- **Databento spend cap $100 total** (shared with the OPRA-universe goal; this
  sprint should spend $0 — no new data required).
- **Economic-correctness gate for pricing/greeks changes:** price abs err ≤
  `min(0.5·tick, 0.1·vega·1e-4)` and inside quote half-spread on the real-OPRA
  fixtures; greeks vs FD reference to documented tolerance; no new
  butterfly/calendar/vertical arb. Pure-refactor and fewer-solves tasks claim
  bit-identity or document exact divergence (summation order etc.).
- **Determinism across worker counts is preserved everywhere.** Any new parallel
  fan-out uses disjoint output slots + static or deterministic-dynamic partition,
  and lands with a thread-count invariance test.
- **Tier honesty:** any accuracy-tier change lands with an explicit economic gate
  vs the accurate tier on real-OPRA fixtures and a documented policy of where the
  cheap tier is allowed.
- **TDD per task:** write the failing test, run it, watch it fail, fix, watch it
  pass, commit. Tests that cannot observe the defect (empty-subject, symmetric
  guard, value==default) are rejected in review.
- **House style:** C++20, no UB, `Result<T>` not exceptions, `/W4 /permissive-
  /WX` clean (clang-cl), Rule of Zero, `.agents/cpp/agent.md`.
- **Build discipline:** worktree's own `scripts/atx-build.ps1` by absolute path;
  isolated `FETCHCONTENT_BASE_DIR` per worktree; `-j 6` (default `-j` OOMs on
  rel-avx2); `build` verb is hardwired to Debug — rel-avx2 needs raw
  `cmake --build build-rel-avx2 --target …`; submodule init per worktree
  (`git submodule update --init atx-core/third-party/databento-cpp`).
- **Attribute test failures serially.** `ctest -j 16` inflates the failing set
  (measured +26). Quiet-window protocol for all timing claims.
- **Park protocol:** PRIMARY commit to your own branch; SECONDARY `cp` to scratch.
  `git stash` BANNED (shared repo-global stack). Never trust `git diff > file` —
  rtk may rewrite it; validate patches with `grep -c "^diff --git"`. Byte
  comparisons via `cmp`/SHA256, never `diff`.

---

## 4. Workstreams & tasks

Task classes: `fix` (correctness), `feat` (capability), `perf`, `infra`, `docs`.
Every task: re-grep cited lines; failing test first; commit message cites ledger ID.

### WS-M — Keystone: disp-hotpath reconciliation (SERIAL, dispatch first, nothing else runs)

Owner: one senior agent (or PM-driven). Branch: `feat/pipeline-m` off `main`.

| ID | Title | Files / scope | Approach & gate | Class |
|---|---|---|---|---|
| **M1** | Merge `feat/disp-hotpath @ 30809a0` into main | 7 known textual conflicts: `listed_dispersion_strategy.cpp` + test, `examples/spy_dispersion_backtest.cpp`, `src/simd/american_boundary_batch.cpp`, `tests/backtest_test.cpp`, `tests/american_batch_test.cpp`, `tests/CMakeLists.txt` | `--no-ff` merge. Resolve against BOTH sprints' intent; watch for double-applied SIMD fixes (branch `095be4a` greeks-ledger invariance vs peer AVX2 kernel work). The branch carries: zero-copy Sealed/Mutable `ArchiveBacking` (+ zcfix `e0da68b` caller-owned-cache fix), laned greeks tolerance re-band (`isa_golden_tol.hpp`), WS-X typed frictions/financing/risk-limits + Almgren √-impact, regime-first artifacts, content-derived `created_ts_ns` (`07950ec`). Gate: clean build dev + rel-avx2; serial atx-vol-tests run recorded. | infra |
| **M2** | Re-establish golden baselines post-merge | `C:\atx-data\spy-dispersion\runs\bt-sota-baseline` (read-only!), new SHAs recorded in STATUS doc + `docs/superpowers/plans/2026-07-20-dispersion-two-route-parity.md` | Old pins `0737660…`/`ac97a643…` will legitimately break (peer engine rewrote the greeks kernel). Run 82- and 135-session replays 3× on rel-avx2 quiet host; require run-to-run identity; pin new SHAs. Any run-to-run NON-identity = engine nondeterminism → STOP, escalate (outranks everything). | infra |
| **M3** | Triage the 4 pre-existing serial failures | `tests/surface_v2_qualification_test.cpp` (RiskBuild Latency/Balanced), `tests/…SurfaceV2Provenance.ValidationFallbackAdmissionRecordsTheServedFamily`, `tests/pricer_fitter_test.cpp` (`LocalRiskRefitPublishesCopyOnWriteGeneration`) | PricerFitter test is a documented over-assertion (STATUS follow-on 1): rewrite it to assert reuse-with-correct-IVs for a mid-preserving spread rescale (mechanism at `session.cpp:2075-2243`). For the other three: root-cause; latency gates may need quiet-window re-pin vs genuine regressions — classify and fix or re-pin with measurement. Gate: serial run = 0 unexplained failures. | fix |
| **M4** | Regime-mandatory artifacts on main | post-merge `dispersion_run.cpp` / tearsheet TSV emitters | Verify the branch's `friction_regime` key survives merge into every artifact writer; add C++ test asserting the emitted run TSV contains `friction_regime` and the renderer input contract documents it (Python side is Y4). | fix |
| **M5** | Doc sync | `sprints/2026-07-19-spy-dispersion-hotpath-sprint-STATUS.md`, README | Mark the deferred integration DONE with new SHAs; note tolerance re-band values now on main. | docs |

### WS-A — Pricer core correctness + solve throughput

Branch `feat/pipeline-a`, worktree `wt-pipe-a`. Owns: `american.cpp/hpp`,
`correction.cpp/hpp`, `session.cpp/hpp`, `bulk.cpp`, `query_pricing.hpp`,
`boundary_interp.{cpp,hpp}`, `scenario_grid.{cpp,hpp}`, `implied_vol.cpp`,
`simd/american_boundary_*`, README pricing sections.

| ID | Ledger | Title | Approach | Test-first gate | Class |
|---|---|---|---|---|---|
| **A1** | PR-C1 | Gate `VolaSession` serve path on cache box | In `fair_value`/`greeks`/`evaluate_ladder`/cached-IV routes: consult `CorrectionCache::contains(k,T,σ)`; out-of-box → cold Andersen-Lake fallback (mirror the `PricedSurface` certified-box `ColdFallback` pattern, `query_pricing.hpp:40-45`) + a route flag in diagnostics. Widen build box: σ_max = max(1.5, 1.25·max fitted slice σ) at `session.cpp:666-670`. | New `session_test`: build session on a synthetic board with fitted σ≈2.0 (earnings-style); assert `fair_value` matches cold AL within economic gate (fails today: clamped-at-1.5 correction); second test: query T = 0.4·T_min asserts cold-fallback route flag set and price ≥ intrinsic, ≤ cold+tol. | fix |
| **A2** | PR-C3 | `bulk_price` floor + unit consistency, or retire | Preferred: route `AmericanFirstOrder` branch through `american_greeks_first_order` (`american.cpp:1849-1908` — already correct) and apply `floor_cached_price` on PriceOnly; if call sites make that infeasible, mark the API `[[deprecated]]` and quarantine from the umbrella. | `bulk_test`: deep-ITM put via cached route asserts price ≥ intrinsic (fails today); gamma parity test: corr-branch vs no-corr-branch gamma on same contract within band (fails today by ~m² factor). | fix |
| **A3** | GR-P2-3 | Baked-carry staleness tripwire | Debug-assert + release-mode counter/flag when cached-jet query (r,q) deviates from `baked_r()/baked_q()` by > 25bp (reuse the C2 stale-gate threshold); surface `CacheOutOfBoxClamps` + new `CacheCarryDrift` counter in session diagnostics. | Test drives cached greeks with r shifted 100bp from bake; asserts flag fires (fails today: silent). | fix |
| **A4** | PR-C4 | σ→0 regime-correct limit | Classify regime before the degenerate guard; σ ≤ 1e-8 returns European σ→0 limit `df·(forward intrinsic)` floored at spot intrinsic. | `american_test`: put r=0, q=5%, T=1, S=K=100, σ=1e-9 asserts ≈ df·(K−F)⁺ (≈4.88, fails today: returns 0). | fix |
| **A5** | PR-P1 | Pack σ-node + row-sampler solves through 4-lane AVX2 | `SigmaBoundaryInterp::build` (`boundary_interp.cpp:273-288`) and `CorrectionCache` row sampler (`correction.cpp:536-560`) are batch-shaped; route through `american_price_batch_resolved` pack path under the same Auto predicate. Bit-parity with scalar per existing `isa_golden_tol` framework. | Parity test (pack vs scalar per node, tol per isa_golden_tol) + quiet-window bench: cache build time row, target ≥ 2× on AVX2 host. | perf |
| **A6** | PR-P2 | F6 barycentric hoist | Precompute sweep-invariant `wbary[i]/(zc−z[i])` + `den` per node set into workspace (+~28KB); preserve summation order (bit-identity claim). | Bit-identity test vs pre-change on fixed seeds; bench sweep row ≥ 8% improvement or documented no-win revert. | perf |
| **A7** | GR-P3-S | Scenario Exact-cell warm-start + alloc hoist | Reuse `AloPricer` per unique across cells (σ-sweep warm state exists for this shape); hoist `pprime` out of the per-cell parallel loop into per-worker scratch. Determinism: per-worker scratch, disjoint cells. | `scenario_grid_test` value-identity vs old path + solve-ledger count drop per grid (deterministic gate); thread-invariance test stays green. | perf |
| **A8** | PR-C2 | Kill doc/flag drift + non-AVX2 leg | Fix `american_boundary_batch.cpp:88` comment, `american_batch.cpp:307-309`, README `:328-331` SIMD paragraph, `scalar_erfc.hpp` header (keep-or-delete decision); add a CI/test-runner leg with `ATX_SIMD_ISA=ForceScalar` (override exists) running the pricing suites. | The ForceScalar leg passes; grep gate in review: zero remaining "Auto => scalar" / "dark in production" strings. | docs |

### WS-B — Fitting correctness + fit perf

Branch `feat/pipeline-b`, worktree `wt-pipe-b`. Owns: `svi_calib.cpp`,
`essvi_calib.cpp`, `arb.cpp`, `vol_curve.cpp`, `curve_selector.cpp`,
`surface_parity.cpp`, `prepared_fitting.cpp`, `spline_curve.cpp`, `calib.cpp`,
`curve_fit.cpp`, `bench/fitting_throughput_bench.cpp` + their headers/tests.

| ID | Ledger | Title | Approach | Test-first gate | Class |
|---|---|---|---|---|---|
| **B1** | FT-C1 | Clamp the winning NM vertex | In `nm_search`, either store clamped coordinates in the simplex or clamp the returned best vertex into the (m,σ) box before the (u,v)→(a,b,ρ) map at `svi_calib.cpp:884-892`; reject σ ≤ σ_min at the map. | Test: short-dated kinked smile constructed to saturate σ_min (drive the optimum to the bound); assert returned params in-box AND fitted slice non-flat (RMSE vs quotes < flat-slice RMSE). Must fail on current code (reproduce the negative-σ vertex win — if not reproducible after honest effort, pin the invariant with a property test over 1000 randomized smiles: params always in-box). | fix |
| **B2** | FT-C2, FT-C7 | Sufficient strike-density admission | (a) For SVI on the served path: extend `validate_parametric_risk_shape` to scan the full quoted range ± 0.5 (match C8/CStar policy, `vol_curve.cpp:542-548`) — parametric closed form, cost is nil; apply in selector disqualification too. (b) Pinned SplineVol: call the risk-shape gate in the `SplineVol` branch of `fit_slice_curve`; violating pinned fits fail admission (fallback ladder handles it). | (a) Vogt counterexample (a=−0.041,b=0.1331,ρ=0.306,m=0.3586,σ=0.4153,T=1) fed through the SVI served-path gate asserts REJECTED (passes gate today — must fail first). (b) hand-built butterfly-violating spline via pinned route asserts admission failure. | fix |
| **B3** | FT-C3 | Fix both Lee-bound conventions | eSSVI `lee_project`: enforce θφ(1+\|ρ\|) ≤ 4 (T-free, total variance) — i.e. make it consistent with `essvi_phi_max`; correct the comment. SVI-MM `mm_project_admissible`: bound total-variance wing slope b(1+\|ρ\|) ≤ 4 (T-free). Class: accuracy-improving; long-T wings will move — document delta on the LEAP fixture per §3. | Test T=2y high-vol steep-skew slice: wings no longer flattened (fitted wing IV within band of quotes; fails today); short-T SVI slice with b(1+\|ρ\|)=100 asserts projected (passes vacuously today — must fail first). | fix |
| **B4** | FT-C8 | Canonical eSSVI: Configured prep + parallel expiries | Honor `in.fit_prep_policy` in `run_surface_parity` (kill the hardcoded Legacy at `surface_parity.cpp:311-313`), route through the configured filter cascade; parallelize the per-expiry loop via `run_bounded_fit_tasks` with disjoint slots (pattern: `curve_fit.cpp:368-483`). Default remains Legacy-compatible until the quality gate passes (flag-guarded rollout): compare on XOM + SPY real fixtures — in-band %, χ², vol-RMSE within documented tolerance, then flip default. | Determinism test (1 vs 8 workers bit-identical); quality A/B recorded in commit; filter test: flagged/wide quotes excluded under Configured (fails today). | fix/perf |
| **B5** | FT-C5, FT-C6, FT-C9a | Small-defect cluster | (a) reject σ ≤ 0 in `svi_jw_to_raw` symmetric branch; (b) unify vega≤0 ⇒ `noise_sigma = 1.0` in `prepared_fitting.cpp:523`; (c) alternate eSSVI driver: default `validate_no_arb` no longer runs the θ-bump projection silently (make it explicit opt-in, matching README warning). | Three unit tests, each failing first: σ=0 JW round-trip rejected; vega-0 row weight equals configured-builder weight; alternate driver default leaves ATM level unmoved on a crossing surface. | fix |
| **B6** | FT-P | Selector + LM micro-perf | (a) Holdout scoring via `audit_european_equiv_iv_batch` per (expiry,side) instead of cold `american_price` per row (`curve_selector.cpp:559-561`); (b) stack-capacity matrices in eSSVI `lm_step` and SVI-MM `solve_spd_dense` (copy CStar pattern `cstar_calib.cpp:52-58`). | Selector outcome-identity test (same candidate chosen, same metrics within 1e-12) + solve-ledger count drop; LM bit-identity on fixed seeds; bench delta recorded. | perf |
| **B7** | FT-P | Re-point fitting bench at the canonical facade | New bench row timing `PricerFitter::fit` on the real SPY fixture with per-phase counters (carry/de-Am/cache/calib/diag via FitTimings); retire or demote the `essvi_calib_surface` row; capture quiet-window baseline. | Baseline JSON committed under `bench/baselines/`; `compare_baseline.py` passes. | infra |

### WS-C — Storage hardening

Branch `feat/pipeline-c`, worktree `wt-pipe-c`. Owns: `priced_surface_view.cpp`,
`surface_archive.{cpp,hpp}`, `surface_archive_v1.cpp`, `surface_db.{cpp,hpp}`,
`surface_db_populate.cpp`, `tools/migrate_atxvsa_v1_to_v2.cpp`,
`docs/atxvsa2-format.md` + tests.

| ID | Ledger | Title | Approach | Test-first gate | Class |
|---|---|---|---|---|---|
| **C1** | SE-P1-1, SE-P1-3 | View validation to reconstruct-equivalence | In `create_over_record`: validate ascending finite T, positive finite forwards, S>0, finite r (mirror `PricedSurface::create` checks at `priced_surface.cpp:381-405`); add `col_borrow_off`/`col_nused_off`/`col_ndropped_off` to the bounds conjunction. O(n) at open, off hot path. | Adversarial tests failing first: record with NaN in last T slot → view creation returns ParseError (today: silent OOB-read serve); record with `col_borrow_off` pointing past extent → rejected. | fix |
| **C2** | SE-P1-2 | Persist `n_quad_price` | Use the reserved u16 in `ArchiveV2SurfaceHeader` (and `DbSymbolRecord`); bump format salt; reader defaults missing/0 → tied (back-compat with all existing archives); thread through view + reconstruct + db round-trip; migration note in `atxvsa2-format.md`. | `SurfaceArchiveV2.RoundTripsNQuadPrice`: write surface with al_opts{n_quadrature=8, n_quad_price=32}, reload, assert repriced theo bit-identical to pre-serialize (fails today); old-archive read still succeeds (fixture). | fix |
| **C3** | SE-P2-1, SE-P2-2 | Durable atomic publish | `FlushFileBuffers`/fsync the temp file before `rename` in all three writers (v2, v1, manifest); bounded retry-with-backoff on Windows rename failure (reader-held destination), preserving temp on final failure with a clear IoError. | Test: publish succeeds while a reader holds the old file open with FILE_SHARE_READ only (fails today with IoError on first try); fsync presence asserted via code review + crash-consistency note in format doc (no automated power-loss test — document). | fix |
| **C4** | SE-P2-3, SE-P2-7 | v2 adversarial test port + integrity cross-check | Port the v1 "repaired blob" corruption corpus to v2 (corrupt lookup slots via `map_symbol`, cross-linked slot↔directory, overlapping columns, unknown kind through `map_all`, `open_mapped` direct, `n_slices` u16-vs-u32 mismatch); add the lookup↔directory agreement check to `open_impl`. | Each new adversarial test fails (accepts corrupt input) before the cross-check lands, passes after. | fix |
| **C5** | SE (stamp doc) | `created_ts_ns` semantics doc (post-M1) | Update `surface_archive.hpp:336` comment, `atxvsa2-format.md`: field is content-derived identity (not wall-clock, may be negative as int64, 32-bit entropy caveat, never tamper evidence); note `DbPartitionInfo.created_ts_ns` stays wall-clock. | Doc grep gate: zero remaining "fill from the system clock" claims. | docs |
| **C6** | SE-P2-6 | Checkpoint resume O(framing) | Give the checkpoint counter a framing-only enumeration (directory entries) instead of `map_all()` materializing heavy kinds — coordinate with WS-T (corpus.cpp owner) via the archive-side API: add `SurfaceArchiveV2::entry_count()/entries()` cheap accessors here; WS-T consumes. | Unit test: entries() count == map_all count on mixed-kind fixture without constructing views (assert via allocation counter or kind-materialization hook). | perf |

### WS-G — Greeks / risk product

Branch `feat/pipeline-g`, worktree `wt-pipe-g`. Owns: `portfolio_pricer.{cpp,hpp}`,
`portfolio_risk.{cpp,hpp}`, `portfolio.hpp`, `portfolio_greeks.cpp`,
`portfolio_price.cpp`, `pnl_attribution.{cpp,hpp}` + tests.

| ID | Ledger | Title | Approach | Test-first gate | Class |
|---|---|---|---|---|---|
| **G1** | GR-P2-1 | isfinite sweep at the Ok-stamp site | In `solve_uniques` Ok-stamp: check all requested greek columns finite; non-finite → `NumericError` lane (NaN-isolated like other error lanes). Mirror in FD bundle return. | Test injects an overflow-shaped contract (extreme S/σ/T where σ+h stencil overflows, base finite); asserts lane status NumericError and book totals finite (fails today: inf vega in totals). | fix |
| **G2** | GR-F1 | Bucketed risk + carry/div columns | Add per-underlier and per-expiry aggregation to `PriceTotals`/`PnlTotals` (opt-in bucket spec to keep the zero-alloc path), plus `dP_dq` column (kernels exist: `american_carry_greeks*`) and per-discrete-dividend `dP_dDiv` surfacing via `american_dividend_sensitivities`. Keep determinism: serial scatter into per-bucket slots. | Tests: two-underlier three-expiry book asserts bucket sums == whole-book totals bit-exactly; dP_dq column vs direct kernel call identity; thread-invariance green. | feat |
| **G3** | GR-P1-1, GR-P1-2, GR-P2-5 | Legacy engine: fix-or-quarantine | Decision rule: G2 gives canonical buckets ⇒ legacy no longer owns aggregation. Then: fix the two P1s minimally (route `price_group` FirstOrder greeks through the correction-jet like `price_option`; make `scenario_pnl` include the EEP overlay when a cache is bound) OR gate the module behind `ATX_VOL_ENABLE_LEGACY_PORTFOLIO` (off by default) with a migration note. Do NOT leave it shipping as-is. Also close GR-P2-5 (status-gate the legacy aggregate) whichever path. | If fixed: deep-ITM American put on B76AlCache route asserts delta within band of canonical path delta (fails today by EEP delta); scenario base marks == pricing marks (fails today). If gated: compile-off default + umbrella exclusion test. | fix |
| **G4** | GR wiring | Laned-greeks Auto decision + needs mask | Either wire the laned AVX2 greeks bundle under Auto at `priced_surface.cpp:1061-1074` (with the seed-bit-identity concern resolved via the isa_golden_tol framework + M2's fresh goldens) and honor `GreekNeeds` in the laned route, or document ForceAvx2-only as the contract and stop advertising the throughput in README. Measured decision, quiet-window. | A/B bench row + parity suite (`simd_greeks_test`, laned invariance) green under chosen policy; README updated. | perf |

### WS-E — Analytics conventions + productization

Branch `feat/pipeline-e`, worktree `wt-pipe-e`. Owns: `dispersion.cpp`,
`listed_dispersion_schedule.cpp`, `analytics_*.cpp`, `event_vol.cpp`,
`earnings_term_fit.cpp`, `derivatives.{cpp,hpp}`, `projection.cpp`,
`include/atx/vol/vol.hpp`, `analytics.hpp` + tests. (Session-side eMove wiring:
E3b runs AFTER WS-A merges — see §6.)

| ID | Ledger | Title | Approach | Test-first gate | Class |
|---|---|---|---|---|---|
| **E1** | AN-P1-1 | One vega unit for `target_vega` | Canonical unit: **dollar vega per vol point** (industry convention, matches listed route). Convert in `build_dispersion_book` (`dispersion.cpp:490-495`: divide by `straddle_vega·multiplier·0.01`); document on `DispersionConfig::target_vega`; add a loud CHANGELOG note (projected-route books shrink 100×; goldens re-pinned — coordinate with M2 goldens, this lands after M1 so the re-pin is once). | Cross-route test: same `target_vega` ⇒ projected-route straddle vega within 5% of listed-route straddle vega on the shared fixture (fails today by ~100×). | fix |
| **E2** | AN-P1-2 | Var-strip adaptive wings + single MFIV | Widen `derivatives.cpp` strip span to `max(tier_span, 6·σ_atm·√T)` (adopt `analytics_density.cpp:85-89` policy); set truncation flags from span coverage, not IV finiteness; extract one shared strip/grid builder used by both derivatives.cpp and analytics_density.cpp (single forward-interp convention: log-F). | Test σ=0.6, T=1 synthetic lognormal surface: K_var within 0.5 variance point of closed form (fails today, biased low); flag test: forced narrow span sets `StripTruncated*`. | fix |
| **E3a** | AN-P1-3 | Promote joint eMove (analytics side) | New `implied_emove_joint(...)` in `event_vol.cpp` delegating to `fit_earnings_term` over ALL usable pillars (per research doc); route `earnings_implied_move` (`analytics_aggregate.cpp:39-60`) and `EventContext` construction through it with two-pillar fallback + method flag in output. | Reproduce the review's AAPL-shaped case (no near expiry spanning event): joint estimate within 25% of truth where two-pillar is +173% off (test fixture from the atmcen sweep data; fails with two-pillar). | fix |
| **E3b** | AN-P1-3 | Session eMove wiring (post-WS-A merge) | Swap `VolaSession`'s eMove solve to `implied_emove_joint`; rebase onto post-A main; session-owned files edited only in this task. | Session diagnostics expose method flag; parity test vs E3a direct call. | fix |
| **E4** | AN-P2-5 | Projection spine for C8/CStar/Wing | Implement `slice_w`/`slice_T` for the remaining `Parametrization` tags (all have closed-form w(k); CStar via its basis eval) so `surface_insert_vol_slice`/event-aware `surface_eval_ex` work on event-routed families. | Test: C8-fitted event board through `event_aware_w` returns finite, positive w matching direct slice eval at pillars (fails today: NaN). | fix |
| **E5** | AN-W, AN-P2-6 | Umbrella + delta-convention contract | Add `analytics.hpp`, `event_vol.hpp`, `earnings_term_fit.hpp`, `vol_time.hpp`, `sr_tenor_grid.hpp`, `tearsheet.hpp`, `run_report.hpp`, `dense_slice.hpp`, `dispersion_strangle.hpp`, `strategy.hpp` to `vol.hpp` (grouped, commented); document the three delta conventions in `analytics.hpp` header with a `DeltaConvention` enum on `SurfaceAnalyticsConfig` (American default, B76-forward vendor-compat option) applied to wing/RR/BF strike solves. | Umbrella compile test includes-everything; convention test: 25Δ RR under B76-forward mode matches hand-computed B76 strike (new capability). | feat |
| **E6** | AN-W | Re-type `derivatives` onto `VolSurface`/`PricedSurface` | Replace the `EssviSurface`/`SviSurface` template params with the modern surface interface (iv(k,T) + forward accessors); keep numeric behavior (E2's shared strip builder is the seam); wire `var_swap_fair_strike` reachable from `PricedSurface`. | Existing derivatives tests pass re-typed; new test calls var-swap through a fitted `PricedSurface` end-to-end. | feat |

### WS-F — Backtest economic realism

Branch `feat/pipeline-f`, worktree `wt-pipe-f`. Owns: `backtest.{cpp,hpp}`,
`dispersion_workflow.cpp`, `listed_dispersion_strategy.cpp`,
`listed_dispersion_reconciliation.cpp`, `listed_dispersion.cpp`,
`examples/spy_dispersion_backtest.cpp`, `examples/mag7_dispersion_backtest.cpp`
+ tests. Forks AFTER WS-M (heavy overlap with the merge).

| ID | Ledger | Title | Approach | Test-first gate | Class |
|---|---|---|---|---|---|
| **F1** | BT-P1-3, BT-P1-2 | Close the accounting-leak family | (a) `spot_of` missing-surface → hard error on any uid with residual hedge shares (fail closed like roll-closes); (b) shares-PnL loop no longer `continue`s silently — unpriced shares follow `UnpricedLotPolicy`; (c) flip the default `UnpricedLotPolicy` to `Error` (callers opt into ExcludeAndReport explicitly; fix `mag7` example); (d) add per-row NAV-vs-liquidation reconciliation: independently reprice cash + book MTM + shares MTM each recorded row, assert == NAV within 1e-9 in tests (engine-level debug check behind a flag). | Tests failing first: surface-gap fixture with residual hedge shares asserts run errors (today: free-flatten at spot 0.0); ExcludeAndReport multi-day-gap fixture asserts NAV == liquidation after gap under new recon (today drifts). | fix |
| **F2** | BT-P1-1 | Quote-side fill option for the listed route | `ListedDispersionStrategy`: new fill policy enum {ModelMark (compat default), QuoteMid, CrossSpread} using the already-recorded `raw_bid/raw_ask`; CrossSpread pays ask on buys / hits bid on sells; missing quote on a leg under Quote* policies → fail closed (consistent with F1). Wire `--fill-policy` in the spy CLI. Report both NAV tracks when policy ≠ ModelMark (reuse reconciliation machinery). | Test on the listed fixture: CrossSpread NAV ≤ QuoteMid NAV ≤ ModelMark NAV on a round-trip trade (strict ordering by construction; fails today: no such policies). | feat |
| **F3** | BT-P1-5, BT-P1-4 | PIT removals + share-ledger discrete dividends | (a) `universe_at`: `raw_weight == 0` row = explicit removal (documented semantic; loader accepts 0; membership excludes); (b) share ledger books discrete dividend cash on ex-dates from the corpus `cash_divs` (already priced into surfaces — reuse the same schedule) replacing the q_eff(0.25y) proxy when divs available; assignment simulation stays deferred (§9) but document it in `backtest.hpp`. | Tests: schedule with a zero-weight row asserts name leaves basket on effective date (fails today: sticky forever); delta-hedged fixture across an ex-date asserts share cash includes the dividend (fails today: proxy accrual only). | fix |
| **F4** | BT-W | Wire frictions/financing/provenance into CLIs (post-M1) | Expose in `spy_dispersion_backtest` + `mag7`: `--half-spread-bps`, `--vol-tick`, `--hedge-slippage-bps`, `--impact` (Almgren params from M1), full `FinancingConfig` flags, `--provenance=require-admitted-risk`, `--unpriced=error\|exclude`. Emitted run TSV records every value (regime-first per M4). | CLI round-trip test: flags → RunConfig → artifact TSV keys present with values; frictionless run now requires explicit `--frictions=off`. | feat |
| **F5** | BT-T2 | Subset-deserialize for schedule strategies | Strategy overload: when the strategy can enumerate uids up front (add `IStrategy::referenced_uids()` optional hook; `ListedDispersionStrategy` returns the schedule's set), construct the private snapshot cache with the subset (`backtest.cpp:1424-1434` pattern at `:1836-1838`). | Byte-identity of NAV track vs whole-board load on the golden fixture + measured per-date load-bytes drop (counter). | perf |
| **F6** | BT-P2-8 | Quote-quality gates | `is_valid_listed_quote`: reject bid == 0 (config floor), flag locked markets, add staleness threshold on `quote_ts_ns` (config, default 10 min) with per-date reject counters in the join report. | Tests: zero-bid quote excluded from selection (fails today); stale-quote fixture rejected with named reason. | fix |

### WS-T — Corpus throughput

Branch `feat/pipeline-t`, worktree `wt-pipe-t`. Owns: `corpus.cpp`,
`fit_scheduler.cpp` (+ `detail/fit_scheduler.hpp`), `corpus_board_fit.{cpp,hpp}`.
Forks after WS-M.

| ID | Ledger | Title | Approach | Test-first gate | Class |
|---|---|---|---|---|---|
| **T1** | BT-T1 | Cross-date task pooling | Restructure `CorpusBuildSession::append_date` fan-out: enqueue all (date × symbol) boards across the batch window into one `run_bounded_fit_tasks` pool; per-date checkpoint/manifest commit remains ordered after all its boards complete (order-preservation of `session.finish()` + input fingerprint is THE invariant — byte gate below). Alternative if ordering proves fragile: last-board-standing reclaims inner `fit_workers` (corpus.cpp:585-586). | Byte gate: golden 82-date corpus rebuild payload-identical (manifest + quality TSV + content fingerprints) vs serial baseline — content-derived stamps from M1 make this checkable; utilization: phase-split log shows ≥ 14/16 mean workers (from ~9/16) or written explanation. | perf |
| **T2** | SE-P2-3, SE-P2-6 | Checkpoint scrub + framing-only resume | Consume WS-C's `entries()` accessor for checkpoint counting (drop `map_all` materialization); add opt-in `validate_all` scrub at corpus checkpoint verification (config flag, default on for `--qualify` runs) so lazy CRC gets one scheduled verification point. | Resume-time test on mixed-kind fixture asserts no heavy-kind materialization (hook/counter); corrupted-record fixture caught at checkpoint with named error (fails today: served). | fix/perf |

### WS-Y — Python API

Branch `feat/pipeline-y`, worktree `wt-pipe-y`. Owns: everything under
`atx-vol/python/`. Y4 forks after WS-M.

| ID | Ledger | Title | Approach | Test-first gate | Class |
|---|---|---|---|---|---|
| **Y1** | PY-1,2,3,5, io | Fidelity cluster | (a) `AtxError.code` attribute (pybind custom exception; `ErrorCode` enum already bound); (b) `io.py:74` → `int(r[ti])`; (c) `implied_vol_batch` returns (vols, status) arrays — NaN + per-lane status, no throw on lane failure (batch-level throw only on shape errors); (d) drop the GIL release on mutating `AloPricer.price` (or add an internal lock); (e) `read_kv_tsv` header heuristic → match `key\tvalue` literal anywhere-first-noncomment; (f) escape spec values in the report colophon. | Failing-first tests: `err.code == ErrorCode.InvalidArgument`; ts_ns 1.7e18 round-trips exactly; batch with one below-intrinsic lane returns 9 good vols + 1 NaN/status (today: throws); two-thread AloPricer hammer test (TSAN-shaped, best-effort). | fix |
| **Y2** | PY-F | Bind the fit front-end | `OptionChain` (`from_frame`, `update_quotes`), `MarketEnv`, `PricerFitter` (fit, `value_chain` → numpy SoA columns via `OutputField`), `FittedSurface::to_priced_surface`; QuoteFrame construction from numpy arrays. GIL released during fit/value_chain (pure C++, pattern proven by run_backtest). | End-to-end pytest: synthetic panel → chain → fit → `value_chain(ModelIV\|Greeks)` → assert vs C++ golden values; determinism across n_threads from Python. | feat |
| **Y3** | PY-F | Numpy batch American + surface grids | Bind `american_price_batch`/`american_greeks_batch` (NaN+status convention per Y1c), strike-axis American IV batch, and a `PricedSurface.grid(k[], T[])` vectorized query; zero-copy in via `py::array_t` buffers, preallocated out. | Parity pytest vs scalar loops (bit-identical); microbench note in README (target ≥ 20× vs Python loop at 10k contracts). | feat |
| **Y4** | PY-4 | Regime-first reporting (post-M1) | Port the branch's regime contract into `atxvol.report`: `build_report_from_run` hard-refuses a track without `friction_regime`; regime banner + per-tile captions per WS-X-B design; add the missing run-dir fixture test for `build_report_from_run`; accept `pnl_track.tsv` naming alongside `backtest.tsv`. | Failing-first: regime-less fixture raises; regime fixture renders banner (snapshot assert); run-dir fixture test exercises the real deliverable path. | fix |

## 5. Ownership & disjointness

| File / area | Owner |
|---|---|
| `american.cpp/hpp`, `correction.*`, `session.*`, `bulk.cpp`, `boundary_interp.*`, `scenario_grid.*`, `query_pricing.hpp`, `implied_vol.cpp`, `simd/american_boundary_*`, README pricing/SIMD text | **WS-A** |
| `svi_calib`, `essvi_calib`, `arb`, `vol_curve`, `curve_selector`, `surface_parity`, `prepared_fitting`, `spline_curve`, `calib.cpp`, `curve_fit.cpp`, `bench/fitting_throughput_bench.cpp` | **WS-B** |
| `priced_surface_view`, `surface_archive*`, `surface_db*`, `tools/migrate_*`, `docs/atxvsa2-format.md` | **WS-C** |
| `portfolio_pricer.*`, `portfolio_risk.*`, `portfolio.hpp`, `portfolio_greeks/price.cpp`, `pnl_attribution.*` | **WS-G** |
| `analytics_*`, `dispersion.cpp`, `listed_dispersion_schedule.cpp`, `event_vol`, `earnings_term_fit`, `derivatives.*`, `projection.cpp`, `vol.hpp` | **WS-E** |
| `backtest.*`, `dispersion_workflow`, `listed_dispersion{,_strategy,_reconciliation}`, `examples/{spy,mag7}_dispersion_backtest.cpp` | **WS-F** |
| `corpus.cpp`, `fit_scheduler.*`, `corpus_board_fit.*` | **WS-T** |
| `atx-vol/python/**` | **WS-Y** |

Shared-file rules:
- `session.cpp` is WS-A's. WS-E's E3b (session eMove) executes **after WS-A merges**, rebased.
- `tests/CMakeLists.txt`: append-only, one line per new test TU, note in PR to ease union-merge.
- `counters.hpp` (solve ledger extensions): WS-A adds counters; others consume read-only.
- `priced_surface.cpp` greeks routing (`:1061-1074`, G4): **WS-G**, not WS-A (WS-A stays out of priced_surface.cpp).
- Nobody touches `C:\atx-data\**`. Golden re-pins are WS-M's alone (plus the single coordinated E1 re-pin, executed with the PM).

## 6. Merge order & gates

1. **WS-M** → main. Gate: build both presets; serial suite 0 unexplained failures; new golden SHAs pinned 3×-stable; STATUS doc updated. **All other worktrees fork from post-M main.**
2. **WS-A, WS-B, WS-C, WS-G, WS-Y(Y1-Y3)** in parallel. Merge as each completes its gate (serial suite green + own new tests + no cross-WS files touched). Suggested landing order on contention: C → A → G → B (B4's flag-flip A/B last).
3. **WS-F, WS-T, WS-E** fork post-M as well; E3b and Y4 wait for their stated deps (post-A, post-M respectively). E1's golden re-pin is coordinated with the PM (single re-pin event).
4. **Final gate:** quiet-host rel-avx2 serial suite; golden replay identity 3×; bench baselines re-captured for changed rows; scoreboard §1 walked line-by-line; sprint report appended to this doc.

## 7. Tracker

Integration trunk is `feat/pipeline-m` (user directive: nothing merges to local main
this sprint). Trunk tip after the wave-1 merge train: **264b2fe**.

| WS | Branch | Worktree | Status | Tip SHA | Gate log |
|---|---|---|---|---|---|
| M | feat/pipeline-m | wt-pipe-m | GATED, then wave-1 merge train | 264b2fe | M gate @5e2c31a: serial 2048/2048, 0 unexplained; goldens 3×-stable (82s `5e7ca065…`, 135s `141173fd…`); both presets clean; M1 reconcile→M2 re-pin→M3 known-reds→M4 regime pin→M5 doc sync. Merges: 9390a15 C, e35cddf A, 96172e5 G, 264b2fe B — all `--no-ff`, zero conflicts (only A∩C overlap = tests/CMakeLists.txt, append-only both sides) |
| A | feat/pipeline-a | wt-pipe-a | MERGED (e35cddf) | bf6968c | 7 commits; serial 2057/2057; review Approved-with-minors (0 Critical, 0 Important, 3 Minor, 3 Nit) — reviewer re-ran the failing-first tests, the ForceScalar leg and ScenarioGrid. A5/A6 + A7's solve-count half deferred with confirmed blockers |
| B | feat/pipeline-b | wt-pipe-b | MERGED (264b2fe) | eed7131 | 8 commits; serial 2011 pass / 43 skip / 0 fail under `-DATX_BUILD_BENCH=ON` (count delta vs the 2048/102 trunk baseline is a config artifact); review Approved-with-minors (0 Critical, 2 Important process-only, 5 Minor). B4 default flip / B6 selector holdout / B7 baseline JSON deferred, blockers confirmed real |
| C | feat/pipeline-c | wt-pipe-c | MERGED (9390a15) | 07dd317 | 6 commits; owning suites green (adversarial 17/17, writer/archive/db 140, db+populate 66/66, durability 2/2); full-serial verdict folded into the trunk gate; review in flight |
| G | feat/pipeline-g | wt-pipe-g | MERGED (96172e5) | de0101b | 3 commits + G4 no-code-change (premise overtaken by the WS-M merge: Auto already rides the laned AVX2 greeks via `avx2_greeks_selected`, GreekNeeds threaded); owning suites green (219 pass / 2 skip); full-serial verdict folded into the trunk gate; review in flight, carries the M1 AVX2 auto-merge deep-dive mandate |
| E | feat/pipeline-e | wt-pipe-e | in flight (wave 2) | 264b2fe | forked from the merged trunk; E3b unblocked by the WS-A merge; E1's golden re-pin is a coordinated PM event, serialized against WS-F |
| F | feat/pipeline-f | wt-pipe-f | DONE — reviewed, 0 Critical / 3 Important all closed | branch tip | 10 commits. F1 leak family (default `unpriced`=Error; spot-0.0 hedge and unmarked shares fail closed; opt-in NAV-vs-liquidation recon). F2 `ScheduleFillPolicy` + `RunConfig::book_entry_fill_slippage` — crossing the spread on every leg was BIT-IDENTICAL in NAV before, so the policy alone would have been vacuous. **F3(a) PREMISE FAILED** (WS-M's C3 already made PIT removals expressible); F3(b) discrete share dividends; F3(c) assignment deferral documented. F6 quote-quality gates: zero-bid rejection is admission-changing; staleness is **provably INERT** on the snapshot-stamped OPRA panel (measured: one distinct `ts` per file) and now reports `stale_unevaluable` instead of a reassuring 0. F4 listed route honours the typed spec, and build-corpus no longer ERASES every typed key on the way into the run dir. F5 `IStrategy::referenced_uids()` subset-deser: 33.3% record bytes, NAV bit-identical. Pinned 135-session replay run read-only: **does NOT abort** under F1(a)/(b); artifact moves 7.1e-13 rel on final NAV — wave-1 pricing drift, not WS-F |
| T | feat/pipeline-t | wt-pipe-t | in flight (wave 2) | 264b2fe | forked from the merged trunk; T2 unblocked — WS-C's C6 `entries()`/`entry_count()` is in the base |
| Y | feat/pipeline-y | wt-pipe-y | in flight (wave 2) | 10609ee | trunk + one base commit snapshotting the in-flight Python layer the PY-* findings were written against (see 10609ee's message); `test_dispersion_runarchive_e2e.py` is environment-blocked here by design |

## 8. Dispatch protocol (per subagent)

```powershell
# one-time per worktree (from repo root)
git worktree add C:\atx-wt\wt-pipe-<x> -b feat/pipeline-<x> main
Set-Location C:\atx-wt\wt-pipe-<x>
git submodule update --init atx-core/third-party/databento-cpp
& C:\atx-wt\wt-pipe-<x>\scripts\atx-build.ps1 --preset dev `
    '-DFETCHCONTENT_BASE_DIR=C:/atx-wt/wt-pipe-<x>/deps/dev' -DATX_BUILD_EXAMPLES=ON
# build: raw form, -j 6 (default -j OOMs on rel-avx2)
cmake --build C:\atx-wt\wt-pipe-<x>\build -j 6
# rel-avx2 perf: add -DATX_VOL_COUNTERS=ON -DATX_VOL_PROFILE=ON, raw --build build-rel-avx2
```

Per task: (1) read your lane's review file section for the ledger IDs cited;
(2) re-grep every cited symbol — lines have drifted; (3) write the failing test,
run it, PASTE the failure into your notes; (4) minimal fix; (5) test passes; (6)
run the owning suite serially; (7) commit `fix(vol): <title> [<ledger-id>]` with
the evidence + any documented drift; (8) update §7 tracker row. Park protocol and
rtk hazards per §3. If a task's premise fails on re-grep (code changed, defect
gone), STOP, record in tracker, move to next task — do not improvise scope.

## 9. Deferred — explicitly out of this sprint (tracked, not forgotten)

- **Discrete-cash-dividend PDE American pricer** (PR feature #1; grow from
  `tests/support/oracle_pricer_pde.*` behind `AmericanMethod::DiscreteDivPde`) —
  the single biggest Vola-parity differentiator remaining; needs its own
  design + sprint (accuracy tiers, de-Am integration, cache story).
- Calendar-coupled joint surface fit; asymmetric-ρ calibrator; Fengler overlay.
- Assignment/early-exercise simulation in the backtest (F3 documents the gap).
- Margin/capital model; multi-strategy netting; intraday multi-snapshot replay.
- Analytics time-series store + cross-sectional screens; realized-vol
  (Parkinson/GK/YZ) suite; strip-based implied correlation.
- SurfaceDb on `open_mapped` + `PortfolioPricer` on views (SE perf wave-2);
  archive compression decision (LZ4-per-record) before format ossifies.
- CStar production wiring (`VolCurveKind::CStar` + slice storage) — decide
  adopt-or-delete next sprint; 1.9k LOC unreachable today.
- Wheels/CI/abi3/`.pyi` stubs for the Python package.
- Two-route residual sign disagreement (`research/2026-07-19-dispersion-two-route-comparison-status.md`)
  — re-run the comparison AFTER E1 lands (vega-unit fix changes the projected
  route's book); if the sign disagreement survives, open a dedicated
  investigation.

## 10. Do-not-relitigate dead ends (carried forward + new)

- In-solve SIMD of the AL Gauss-Legendre inner loop via xsimd (6.6× slower) or
  SVML (toolchain-unavailable). The shipped route is batch-across-contracts
  4-lane packs — extend reach (A5), don't re-attempt in-solve vectorization.
- Temporal boundary warm-start across dates; eSSVI LM optimizer replacement
  (0 AL solves per eval — not a lever).
- Corpus fan-out call batching (WS-BATCH measured ±40% noise, no win) — the
  lever is T1's cross-date pooling, not batch size.
- P3.1 idle-E-core affinity for corpus fits (measured regression 9.94→15.45 s).
- Pinning `created_ts_ns` to a constant (breaks SnapshotCache identity — the
  content-derived stamp is the settled answer).
