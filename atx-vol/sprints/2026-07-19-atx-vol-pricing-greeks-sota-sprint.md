# atx-vol Pricing & Greeks Correctness/Throughput/Buildout Sprint — 2026-07-19

> **For agentic workers:** executed by **implementation subagents dispatched task-by-task by a PM agent, all in ONE worktree `C:/atx-wt/wt-pg-sota` on branch `feat/pg-sota`** (base `main @ 99f332f`). One task = one agent = one commit (or a small commit series). Every subagent reads §2 (global constraints) and §6 (traps) before touching a file, plus the review evidence for its task in `atx-vol/research/pg-review-2026-07-19/`.

**North star:** atx-vol fits **any US-listed American equity option surface** robustly and quickly. This sprint executes the 2026-07-19 five-lens review of the pricing/greeks module: fix every verified correctness defect, bank the highest-leverage throughput wins on the fitter's inner loops, wire in or delete dark code, and close the cheapest capability gaps that block "any underlying" (true expiry instants + 0DTE ingest, carry sensitivities, exercise-boundary API).

**Review evidence (read these — they carry file:line and verification detail):**
- `research/pg-review-2026-07-19/review-correct-core.md` — core math findings 1-11 + architecture note
- `research/pg-review-2026-07-19/review-correct-simd.md` — SIMD findings 1-10 + coverage map
- `research/pg-review-2026-07-19/review-perf.md` — perf findings F1-F13 + hot-path map + bench-state
- `research/pg-review-2026-07-19/review-wiring.md` — wiring findings 1-21
- `research/pg-review-2026-07-19/review-gaps.md` — capability inventory + gaps 1-10

---

## §1 Scoreboard & merge gates

| Gate | Requirement |
|---|---|
| G-BUILD | `atx-vol` + `atx-vol-tests` + bench build clean `/W4 /WX` (preset `dev`) |
| G-TEST | Full `ctest -R AtxVol` green (incl. slow suite where task touches its scope) |
| G-COUNTER | Perf tasks prove wins via **deterministic algorithm counters** (`ATX_VOL_COUNTERS`: boundary-solve counts, Clenshaw-traversal counts, Newton iteration counts) — NOT wall-clock. This host is noisy; counters are exact. Each perf task states its counter delta in the commit message. |
| G-PARITY | Numeric changes classify as: (a) **bit-identical** — existing pins must not move; (b) **repin** — bit-pinned tests updated with justification in commit message; (c) **economic-parity** — new-vs-old |Δprice| bounded and asserted by a test (budget stated per task). |
| G-BENCH | Wall-clock bench runs are *recorded* (`rel-avx2` preset, best-of-3) for the sprint report but never gate merges on this host. |

Expected headline outcomes:
- Cold AL seed Newton iterations: ~16 → ~5-7 per node (A1), counter-verified.
- Cached IV inversion: 3 → ≤2 tensor traversals per Newton step (P1a), → ~1 with P1b.
- Carry solve: warm-start ON cuts per-slice boundary solves (P2), counter-verified vs the 300-400/slice baseline.
- Audit reprices: O(strikes) ACCURATE cold solves → O(8)/side (P3).
- 0DTE contracts ingest and fit (G1) — new capability, real-data validated.

## §2 Global constraints (every agent)

1. **Build/test commands** (from INSIDE the worktree — wrong-tree guard enforces this):
   ```powershell
   Set-Location C:\atx-wt\wt-pg-sota
   & C:\atx-wt\wt-pg-sota\scripts\atx-build.ps1 build atx-vol-tests
   & C:\atx-wt\wt-pg-sota\scripts\atx-build.ps1 -Ctest -R <SuitePattern>
   ```
   Configure is already done by the PM (preset `dev`, bench ON). Do NOT reconfigure. Do NOT touch `C:\atx` (the live tree). ctest patterns with regex metachars: pass via `-Ctest -R` (it invokes ctest directly, no cmd.exe parsing).
2. **TDD:** write/extend the failing test first, then fix, then show green. Use `superpowers:test-driven-development` discipline. Slow-suite tests: tag consistently with existing conventions in `tests/`.
3. **Pre-release, clean-break license:** no compat shims, no dual paths. If an API shape is wrong, change it and fix all call sites (grep whole repo incl. `python/`, `tools/`, `examples/`, `bench/`).
4. `/W4 /WX` is on — warnings are errors. Match surrounding code style; no drive-by refactors outside task scope.
5. **Commit protocol:** conventional commits, scoped to the task, message includes: findings addressed (e.g. "core-review finding 1"), parity class (bit-identical / repin+why / economic budget), counter deltas for perf tasks. End with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
6. **Do not** run benches for gating; record only if asked. Do not push to origin. Do not merge to main — PM handles integration.
7. **Counters:** `include/atx/vol/counters.hpp` — opt-in via `ATX_VOL_COUNTERS` env/compile flag; check existing usage in tests before adding new counter IDs.
8. One task may NOT edit files owned by a concurrently-running task (PM enforces the wave schedule §4; if you find you must touch an unowned file, stop and report back instead).

## §3 Workstreams and tasks

### WS-A — Correctness (core math)

**A1 [CRITICAL — lands first, alone] BAW critical-price Newton derivative sign + convergence contract.**
- Files: `src/american.cpp:97,113` (`put_residual_deriv`/`call_residual_deriv`), `src/simd/american_boundary_avx2_kernel.hpp:218-222` (bit-parity mirror), `src/american.cpp:116-177` (`newton_critical_put/call`).
- Fix: flip the φ-term sign: put `- dq*phim/(q1*v)` → `+ dq*phim/(q1*v)`; call `- dq*phip/(q2*v)` → `+ dq*phip/(q2*v)`. Fix scalar and AVX2 together (kernel replicates the expression for bit-parity).
- Also: `newton_critical_*` gains a converged flag (or iteration count out-param); `baw_american` checks `|f| < tol_scale*K` before using Sx (fallback: keep bisection result — do NOT change the safeguard structure).
- Tests FIRST: (i) FD-parity test of the residual derivative at the review's verified points (K=100,T=0.5,σ=0.25,r=0.05,q=0.02,S=70 put: true ≈ −0.0982; S=130 call: true ≈ +0.121) — must fail before fix; (ii) convergence test: critical-price root-find converges in ≤8 iterations across a (r,q,σ,T) grid; (iii) existing BAW-vs-PDE-oracle stays green.
- Expected fallout (this is a **repin** task): bit-pinned boundary/seed tests move (BAW seed values shift ~1e-4·K); `BoundaryHoist.SeedSpike_SweepCount`-style counts likely DROP — update pins with justification. AVX2 economic-parity suite must stay green (both sides fixed together). Counters: cold-seed Newton iterations before/after in commit message.
- Post-task note for PM (do not implement): A6 QD+-vs-BAW verdict was measured atop this bug; `american_shootout_bench` A/B re-run is a report item, not a ship decision.

**A2 [MED] Intrinsic floor on cached hot path.**
- Files: `src/american.cpp:2164-2191` (`american_price_cached`), `:2193-2217` (blend variant), `:1634` (greeks bundle price).
- Fix: floor served cached price at `max(intrinsic, euro_leg, 0)` matching the cold clamp chain (`al_put_price_from_boundary` :1326-1343). Keep greeks untouched (floor is on the served price only; document the kink at the floor).
- Tests: deep-ITM put beyond `k_log_max` (out-of-box, Clamp policy) asserts `cached >= intrinsic`; in-box property sweep asserts no sub-intrinsic marks across (K/F, T, σ) grid; existing `CachedPrice_MatchesColdAndersenLake` unchanged (in-box, above floor).
- Parity class: economic — only affects marks that were previously BELOW intrinsic (bounded improvement, budget: changes only where old mark < intrinsic).

**A3 [MED] Bracket-clamp the IV cold polish.** `src/american_iv.cpp:444-462`: clamp each polish iterate into `[xl, xh]`; reject polish step if it moves > (few × final tol) away from the rtsafe root. Test: adversarial case with forced warm/cold gap (mock or extreme corner) asserting returned IV ∈ [xl, xh] always. Bit-identical for all quotes where polish stayed in-bracket already.

**A4 [MED] Notional-scaled no-arb tolerances in European IV.** `src/implied_vol.cpp:31-44, 262-269`: replace absolute `1e-15` with a `max(F,K)`-scaled tolerance consistent with `american_iv.cpp:179` (`band_tol = 1e-9*upper + 1e-12` pattern) and the K1 noise floor (`:287-288`). Test: F≈5000 index-scale at-intrinsic quote → clamps to floor instead of `OutOfRange`; tiny-notional (F≈0.5) unchanged behavior.

**A5 [LOW] One-sided rho stencil at regime boundary.** `src/american.cpp:2356-2364, 2494-2495`: when `r - hr` exits the supported regime, use forward one-sided rho stencil (mirror the near-expiry theta treatment). Test: put with `0 < r <= 1e-4`, `q < r - 1e-4` returns a full greeks bundle (currently NotImplemented).

**A6 [LOW] IV floor consistency.** `src/american_iv.cpp:186-190, 303-305, 360-362` + `types.hpp:65`: one floor constant — bracket floor and reported floor unify (decision: floor = `kIvMin = 0.005`; bracket lo becomes kIvMin; roots inside (1e-4, 0.005) no longer representable → returned as kIvMin with the existing at-floor status). Test: monotone quote decay toward intrinsic produces monotone non-increasing IV with no 50x cliff.

**A7 [MED] Static `parallel_for` exception safety.** `include/atx/vol/parallel_for.hpp:105-119`: apply the capture-first-exception/rethrow-after-join pattern from the dynamic overloads (`:148-174`). Test in `tests/parallel_for_test.cpp`: throwing worker in static variant propagates (currently would `std::terminate`); determinism unaffected.

**A8 [MED] Wing-put relative accuracy in AVX2 Black-76 legs.** `src/simd/black76_batch_avx2.cpp:159-161, 232-234`, `src/simd/american_boundary_avx2_kernel.hpp:480-482`: compute put legs from `Φ(−d1), Φ(−d2)` directly (negate args before `norm_cdf_erfc_pd2` — Cody kernel symmetric+accurate for negatives) instead of `1−Φ(d)` complement. Scalar refs (`black76_price`, `euro_put_sk`) already use Φ(−d). Add a **relative-error** wing test (deep-OTM put F=100,K=50,T=0.1,σ=0.2: batch must match scalar to relative 1e-12, not absolute). Fix the "≈1e-16 everywhere" comments. Parity: repin (deep-wing lanes only); value/vega + IV kernels are internally consistent with their scalar refs — leave them, note in comments.

**A9 [LOW batch — one agent] Small correctness cleanups.**
- `src/american.cpp:548-561` `scheme_from_opts`: `n_quadrature < 8` floors to 8 (not silently ACCURATE-default).
- `vol_time.hpp:163`: `kCalendarYearNs` is 365.25d but enum says Calendar365 → set to 365.0d exactly, repin affected tests (pre-release clean break; ~0.07% T shift). Grep all uses first; if repin blast radius exceeds ~a dozen pins, stop and report to PM instead.
- `src/american.cpp:226-233` QD+ doc/sign: reconcile doc vs code against Li (2010) (measurement-only path; pick whichever matches Li, note A6 re-run caveat).
- `src/simd/essvi_batch_avx2.cpp:132-185, 248-279`: mirror w-batch admissibility+nonfinite-k guards in grad/sigma batches + tests.
- `src/simd/greeks_batch.cpp:20`: null-check `price_out` in scalar fallback (parity with AVX2 sink).
- `src/simd/american_greeks_avx2.cpp:106-107`: condition `r − hr > 0` eligibility on `needs.rho`; zero unrequested greek fields in the scalar patch path (contract consistency).
- `detail/vector_math.hpp:44-87`: document `log_pd` domain (positive normal ratio); add `|lnFK| ≥ 708` escape to the input patch masks in the two consumers.
- INFO item: debug assert on carry mismatch in `american_price_cached` vs `baked_r()/baked_q()` (assert only, no behavior change).

### WS-P — Performance (fitter inner loops)

**P1 [HIGH] Fused cached price+vega for IV Newton.** (perf F1 + F8)
- (a) Bit-identical step: new `CorrectionCache`/blend entry `eval_value_and_dsigma`-style API used by a `american_price_and_vega_cached` entry; rtsafe residual+derivative share ONE value traversal (3 → 2). Replace `black76_greeks(...).greeks.vega` with `black76_value_and_vega` in `american.cpp:2547,2578` and `american_iv.cpp:67-85`.
- (b) Fused traversal: sigma is the last collapse axis — use the `clenshaw_d1`-style single-pass value+∂σ (machinery exists for `cheb_clenshaw3d_second_order`, `correction.cpp:769-779` area) → ~1 traversal per Newton step. NOT bit-identical: economic-parity test (|ΔIV| < 1e-12 on a quote grid vs (a) path), plus counter test asserting traversals/Newton-step == 1.
- Files: `src/correction.cpp`/`correction.hpp`, `src/american.cpp:2164-2191, 2532-2560`, `src/american_iv.cpp:205-230, 385-416`.
- Counters: add traversal counter if absent. Commit message: traversals/inversion before/after on a 200-inversion fixture.

**P2 [HIGH, config-flip] Warm-start carry default ON.** `src/deamer.cpp:461-482` + `DeAmOptions` default + fit/backtest config surfaces. Review evidence: converged root unchanged, diagnostics shift < 1e-8, redundant final `deam_pcp_step` skipped. Tests: existing carry/CurveFitCarryFallback suites green; counter test asserting boundary-solves-per-carry-solve drops vs baseline (record ratio). Grep for explicit `warm_start_carry` setters that become redundant.

**P3 [HIGH] Batch the audit reprices per (expiry, side).** `src/deamer.cpp:113-114, 653-670`, `src/calib.cpp:1137,1162`: route audit reprices through `andersen_lake_*_slice_sigma` (8 solves/side/block) or fast preset instead of per-row ACCURATE cold solves. POLICY (PM-decided): acceptable — audit checks IV-inversion consistency, not boundary-path independence; slice-sigma max gap 3.8e-5 (Task 11 gate) ≪ half-spread economic budget. Document this policy in a comment at the audit site. Tests: audit verdicts unchanged on existing fixtures (same pass/fail set); counter: audit boundary solves O(strikes) → O(8)/side.

**P4 [MED] Premium/tip strike-invariant hoists (bit-identical).** (perf F5+F7)
- `src/american.cpp:1093-1115`: per-boundary premium precompute `{b_t, v, dq, dr}[np]` reused across strikes in `andersen_lake_call_slice` (`:1953-1966`), greeks stencils (`:2683-2731`), `boundary_interp.cpp:293-309`. Put-slice boundary rescale is linear in K — share modulo one multiply, verify ULP pins.
- `src/american.cpp:66-73` (`euro_put_sk` exps), `:871-872` (tip d± share `σ√τ`, `log z`), `:960-964` (`eqn_b_NDd` reuse d±).
- Requirement: bit-identical (same op order) wherever pins exist; otherwise repin with justification. Counter: transcendental counts per slice-solve before/after.

**P5 [MED] Ladder T-collapse cached batch.** (perf F4) New batch entry for equal-T cached ladders: pre-collapse T axis per (ladder, cache) into (i,k) plane; hoist per-strike `exp(-rT)`/`exp((r-q)T)`/`sqrt(T)`. Files: `src/correction.cpp` (plane-collapse eval), `src/session.cpp:1916-1917` (`evaluate_ladder`), possibly mirror in `bulk.cpp:162-176`. Economic-parity gate (reorder): |Δprice| < 1e-12·K on grid vs per-strike path. ~8x on correction-eval term.

**P6 [MED] Warm-start chaining on chain driver + public IV batch.** (perf F9) `src/deamer.cpp:641-642` (adjacent-strike seed chaining in `de_americanize_chain`), `src/american_iv.cpp:484-507` (`american_implied_vol_batch` threads warm_start). Bit-identical roots (seed only changes iterations); counter: residual evals per chain before/after.

**P7 [MED] Bench/counter observability rows.** (perf bench-gaps 1-4) Add bench rows: cached-Newton kernel micro (residual+vega evals), de-Am carry solve (`resolve_chain_carry` fixture), correction-cache build. Wire counter emission (`cnt_*`) for each. These rows make F1/F2/F3 wins reportable and future-gateable. Files: `bench/` (follow `bench/README.md` naming + `check_benchmark_names.py`), no production code changes.

### WS-W — Wiring / cleanup

**W1 [MED] Wire Taylor P&L SIMD batch into production.** (wiring 1) Route `PortfolioPricer::pnl_explain_into`/`scatter_pnl_rows` (`src/portfolio_pricer.cpp:1374,1492`) through `pnl_taylor_explain_batch` (validated + benched, parity tests exist). Gates: bit/economic parity on pnl-explain outputs (existing tests + one new end-to-end comparison test), no `/W4` fallout.

**W2 [LOW] Execute the A4-planned Chebyshev deletion.** (wiring 8, simd 10) Delete `src/simd/norm_cdf_cheb.cpp`, `detail::norm_cdf_pd` (`detail/vector_math.hpp:144`), retarget `vector_math_probe_avx2.cpp` + `simd_vector_math_test.cpp` to Cody-erfc, fix `math_mode.hpp:9-13,47-52` FastDeterministic description + `kFastDeterministicPhiBound`, `greeks_batch.hpp:10-12`, `iv_batch_avx2.cpp:9-27` stale headers. CMake: remove deleted TU.

**W3 [LOW] Delete killed research spikes.** (wiring 19) `american.hpp:586` `al_temporal_warm_probe`, `:630` `al_implicit_diff_put_greeks` + implementations in `american.cpp` + their tests (`WarmAcrossTime.*`, `ImplicitDiff.*`). Keep documented seams (`andersen_lake_generic_kernel`, `al_boundary_jn_sweeps_to_converge`, `andersen_lake_seeded`).

**W4 [LOW] Honor ISA override / MathMode across all batch dispatchers.** (simd 2) Rewire `batch.cpp:127,176,242,269`, `simd/black76_batch.cpp:38,49`, `simd/greeks_batch.cpp:52,65`, `simd/essvi_batch.cpp:74-114`, `simd/pnl_batch.cpp:52` from `have_avx2()` to the `use_avx2()` override-aware gate (pattern in `american_boundary_batch.cpp:94-105`). Test: ForceScalar override produces scalar-path results on every batch entry. Note `cpu.hpp:33-34` T13 scope comment — remove it (now done).

### WS-G — Capability gaps

**G1 [BLOCKER-gap, HIGH] True expiry instants + 0DTE ingest.** (gaps 3)
- `src/opra_panel.cpp:483-490`: stamp true expiry instants — 16:00 ET for PM-settled equity options (the entire single-name/ETF universe), hook for 09:30 ET AM-settled index (flag in contract meta; default PM). Remove the same-day (0DTE) drop. Timezone: ET→UTC via existing calendar machinery (`vol_time.hpp` has NYSE calendar; watch DST).
- Audit EVERY consumer of the expiry instant: grep `iso_to_ns`, `ExpiryInputs`, event-vol `count_between`, backtest lifecycle (assignment/expiry PnL), `contract_projection`. Each consumer either already handles intraday instants or gets fixed.
- Tests: unit — OSI date → correct UTC ns for standard Friday PM, quarterly, weekly, half-day session dates 2024-2028; ingest — synthetic 0DTE chain survives to fit with T>0 intraday and T→0 monotone through the session; regression — existing SPY fixture expiries shift by the documented +16h and downstream pins repin (this WILL move fixtures — commit message documents the shift; slow-suite earnings tests re-run).
- Parity: repin (T shifts ~0.8 trading day at the front — IVs move materially at short T; that is the FIX, not a regression). Coordinate with PM before landing: this is the widest-blast-radius task in the sprint.

**G2 [HIGH] Carry sensitivities (dP/dq, dP/dDiv).** (gaps 2)
- `american_greeks_al` gains q-sensitivity via q± solves (symmetric to existing r± machinery, `src/american.cpp:2583-2733`); cached tier gets fixed-carry-consistent dP/dq (document: correction term held fixed, mirrors rho semantics); expose the adjoint European-rung ∂P/∂q that `detail/adjoint_greeks.hpp:32-38` already computes.
- Chain rule to per-event dividend sensitivity: dP/dDiv_i = dP/dF · ∂F/∂Div_i through `hybrid_forward` (`dividend.hpp`/`curve.hpp:156`) — analytic ∂F/∂Div_i for the escrowed blend.
- API: extend `AmericanGreeks` (API break fine) or add `CarryGreeks` struct; wire into `american_greeks_fd` reference for FD-parity tests.
- Tests: FD parity (q-bump) across regime grid; dDiv FD parity through the forward; adjoint-vs-bump consistency.

**G4 [MED] Exercise-boundary public API.** (gaps 5) Expose `exercise_boundary(K,T,r,q,sigma)` (critical price) from retained AL state (`al_boundary_at`, `src/american.cpp:661` internal today) + `assignment_risk` heuristic (deep-ITM call: div vs remaining time value; deep-ITM put: r·K·dt vs time value). Small header addition in `american.hpp`, tests vs internal boundary + textbook limits (B∞, T→0 → K).

**G-DATA [enabler] Real-data fixtures via databento (budget ≤ $100 TOTAL, approved).**
- Pull: (1) one recent SPY 0DTE session slice (validates G1 end-to-end); (2) one high-dividend single name chain near an ex-date (e.g. MO/T/XOM week containing ex-div) for future discrete-div work + G2 dDiv sanity; (3) optional HTB name day (borrow solve stress) if budget remains.
- Use EXISTING loader tooling (grep `tools/` + `atx-vol/data/` + prior `DATABENTO_API_KEY` usage — a loader exists from prior sprints; do not write a new one). Store under the existing fixture conventions (`tests/fixtures/` or `artifact-cache/` — follow how SPY/XOM fixtures landed). Record actual spend in the sprint report. STOP and report if a single pull would exceed $40.

### Deferred to next sprint (documented backlog — do NOT start)
Discrete-dividend American PDE tier (gaps 1 — needs its own sprint: oracle promotion, materiality router, cache design); EventSchedule into PricedSurface serve layer (gaps 4 — archive schema rev); per-expiry rate term structure through carry solve (gaps 6); VolTime theta unit conversion + calendar data feed (gaps 9a); double-continuation regime (gaps 7); AVX2 boundary/greeks ship-flag flips (perf F10 — blocked on quiet-host measurement infra, not code); `implied_vol_batch` AVX2 rewire (R-24 retirement decision stands until the simd finding-7 notional-scaled gate is redesigned); book-level batch API fate (wiring 10).

## §4 Wave schedule & file ownership (PM enforces)

| Wave | Tasks (parallel within wave only if listed together) | Contended files guarded |
|---|---|---|
| 1 | **A1** alone | american.cpp, avx2 kernel |
| 2 | **A2+A5+A9(american.cpp parts)** as one agent; **A3+A6** (american_iv.cpp) ∥ **A4** (implied_vol.cpp) ∥ **A7** (parallel_for) ∥ **A8** (b76 avx2) | american.cpp single-owner per wave |
| 3 | **P1** (correction/american/american_iv) ∥ **P2** (deamer) | |
| 4 | **P3** (deamer+calib) ∥ **P4** (american.cpp+boundary_interp) ∥ **W1** (portfolio_pricer+pnl_batch) | |
| 5 | **G1** (opra_panel+consumers) ∥ **G2** (american greeks+adjoint+dividend) ∥ **W2+W3+W4** (simd cleanup, one agent) | G1 has widest blast radius — lands last in wave |
| 6 | **P5** (session/correction/bulk) ∥ **P6** (deamer+american_iv) ∥ **G4** (american.hpp API) ∥ **P7** (bench) | |
| any | **G-DATA** (tools/data only — no lib code) | independent |

Rules: american.cpp/american.hpp/american_iv.cpp/deamer.cpp are single-owner per wave. A9's non-american.cpp items may be split off if convenient. Wave N+1 starts only when wave N is committed and `ctest -R AtxVol` is green.

## §5 Git tracker (PM updates SHAs)

**Baseline @ 7e42f7c (dev preset):** 1858 tests, 1823 pass, 30 skip, **2 pre-existing FAILs** (not this sprint's debt, do not fix, do not count against tasks): `AllQualityModes/SurfaceV2Qualification.RiskBuildRunsTheModeCarryAndInversionBudgets/{Latency,Balanced}` — test expects max_borrow_pairs 6/12, code implements 5 (inherited from main @ 99f332f). Full suite ≈ 8 min.

| Task | Status | SHA | Notes |
|---|---|---|---|
| plan+reviews committed | done | 7e42f7c | |
| A1 | **done** | 51fc212 | Newton iters 16(exhausted)→mean 6.17/max 10 on 240-grid; JN sweeps mean 21.78→18.99; repins justified in commit body. 2 new known-reds handed to fix-up agent: PricerFitter.LocalRiskRefitPublishesCopyOnWriteGeneration (cache-certification flip), SurfaceV2Provenance.ValidationFallbackAdmissionRecordsTheServedFamily (fixture premise broken at HEAD post-b118439 — needs injected-surface redesign, not S3 re-tune). A6 QD+/BAW A/B re-run = report item. |
| A2/A5/A9 | **done** | a9890c7, 57a2e2d, 6ca2d99 | A2 floor: fixture fingerprints bit-unchanged (no sub-intrinsic marks in fixtures). A5 one-sided rho at regime boundary. A9 batch: quad floor, QD+ doc-vs-code reconciled, essvi guards, null-check, GreekNeeds rho-gate, log_pd escape, carry debug-assert ([[maybe_unused]] tol for NDEBUG). Calendar365 decision: see agent report. |
| P2 | **done** | 0b47315 | warm-start carry default ON. Boundary solves/slice 474→139 (0.293 ratio, ~3.4×). Δborrow 5.9e-10 ≪ 1e-8 premise. New counter gate test. |
| fixup r2 (Provenance) | **done** | f99d796 | Fixture on discrete InsufficientExpiryCoverage gate (genuine calendar arb, QP-infeasible 4m lift, 2-of-3 < floor=1.0). Margin ~10⁴× drift scale; 5/5 deterministic; reason-code asserted. Geometric gates proven capped at ~2-4e-10 for validly-built ConvexDense. |
| A3/A6 | **done** | 46a9ae4, 3c43c90 | polish bracket-clamped + traced seam; floor unified at kIvMin=0.005; A1 vega-gate retained (measured: K=110/120 T=2 σ=0.1 puts sit at inverter's fundamental vega=3e-3 floor ~1.2e-5 rel, not polish artifact) |
| A4 | **done** | b5baa18 | tolerances scaled kIvResidNoiseFloor·ε·max(F,K); index-scale at-intrinsic now clamps |
| A7 | **done** | f84e72a | static parallel_for capture/rethrow; no more terminate-on-throw |
| A8 | **done** | 6d64a4b | put legs via Φ(−d) direct; deep-OTM rel err 1.0 → 1.7e-13; no pins moved |
| fixup (A1 known-reds) | **done** | 231e798 | test-only. PricerFitter: post-A1 cache REUSE correct (Δlog-F 1.8e-11 ≪ 1e-5 gate; old refusal = buggy-seed accident). Provenance: redesigned onto family-neutral convexity admission gate (was riding 1.3e-8 vs 1.42e-8 butterfly-kink knife edge) |
| P1 | **done** | 6965f85 (a), d0682b2 (b) | Traversals/Newton-step 3→2→1 (counter-asserted); 200-inversion fixture 2353→1343 ClenshawSweeps (~43% fewer); stage a bit-identical + F8 vega; stage b max|ΔIV| 5.6e-15 (197/198 bit-identical). Flagged suspect red: QualifiedCorpus.QuarantinedFitStaysReportedAndCannotLeakIntoADateArchive (triage dispatched). |
| P3 | **done** | 8f32798 | Audit AL solves per (expiry,side) 24→8 via slice-sigma (same ACCURATE premium quad); identical audit verdicts; residual shift ≤3.8e-5. Backlog: prepared_fitting.cpp audit site (default-off) left per-row. |
| QC triage | **done** | 09640c7 | Root cause: latent FP bug, NOT fit regression. XOM board 100% in-band → in-band vs total vega sums grouped differently → oos_vw=1.0+2ULP trips strict guards (select_candidate_index oos_vw>1.0; corpus.cpp:1635 in_band>total) → NotFound → empty oos_score. Debug red / Release green = FMA contraction straddles 1.0 exactly. Exposer ~8f32798 (rewrites de-Am reprice vega); fix commit-independent. Fix: clamp in_band=min(win,total), oos_vw derived — definitional subset invariant, bit-identical no-op where invariant holds. 54 affected-suite green + invariant pin added. |
| P4 | **done** | 6fc48da (F7), 409607a (F5) | Bit-identical PROVEN (reverse-apply → byte-identical frame hash). Counter gates: slice ExpCalls strike-count-independent (n=1→624, n=8→624, was 1296); per-strike premium exps N·96→96. AlPremiumCache separate ~2.6KB object (trap 7 honored). price_internal_put not wired (per-strike sigma, no reuse — documented). Fingerprint red root-caused as PRE-EXISTING: SSE2-captured golden vs FMA under /arch:AVX2, exact-hash EXPECT_EQ lacks ISA guard; fails rel-avx2 only, since A1 repin. Fix dispatched separately. |
| P5 | **done** | 49b797e | american_price_cached_ladder: collapse_T_plane once per endpoint + 2D eval_plane per strike (~n_T× less Clenshaw work). evaluate_ladder AND fair_value_ladder wired (CachedPriceBatch mirrors ColdPriceBatch flush). Economic parity 2.84e-17/K (bit-identity impossible: T is middle collapse axis, reorder inherent). 2 justified repins EXPECT_DOUBLE_EQ→1e-12·K. bulk.cpp NOT wired (per-lane resolve, unfloored PriceOnly, dT partial needed — documented in-file; F12 LOW). Counter gate CorrectionCache.LadderBatchClenshawSweepCountIsStrikeCountIndependent asserts **ClenshawSweeps** (scalar=n_strikes → ladder=1, strike-count-INDEPENDENT), NOT ExpCalls — the ExpCalls 624/1296 figure is P4's original measurement, not this gate. **Counters-ON sweep CONFIRMED @443dfcf: scalar=31, ladder=1, gate green, GTEST_SKIP canary did not fire (preset genuinely defines ATX_VOL_COUNTERS).** 110+192 pass. SpyDispersionPnl PASSES clean rel-avx2 (4.4e-15). |
| P6 | **done** | fa37848 | Fork: BATCH SHIPS, CHAIN DELETED. Batch `american_implied_vol_batch` gains opt-in warm_start_chain (default false, zero blast radius), 0.15 log-moneyness guard; counter 65→42 IvNewtonIters (−35%) on 11-lane cached ladder. de_americanize_chain warm-chaining measured-and-rejected: cold 147→147, cached (DeAmOptions.caches) 137→140 WORSE — chain inverts only OTM legs where European seed already in pricer warm-reseed band; deleted entirely per no-dead-plumbing ruling. CORRECTION: interim 1.2e-11 drift figure was truncated-output artifact; true cold-AL max 1.17e-6 (2-step polish american_iv.cpp:512-553 seed-dependent). Path-appropriate parity pins: cached map <1e-9 (production hot path, PM mandate holds), cold-AL <1e-5 (matches shipped WarmStartResultInvariantToSeed 1e-6 + calib.cpp 1e-4 precedent; PM accepted). ClenshawSweeps gate wired for counters-ON sweep (skips counters-OFF). **Counters-ON sweep CONFIRMED @443dfcf: gated block fired (cold=87/warm=64 ClenshawSweeps, warm<cold), IvNewtonIters cold=65/warm=42 exact; FusedCachedInversionTraversalCount 1343 sweeps/983 Newton exact.** 55 pass/0 fail, /W4 /WX clean. |
| P7 | **done** | 32161ad | 5 bench rows (cached-Newton IV, carry legacy/warm A/B, correction build put/call) + name-coverage ctest. Always-on sl_* ledger columns cite wins from shipping binary: 474→139 carry solves (P2), 96 build solves, 983 IV iters/200. cnt_* columns light up under counters build. **Counters-ON sweep CONFIRMED @443dfcf: bench builds+runs under ATX_VOL_COUNTERS=ON (-DATX_BUILD_BENCH=ON), cnt_clenshaw_sweeps=1343, sl_iv_newton_iters=983, carry legacy/warm boundary solves 474→139, build put/call 96 — all exact vs tracker.** Fixture fix: call-side build carry q=0.02 (q≤0 short-circuits American call). NOTE (deferred tooling): atx-build.ps1 -Ctest/configure/build hardcode $RepoRoot\build and cannot target build-counters; sweep used the script's raw cmake pass-through + direct `ctest --test-dir build-counters` (PowerShell, no cmd.exe → trap-2 safe). Script gap, out of sprint scope. |
| FP-fix | **done** | 7d56ef0 | prepared_portfolio_test.cpp dual-golden keyed on kFmaContraction (__FMA__): SSE2 pin 718570745730299145 preserved exactly, FMA pin 8754310291975640041 added. golden_close rejected — whole-frame hash has no tolerance band. PreparedPortfolio 7/7 green on rel-avx2. |
| W1 | **done** | 79abd68 | scatter_pnl_rows → ONE serial pnl_taylor_explain_batch (qty=nullptr, weight at call site preserves bit-identical unexplained). Scalar-tail/non-AVX2 bit-identical; AVX2 2nd-order terms within 1e-7/1e-12 kernel bound. Grouped-vs-ungrouped repin justified (SIMD routing, not math). 4 new SimdPnlWiring tests. have_avx2() gate left for W4. |
| W2 | **done** | fc3f10c (+deletions swept into 750a465) | norm_cdf_pd AND norm_cdf_pd2 deleted (zero live callers — bit-identical for all shipped paths). Probe/tests retargeted to Cody-erfc, full-range Φ gate, kFastDeterministicPhiBound 5e-11→1e-14 (measured 1.665e-15). Stale wing-patch docs scrubbed. 60/60 affected green. Leftover: 2 historical kNormCdfWing comment tokens (black76_batch_avx2.cpp:77, greeks_batch_avx2.cpp:85) — cosmetic, W4 may sweep. |
| W3 | **done** | 19e4c78 | Spikes + file-static helpers (al_solve_put_counted, lu_solve_dense) + tests deleted, 792 deletions/6 files. Documented seams + al_put_boundary_residual (adjoint links it) kept. Grep-clean in code; archival docs untouched. 79 pass/0 fail. |
| W4 | **done** | 5619b6a | 14 dispatchers have_avx2()→use_avx2() across batch.cpp, black76/greeks/essvi/pnl_batch.cpp. New SimdIsaOverride suite 10/10: ForceScalar bit-identical to scalar ref on every entry, with teeth (Auto differs on b76 family). iv_batch unconditionally scalar (no gate; stale NOTE removed). Auto path numerically no-op. 99/99 + 128/129 (sole red = pre-existing fingerprint pin). |
| G1 | **done** | 798dcf0 | Design B (PM-confirmed): true 16:00-ET PM / 09:30-ET AM settlement instant stamped on the REAL OPRA path only via QuoteRow::expiry_ns; synthetic/hand frames fall back to legacy midnight ⇒ BIT-IDENTICAL (fit-recovers-truth oracle unmoved). New surface: vol_time SettlementSession{Pm,Am}+settlement_instant_ns; data expiry_instant_ns+QuoteRow{settle,expiry_ns}; opra_panel stamps + 0DTE drop removed (filter now measures T vs true instant). Fixes OSI-date→midnight ~0.8-trading-day front-T understatement + same-day-ex-div now included on hybrid_forward. 4 repins (opra_panel_test.cpp) recomputed from pm_year_fraction/expiry_instant_ns — no golden/DCT/CRC/archive moved, source_fingerprint untouched (trap #4 satisfied). Consumer audit: EssviParams/SviParams expiry_ns + event-vol count_between + escrowed hybrid_forward cutoff consume opaque int64 correctly; backtest/contract_projection/sr_tenor_grid/listed_opra decoupled (listed_opra already 20:00Z — now matches). Python bindings: no changed symbol (trap 9 clear). Full+slow suite 0 G1-attributable reds; new real-data G1ZeroDteSessionSweep green (SPY 2026-07-17 ×4: 249 strikes, T 6.42h→5min monotone, all fit). |
| G2 | **done** | 55cd3ca | New CarryGreeks{price, dP_dq, q_one_sided} struct (AmericanGreeks NOT widened — 69-file ecosystem, no API break; python additive). No-spot-stencil insight ⇒ carry_al = carry_fd BIT-IDENTICAL both sides on 324-pt regime grid (no homogeneity rescale needed). Cached dP/dq=−T·F·D fixed-carry (rel 2.7e-16 vs −T·S·delta); adjoint ∂P/∂q exposed (4.5e-9 vs bump); analytic ∂F/∂Div escrowed-blend Jacobian (8.6e-12 vs bump); dP/dDiv composed via q_eff bridge, decoupled from dividend.hpp. Focused 3-boundary function (trap 7 ok). 151 pass/0 fail. Deep-ITM negative dP/dq near boundary = shipped-pricer FD kink, sign assertion gated to meaningful time value. Watch item SpyDispersionPnl: not run (outside suites). |
| G2/G4 watch | **resolved** | | SpyDispersionPnl.DailyDeltaHedgeBandsNetDelta PASSED in G1's isolated re-run on the quiet host (798dcf0); the single earlier flag was concurrent-build contention, not a regression. PM re-confirms on the clean-tree final gate. |
| G4 | **done** | 3869ec7 | exercise_boundary(K,T,σ,r,q,side): put via al_boundary_at, call via McDonald-Schroder K²/B_put(swap); European regime → OutOfRange sentinel (distinct from double-cont NotImplemented — glance at final review), degenerate → al_xmax_put analytic. AssignmentRisk{at_risk,margin,carry_benefit,time_value} screen (call q·S·T, put r·K·T vs time value). Limit table green: perpetual 7e-5, T→0 exact both regimes (B(0+)=K·min(1,r/q) — task prose had labels reversed, derived from code), symmetry <1e-12. 156 pass/0 fail; bindings additive. |
| G-DATA | **done** | | spend: $0 total. SPY 2026-07-17 0DTE session slices in fixtures (G1 validated end-to-end: 249 strikes, T 6.42h→5min monotone, all boards fit). |

## §6 Traps (hard-won, read before building)

1. **Wrong-tree guard:** `atx-build.ps1` refuses unless your shell cwd is inside `C:\atx-wt\wt-pg-sota`. `Set-Location` first. Never touch `C:\atx`.
2. **ctest regex:** use `atx-build.ps1 -Ctest -R <pat>` (direct ctest, no cmd.exe metachar mangling).
3. **MT.exe:** fresh build dirs need the Windows SDK bin on PATH — atx-build.ps1 handles it; do not hand-roll cmake calls.
4. **Bit-pinned tests:** the suite pins exact doubles in several places (boundary nodes, DCT coefficients, archive CRCs). A repin without a stated reason in the commit message is a review reject. Never repin to make a wrong number pass — justify from the math.
5. **AVX2 ↔ scalar bit-parity pairs:** BAW seed expressions are mirrored scalar↔AVX2 (A1), and several kernels deliberately forbid FMA for parity. When touching one side, touch both, and check the economic-parity suites (`simd_american_test`, `american_batch_test` ISA routing).
6. **Counters are the perf gate.** Wall-clock on this host does not hold CV ≤ 5%; a perf claim without a counter delta is not accepted.
7. **`american_greeks_fd` stacks 7 workspaces** — stack budget matters; no large per-workspace additions on bundle paths (P6-style tradeoffs were already rejected once; see review-perf F6 gating note).
8. **Slow suite:** earnings/repro/backtest tests are slow-tagged; run them when your task touches their scope (G1 does), not by default.
9. **python/ bindings are a real consumer** — API breaks must update `atx-vol/python/src/bindings/*` too (it is a standalone CMake project; at minimum grep it and fix signatures; PM smoke-builds it at sprint end).
10. **Windows PowerShell 5.1**: no `&&`/`||` in commands; the Bash tool exists for POSIX syntax but builds go through atx-build.ps1.

## §7 Sprint acceptance

1. All waves committed, full `ctest -R AtxVol` + slow-suite green at HEAD.
2. Counter-verified wins recorded per perf task (§1 expectations).
3. G1 validated on real 0DTE data (G-DATA fixture) end-to-end: ingest → carry solve → fit → serve, front expiry included.
4. Sprint report appended to this doc: findings fixed, counters table, repin log, bench best-of-3 (informational), spend, deferred-backlog confirmation.
5. PM merges `feat/pg-sota` → local `main` after final whole-branch review (no push to origin).
