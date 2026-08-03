# Historical VaR Engine: Cross-Sectional Inverse-Delta Performance Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Every task is self-contained: quote the **Global Constraints** block verbatim into each implementer prompt together with the task text.

**Goal:** Make the historical-simulation VaR replay for American equity options in `atx-vol` meet the first hard milestone: **<= 1 core-second per scenario** on the SP100 benchmark fixture, without weakening any correctness gate.

**Current vs target (SP100 fixture, 106 scenarios, one-shot Release, benchmark host):**

| Route | t8 wall | core-s/scenario | Status |
|---|---:|---:|---|
| Direct cold root + cold marks | 109.127 s | 8.24 | correct baseline, slow |
| Fast screen, cold-confirmed root + cold marks (`FastScreenColdConfirm`) | 37.695 s | 2.85 | current accepted route |
| Grouped batch-root wrapper around scalar solves | 57.717 s | 4.36 | **rejected — slower** |
| **Target: cross-sectional inverse-delta** | **<= ~13.25 s** | **<= 1.0** | this plan |

At ideal scaling the t8 wall target is ~13.25 s; the t1 wall target is ~106 s (the one-worker run tests the core-second budget directly — thread scaling cannot compensate for a slow single-scenario algorithm).

**Authoritative context:**
- Status doc (economics, accepted/rejected routes, recommended design): `C:\atx-wt\var\atx-vol\docs\historical-var-engine-status.md`
- Coding/build standard: `C:\atx\.agents\cpp\agent.md`
- Worktree: `C:\atx-wt\var`, branch `var`. All builds/tests run from this worktree root.

**Architecture (grounded in the code as found):** The remaining cost is per-option scalar inverse-delta root solving: `evaluate_scenario_batched` (src/var.cpp) already batches final base/shifted price marks through `SurfaceRef::evaluate_batch` (AVX2 `american_price_batch_resolved` lanes), but strike resolution still calls `resolve_var_contract` → `project_option_contract` → `solve_american_delta_screened` (src/contract_projection.cpp) **once per unique option per scenario**, each performing 4–15 scalar American delta evaluations. The replacement: per (scenario × underlier group), one Black-style inverse-delta **seed vector**, then a small fixed number of **laned cold American delta passes** via `evaluate_batch(EvalField::FirstOrder, analytic=true, GreekNeeds{vega=false,rho=false,charm=false}, QueryExecution::ColdReference)` — this rides the existing 128-chunk 4-lane AVX2 kernels `simd::american_put_greeks_batch` / `american_call_greeks_batch` (src/laned_greek_run.hpp, ship gates `kShipAvx2Boundary`/`kShipAvx2Greeks` both `true`), where the K4 reduced tier costs **one boundary solve per lane** — with vectorized Newton/secant correction between passes, half-tolerance acceptance, and a scalar robust fallback for the rare unconverged tail. Both the aggregate path and the retained-leg audit path consume the **same** resolved strikes, so the existing 1e-9 aggregate-vs-retained-leg parity gate in `bench/var_bench.cpp` keeps holding; the independent cold oracle is enforced by new tests that confirm every accepted strike against scalar `PricedSurface::delta(..., QueryExecution::ColdReference)`.

---

## Global Constraints

```text
CORRECTNESS GATES (never weakened):
1. Cold-confirm: every accepted strike must satisfy the cold American delta
   tolerance — abs( abs(cold delta(K)) - target_abs_delta ) <= delta_tolerance
   (default 1.0e-7) where the oracle is scalar
   PricedSurface::delta(K, T, side, QueryExecution::ColdReference).
2. Aggregate P&L must match the retained-leg cold oracle within admitted
   economic error. The SP100 bench fixture gate stays at
   kMaxAggregateRelativeValueError = 1.0e-9 / kMaxAggregateRelativeDeltaError
   = 1.0e-9 (bench/var_bench.cpp) for same-route aggregate-vs-retained-leg;
   cross-route (new engine vs Direct scalar oracle) unit parity is gated at
   1.0e-5 * value_scale (strike slack within delta tolerance is admitted).
3. Cold marks ONLY for valuation. Prepared/fast marks are NOT an admitted
   valuation route (measured 4.8% aggregate value error — rejected). Every
   pricing/delta call the new engine makes passes
   QueryExecution::ColdReference explicitly.
4. Scenarios are mathematically independent: no state, seed, or restruck
   contract carries from one date to another. Thread-count invariance is
   bit-exact and pinned by Var.ReplayIsBitInvariantAcrossThreadCountsAnd-
   LegOutputIsOptional; the new route must satisfy the same property.
5. Rejected routes stay rejected: no prepared/fast marks for valuation, no
   grouped wrapper around per-option scalar root solves.

BUILD/TEST (from worktree root C:\atx-wt\var; use `powershell`, never `pwsh`;
NEVER raw cmake/ninja):
  powershell scripts\atx-build.ps1 configure                # dev preset -> build/
  powershell scripts\atx-build.ps1 build atx-vol-tests      # always target-scoped
  powershell scripts\atx-build.ps1 check atx-vol\src\var.cpp  # single-TU compile
  powershell scripts\atx-build.ps1 -Ctest -R "^Var\."         # focused suites --
  powershell scripts\atx-build.ps1 -Ctest -R "^ContractProjection\."
    (separate -R invocations, never `|` alternation)
  powershell scripts\atx-build.ps1 configure -Preset hygiene  # PCH-OFF include gate
  powershell scripts\atx-build.ps1 build atx-vol-tests -Preset hygiene
  powershell scripts\atx-build.ps1 configure -Preset rel -Bench       # benchmarks
  powershell scripts\atx-build.ps1 build atx-vol-projection-bench -Preset rel -Jobs 12
  powershell scripts\atx-build.ps1 configure -Preset rel-avx2 -Bench
  powershell scripts\atx-build.ps1 build atx-vol-projection-bench -Preset rel-avx2 -Jobs 12
Bench exes land in build-rel\bin\ and build-rel-avx2\bin\ respectively.

STYLE GATES: C++20, clang-cl 18, /W4 /permissive- /WX clean. clang-format
enforced (`clang-format --dry-run -Werror <touched files>` must pass; run
`clang-format -i` before claiming done). Hygiene preset must compile the
touched TUs include-clean (PCH masks missing includes). Do NOT run clang-tidy
(repo disables it deliberately). No UB, bounded loops, no allocation in the
per-scenario steady state (reuse scratch), Rule of Zero, Result<T>/Status
error model — no exceptions for control flow.

TDD IS MANDATORY: write the failing test first, watch it fail for the right
reason, then implement. One behavior per TEST, Subject_Condition_Expected
naming, boundaries + error paths covered.

BENCHMARKS: single-shot while iterating —
  $env:ATX_SP100_SURFACE_DB='C:\atx-scratch\surface-db\sp100-2026'
  $env:ATX_VAR_BENCH_SINGLE_SHOT='1'
Final citable numbers: unset ATX_VAR_BENCH_SINGLE_SHOT so bench_util.hpp
apply_common applies (>=0.5 s warm-up, 5 repetitions, p95 + CV statistics);
run each of t1/t4/t8/t16; a case is citable only when its cv aggregate is
<= 5% (compare_baseline.py NOISY threshold). Quiet host: lease the P-cores to
one bench at a time (scripts/atx-build.ps1 header, P-CORE BENCH-LEASE note).
```

---

## File Map (all under `C:\atx-wt\var` unless absolute)

- `atx-vol/include/atx/vol/contract_projection.hpp` + `atx-vol/src/contract_projection.cpp` — new policy enumerator, public expiry/fingerprint helpers, the cross-sectional solver `solve_american_delta_batch` + `AmericanDeltaBatchScratch`.
- `atx-vol/include/atx/vol/var.hpp` + `atx-vol/src/var.cpp` — policy validation, group resolution via the batch solver in `evaluate_scenario_batched` and `evaluate_scenario`, scratch extension, default-policy flip.
- `atx-vol/tests/contract_projection_test.cpp`, `atx-vol/tests/var_test.cpp` — all new tests (files already in the `atx-vol-tests` target via tests/CMakeLists.txt; no CMake edits needed).
- `atx-vol/bench/var_bench.cpp` — third route registration `cross_cold`.
- `atx-vol/bench/plot_var_cumulative_pnl.py` — unchanged; rerun for the final PNG.
- `atx-vol/docs/historical-var-engine-status.md` — final status update.
- Artifacts: `C:\atx\artifacts\var\...` (absolute; shared with the main tree).

---

### Task 1: Re-run the deferred baseline gates and record the reference benchmark number

**Context:** The status doc (`atx-vol/docs/historical-var-engine-status.md`, "Validation state") lists gates that were deferred after the rejected batch-root implementation was removed. Re-run them on the committed `var`-branch baseline BEFORE any new code, and record the single-shot screened number as the reference point on this host.

**Files:** none modified (pure verification). Baseline files to gate: `atx-vol/src/var.cpp`, `atx-vol/include/atx/vol/var.hpp`, `atx-vol/src/contract_projection.cpp`, `atx-vol/include/atx/vol/contract_projection.hpp`, `atx-vol/tests/var_test.cpp`, `atx-vol/tests/contract_projection_test.cpp`, `atx-vol/bench/var_bench.cpp`.

**Steps:**
- [ ] `powershell scripts\atx-build.ps1 configure` then `powershell scripts\atx-build.ps1 build atx-vol-tests`
- [ ] `powershell scripts\atx-build.ps1 -Ctest -R "^Var\."` — all pass
- [ ] `powershell scripts\atx-build.ps1 -Ctest -R "^ContractProjection\."` — all pass
- [ ] `clang-format --dry-run -Werror` over the seven files listed above — clean
- [ ] `powershell scripts\atx-build.ps1 configure -Preset hygiene` then `powershell scripts\atx-build.ps1 build atx-vol-tests -Preset hygiene` — include-clean, /WX clean
- [ ] `powershell scripts\atx-build.ps1 configure -Preset rel -Bench` then `powershell scripts\atx-build.ps1 build atx-vol-projection-bench -Preset rel -Jobs 12`
- [ ] Single-shot screened reference run:
  ```powershell
  $env:ATX_SP100_SURFACE_DB='C:\atx-scratch\surface-db\sp100-2026'
  $env:ATX_VAR_BENCH_SINGLE_SHOT='1'
  .\build-rel\bin\atx-vol-projection-bench.exe `
    --benchmark_filter='^var/prepared/sp100_dispersion_terminal/ytd/thousands/screened_cold/t8/' `
    --benchmark_out='C:\atx\artifacts\var\sp100_dispersion_ytd_benchmark_screened_baseline.json' `
    --benchmark_out_format=json
  ```
- [ ] Record the t8 wall seconds and `scenarios_per_s` counter in the task report (expected in the neighborhood of 37.7 s / 2.8 scenarios/s; a materially different number means the host is not comparable — say so).

**Acceptance:** every gate green; baseline JSON written; reference number recorded. Any test failure on the untouched baseline is a stop-the-line finding to report, not to fix silently.

---

### Task 2: Contract-projection seams — `CrossSectionalColdConfirm` enumerator + public expiry/fingerprint helpers

**Context:** The cross-sectional engine lives at the VaR group level but needs three seams from `contract_projection`: a policy enumerator to select it, the maturity resolver (currently the file-static `resolve_expiry` in `atx-vol/src/contract_projection.cpp`), and the definition fingerprint (currently file-static `definition_fingerprint`). Promote the latter two to public API and add the enumerator. TDD: tests first.

**Files:**
- Modify: `atx-vol/include/atx/vol/contract_projection.hpp`, `atx-vol/src/contract_projection.cpp`
- Test: `atx-vol/tests/contract_projection_test.cpp`

**Interfaces (exact):**
```cpp
// contract_projection.hpp
enum class OptionDeltaSolvePolicy : std::uint8_t {
  Direct = 0,
  FastScreenColdConfirm = 1,
  // Cross-sectional inverse-delta with cold confirm. In the scalar
  // project_option_contract entry point this behaves exactly as
  // FastScreenColdConfirm (a batch of one gains nothing); batch consumers
  // (PreparedVarPortfolio) select the cross-sectional group solver.
  CrossSectionalColdConfirm = 2,
};

// Public form of the existing static resolve_expiry: absolute expiry timestamp
// for `spec` anchored at `valuation_ts_ns`. Same validation/errors.
[[nodiscard]] Result<std::int64_t>
resolve_projected_expiry(std::int64_t valuation_ts_ns, const ProjectedMaturitySpec &spec);

// Public form of the existing static definition_fingerprint (nonzero result).
[[nodiscard]] std::uint64_t
projected_definition_fingerprint(const ProjectedOptionDefinition &definition);
```
Implementation: rename/move the two static helpers out of the anonymous namespace (keep single definitions — the internal callers in `project_option_contract` call the public functions). Extend `valid_delta_solve_policy` to accept the new enumerator, and route `CrossSectionalColdConfirm` in `project_option_contract`'s `ProjectedStrikeKind::Delta` switch arm to `solve_american_delta_screened` (identical to `FastScreenColdConfirm`).

**Tests (write first, in `contract_projection_test.cpp`, reusing the file's `make_surface`/`spec` helpers):**
- `TEST(ContractProjection, CrossSectionalPolicyResolvesLikeFastScreenInScalarEntryPoint)` — `project_option_contract` with `delta_solve_policy = CrossSectionalColdConfirm` on a `RepresentativeFast`-prepared surface succeeds and returns a bit-identical `ProjectedOption` to the same call with `FastScreenColdConfirm` (assert `EXPECT_EQ(*a, *b)`), and the cold delta at the resolved strike is within `delta_tolerance` of the target (mirror `FastDeltaScreenIsAlwaysColdConfirmed`).
- `TEST(ContractProjection, ResolveProjectedExpiryMatchesProjectedDefinitionExpiry)` — for `months(3)`, `days(30)`, and `years(0.25)` specs, `resolve_projected_expiry(now, spec.maturity)` equals `project_option_contract(...)->definition.expiry_ts_ns`; invalid inputs (`months(0)`, `valuation_ts_ns <= 0`, absolute expiry not after valuation) return errors.
- `TEST(ContractProjection, ProjectedDefinitionFingerprintHelperMatchesProjection)` — helper applied to a projected `definition` reproduces `definition.fingerprint` and is nonzero; changing `K` changes the fingerprint.

**Verification:**
- [ ] `powershell scripts\atx-build.ps1 check atx-vol\src\contract_projection.cpp`
- [ ] `powershell scripts\atx-build.ps1 build atx-vol-tests` then `powershell scripts\atx-build.ps1 -Ctest -R "^ContractProjection\."`
- [ ] `clang-format --dry-run -Werror atx-vol\include\atx\vol\contract_projection.hpp atx-vol\src\contract_projection.cpp atx-vol\tests\contract_projection_test.cpp`

**Acceptance:** new tests green; all pre-existing `ContractProjection.*` tests still green (notably `PreparedBatchIsInputOrderedAndThreadInvariant` and `InvalidSpecsFailWithoutInventingContracts`); no behavior change for `Direct`/`FastScreenColdConfirm`.

---

### Task 3: `solve_american_delta_batch` — the cross-sectional solver core (seed + laned cold delta passes + Newton/secant)

**Context:** This is the heart of the plan. One surface, N rows of (T, side, target delta) → N strikes, each cold-confirmed. All heavy passes ride `SurfaceRef::evaluate_batch` with `EvalField::FirstOrder`, `analytic=true`, `GreekNeeds{.vega=false,.rho=false,.charm=false}`, `simd::SimdIsa::Auto`, `QueryExecution::ColdReference` — this satisfies `detail::laned_greek_route_selected` (FirstOrder bundle, no Delta/Vega selective bits, AndersenLake, AVX2 ship gates ON), so each pass costs ONE boundary solve per option, 4 lanes per AVX2 pack, on both `PricedSurface` and mapped `PricedSurfaceView` (shared `laned_greek_run`). Delta is read from `out.greeks[i].delta`. Do NOT use `EvalField::Delta` — the selective bit forces the scalar per-entry loop (`laned_greek_route_selected` requires `!want_delta`).

**Files:**
- Modify: `atx-vol/include/atx/vol/contract_projection.hpp`, `atx-vol/src/contract_projection.cpp`
- Test: `atx-vol/tests/contract_projection_test.cpp`

**Interfaces (exact):**
```cpp
// contract_projection.hpp
// Reusable, allocation-amortized workspace for solve_american_delta_batch.
// resize(n) grows all columns; steady-state reuse allocates nothing.
struct AmericanDeltaBatchScratch {
  std::vector<double> k_log{};        // current candidate log-moneyness per row
  std::vector<double> strike{};       // F(T) * exp(k_log)
  std::vector<double> residual{};     // |delta| - target at current candidate
  std::vector<double> prev_k{};       // previous point for the secant update
  std::vector<double> prev_residual{};
  std::vector<double> forward{};      // F(T) per row (seed-time resolution)
  std::vector<double> sigma{};        // smile-refreshed seed vol per row
  std::vector<double> signed_d1{};    // Black seed d1 (slope reuse, pass 1)
  std::vector<double> iv{};           // evaluate_batch iv column
  std::vector<double> price{};        // evaluate_batch price column
  std::vector<AmericanGreeks> greeks{};
  std::vector<Status> pass_status{};
  std::vector<std::uint32_t> active{};      // stable-ordered unconverged row ids
  std::vector<double> active_strike{}, active_t{};  // compacted pass inputs
  std::vector<Side> active_side{};
  void resize(std::size_t n);
};

// Cross-sectional inverse-delta solve on ONE surface (owned or view). For each
// row i: find strike_out[i] with | |cold American delta| - target_abs_delta[i] |
// <= tolerance, achieved via a Black-style inverse-delta seed, laned cold
// American delta passes (evaluate_batch FirstOrder + reduced GreekNeeds), a
// vectorized Newton/secant correction, and a robust scalar fallback for the
// unconverged tail (Task 4). Internal batch acceptance uses tolerance/2 so the
// scalar cold oracle holds at the full tolerance despite the documented
// laned-vs-scalar kernel gap. Spans all length n; structural violations
// (length mismatch, invalid tolerance, empty batch) fail the call; per-row
// market/convergence failures land in row_status_out and never invent strikes.
// Deterministic: fixed row order, batch-composition-invariant kernels, no
// cross-call state. Thread-safe for concurrent calls on distinct scratch.
[[nodiscard]] Status solve_american_delta_batch(
    const SurfaceRef &surface, std::span<const double> T, std::span<const Side> side,
    std::span<const double> target_abs_delta, double tolerance,
    AmericanDeltaBatchScratch &scratch, std::span<double> strike_out,
    std::span<double> achieved_delta_out, std::span<std::uint16_t> evaluations_out,
    std::span<Status> row_status_out);
```

**Algorithm (implement in `contract_projection.cpp`, reusing the anonymous-namespace `inverse_normal_cdf` and mirroring `solve_american_delta`'s seed exactly):**
1. Validate: `n = T.size()`; all spans size n; `target in (0,1)` per row (row failure, not batch failure); tolerance finite in `(0, 1e-3]` (batch failure otherwise, same predicate as the scalar solver).
2. Seed (scalar loop, no boundary solves — curve reads only): per row, `forward = surface.forward_at(T)`, `sigma = surface.iv(forward, T)`, `carry_discount = exp(-surface.q_eff_at(T) * T)`, `probability = clamp(target / carry_discount, 1e-8, 1-1e-8)`, `signed_d1 = side==Call ? inverse_normal_cdf(p) : -inverse_normal_cdf(p)`, then the same two smile-refresh iterations as `solve_american_delta` lines "for (int iteration = 0; iteration < 2; ...)": `seed_k = -signed_d1*sigma*sqrt(T) + 0.5*sigma*sigma*T`, refresh `sigma` from `surface.iv(forward*exp(seed_k), T)` when finite-positive. Rows with non-finite forward/sigma → row status `Err(Unavailable)` immediately (excluded from all passes).
3. Pass 0 (all seeded rows): one `evaluate_batch` as described in Context; `residual[i] = fabs(greeks[i].delta) - target[i]`. Rows whose `pass_status` is Err or delta non-finite → route to the scalar-fallback list (Task 4). Rows with `fabs(residual) <= 0.5 * tolerance` converge (freeze `strike/achieved_delta`).
4. Pass 1 (Newton, closed-form Black slope — same formula as the scalar solver): `slope_magnitude = carry_discount * inv_sqrt_two_pi * exp(-0.5*signed_d1*signed_d1) / (sigma * sqrt_t)`, `slope = side==Call ? -slope_magnitude : slope_magnitude`, `step = clamp(-residual/slope, -0.50, 0.50)`; re-evaluate the still-active rows through a stable compaction (`active` keeps ascending row order; compacted inputs feed one dense `evaluate_batch`, keeping AVX2 lanes full).
5. Passes 2..`kMaxBatchDeltaPasses` (compile-time constant, value 6 — bounded loop): secant step from `(prev_k, prev_residual)` and the current point, clamped to `±0.25`; degenerate slope (non-finite, `|prev_residual - residual| < 1e-14`) or non-finite step → route the row to scalar fallback. Converged rows freeze each pass; the active set shrinks and stays stably ordered.
6. Rows still active after the pass budget → scalar fallback list (Task 4). `evaluations_out[i]` counts one per batch pass the row participated in, saturating-added exactly like the existing `add_evaluations` helper; achieved_delta_out is the signed cold delta from the row's accepting pass.

**Tests (write first — happy path and determinism; tail/failures are Task 4):**
- `TEST(ContractProjection, BatchDeltaSolveIsColdConfirmedAgainstScalarDeltaOracle)` — on `make_surface(2u, 120.0, ...)`, build a grid of ~24 rows crossing targets {0.10, 0.25, 0.40, 0.55, 0.75} × sides {Call, Put} × T from `resolve_projected_expiry` of {days(30), months(3), years(0.5)}; solve with tolerance 1e-7; assert every row status Ok, and **for every row** the independent scalar oracle `surface.delta(strike_out[i], T[i], side[i], QueryExecution::ColdReference)` satisfies `|fabs(*oracle) - target[i]| <= 1e-7`. This test IS correctness gate #1 for the new engine.
- `TEST(ContractProjection, BatchDeltaSolveMatchesDirectProjectionEconomically)` — for each row also run `project_option_contract` with `OptionDeltaSolvePolicy::Direct` + `ProjectedStrikeSpec::delta(target)` at the same maturity; assert relative strike agreement `|K_batch - K_direct| <= 1e-4 * K_direct` (both roots satisfy the same tolerance; this bounds route drift without demanding bit equality).
- `TEST(ContractProjection, BatchDeltaSolveRowsAreCompositionInvariantAndRepeatable)` — solving the full grid twice yields bit-identical outputs; solving any contiguous sub-span of rows as its own batch yields bit-identical per-row strikes to the full batch (leans on the pinned pack-composition invariance of the laned kernels, `PricedSurface.EvaluateBatchLanedGreeksPackCompositionInvariant`). This is the property that later makes VaR thread invariance free.
- `TEST(ContractProjection, BatchDeltaSolveRejectsStructurallyInvalidSpans)` — mismatched span lengths, n==0, tolerance 0 / 2e-3 → batch-level Err, outputs untouched.

**Verification:**
- [ ] `powershell scripts\atx-build.ps1 check atx-vol\src\contract_projection.cpp`
- [ ] `powershell scripts\atx-build.ps1 build atx-vol-tests` then `powershell scripts\atx-build.ps1 -Ctest -R "^ContractProjection\."`
- [ ] `clang-format --dry-run -Werror` over the three touched files

**Acceptance:** all four new tests green for the right reason (temporarily disabling the half-tolerance margin should fail nothing here but MUST NOT be done — margin is load-bearing for Task 4's oracle guarantee); zero allocation after the first `resize` in steady state (re-solve loop in the repeatability test may assert `capacity()` stability on scratch columns).

---

### Task 4: `solve_american_delta_batch` — scalar-fallback tail, per-row failures, evaluation bookkeeping

**Context:** Completes the solver contract from Task 3. Unconverged/degenerate rows go to the existing robust scalar solver `solve_american_delta(surface, T, side, target, tolerance, QueryExecution::ColdReference)` (the bracketing/bisection route in `contract_projection.cpp`) — this is the "rare tail", and per-row failures must mirror the scalar error taxonomy so `PreparedVarPortfolio` can map them to `VarLegStatus::ProjectionUnavailable` etc.

**Files:**
- Modify: `atx-vol/src/contract_projection.cpp` (fallback + bookkeeping inside `solve_american_delta_batch`)
- Test: `atx-vol/tests/contract_projection_test.cpp`

**Behavior (exact):**
- Fallback rows are processed in ascending row order (determinism). Each calls `solve_american_delta(..., QueryExecution::ColdReference)`; on success, add its `DeltaSolution::evaluations` into `evaluations_out[i]` via the saturating `add_evaluations`; on failure, `row_status_out[i] = Err(<propagated code>)` and `strike_out/achieved_delta_out[i]` are set to NaN (never a stale candidate).
- A row whose target is outside (0,1) or whose surface data is unavailable at T fails with `Err(InvalidArgument)` / `Err(Unavailable)` respectively without consuming a batch pass.

**Tests (write first):**
- `TEST(ContractProjection, BatchDeltaSolveRoutesHardRowsThroughScalarFallbackAndStillConfirms)` — include boundary-adjacent targets {0.02, 0.95} and a very short maturity (`days(3)`, i.e. T ≈ 0.008) alongside easy rows; all rows must end Ok and pass the scalar cold-delta oracle at 1e-7; assert at least the easy rows converged within the batch pass budget (`evaluations_out <= kMaxBatchDeltaPasses + 1`) so a regression that silently sends everything to scalar fallback is caught: assert the grid's **median** `evaluations_out <= 4`.
- `TEST(ContractProjection, BatchDeltaSolveReportsRowFailuresWithoutInventingStrikes)` — rows with `target = 1.5` and `target = -0.1`, and a row with T far beyond the last fitted slice (e.g. `T = 25.0`, outside the no-extrapolation domain so `surface.iv` is NaN): row statuses Err, `strike_out` NaN for those rows, neighboring valid rows unaffected (bit-identical to solving them alone).
- `TEST(ContractProjection, BatchDeltaSolveEvaluationCountsSaturate)` — feed `evaluations` near `uint16` max only if trivially constructible; otherwise assert monotone nonzero counts on success rows (keep this cheap — the saturating helper `add_evaluations` is already unit-exercised transitively).

**Verification:**
- [ ] `powershell scripts\atx-build.ps1 check atx-vol\src\contract_projection.cpp`
- [ ] `powershell scripts\atx-build.ps1 build atx-vol-tests` then `powershell scripts\atx-build.ps1 -Ctest -R "^ContractProjection\."`
- [ ] Hygiene spot-check: `powershell scripts\atx-build.ps1 configure -Preset hygiene` + `powershell scripts\atx-build.ps1 build atx-vol-tests -Preset hygiene`
- [ ] `clang-format --dry-run -Werror` over touched files

**Acceptance:** all `ContractProjection.*` green; fallback path provably exercised (hard rows report `evaluations_out` greater than the batch budget); failure rows never write a finite strike.

---

### Task 5: VaR aggregate path — group strike resolution through the cross-sectional solver

**Context:** `evaluate_scenario_batched` (src/var.cpp) currently loops `resolve_var_contract` per leader slot before its already-batched base/shifted mark passes. Under the new policy, replace that per-slot loop with one `solve_american_delta_batch` call per `VarOptionGroup` (groups are contiguous leader slots sorted by `(uid, expiry_offset_ns)` — built in `PreparedVarPortfolio::create`). Row inputs per slot: `T` and absolute expiry from `leg.expiry_offset_ns` when `> 0` (`expiry = scenario.base_ts_ns + expiry_offset_ns`, `T = expiry_offset_ns / kNsPerYear`) else from `resolve_projected_expiry(scenario.base_ts_ns, leg.maturity)` (calendar-month legs); `side = leg.side`; `target = leg.target_abs_delta`. Fingerprints via `projected_definition_fingerprint` over a locally assembled `ProjectedOptionDefinition{OptionContract{leg.uid, K, T, leg.side}, scenario.base_ts_ns, expiry, leg.multiplier, 0}` — identical field set to the scalar path so aggregate and retained-leg fingerprints keep matching.

**Files:**
- Modify: `atx-vol/src/var.cpp` (`VarBatchScratch` extension, new group resolver, `evaluate_scenario_batched` policy branch, `valid_evaluation_config`)
- Test: `atx-vol/tests/var_test.cpp`

**Interfaces (in the anonymous namespace of var.cpp; adjust freely if a cleaner shape emerges, but keep the contract):**
```cpp
struct VarBatchScratch {
  // ... existing columns unchanged ...
  std::vector<double> solve_t{};
  std::vector<double> solve_target{};
  std::vector<std::int64_t> solve_expiry{};
  std::vector<std::uint16_t> solve_evaluations{};
  std::vector<Status> solve_status{};
  AmericanDeltaBatchScratch delta_scratch{};
};

// Fill scratch.strike/base_time/shifted_time/base_delta/fingerprint for every
// leader slot of `group` using solve_american_delta_batch on `base`.
// Returns false when any row fails (caller preserves the existing fallback()
// semantics), true when all rows are resolved and cold-confirmed.
[[nodiscard]] bool resolve_group_contracts_cross_sectional(
    const PreparedVarPortfolio::Impl &impl, const VarOptionGroup &group,
    const SurfaceRef &base, const VarScenario &scenario,
    const VarEvaluationConfig &config, VarBatchScratch &scratch);
```
- `valid_evaluation_config` accepts `OptionDeltaSolvePolicy::CrossSectionalColdConfirm`.
- In `evaluate_scenario_batched`: when `config.projection_solve_policy == CrossSectionalColdConfirm`, call the resolver per group; also compute `shifted_time = (expiry - scenario.shifted_ts_ns)/kNsPerYear` per slot with the existing `finite_positive` → `fallback()` behavior. Any row failure → the existing `fallback()` (whole scenario re-evaluated on the retained-leg scalar path for status granularity). The fallback's `evaluate_scenario` runs with a local config copy downgraded to `FastScreenColdConfirm` so the scalar per-leg solver assigns the precise `VarLegStatus` (document this with a `// SAFETY:`-style rationale comment: failure path only, statuses unchanged from the accepted route).
- The delta-vs-target acceptance check stays: resolved rows must satisfy `fabs(fabs(base_delta) - leg.target_abs_delta) <= config.delta_tolerance` (the solver guarantees tolerance/2 internally; keep the check as defense in depth).
- Marks: unchanged — the existing `evaluate_batch(EvalField::Price, ..., config.valuation_execution)` base/shifted passes.

**Tests (write first, in var_test.cpp using its `OneSurfaceSnapshot`, `option()`, `evaluation_config()` helpers):**
- `TEST(Var, CrossSectionalAggregateReplayMatchesDirectColdOracle)` — mirror the structure of `Var.ScreenedBatchProjectionIsColdConfirmedAndMatchesRetainedLegOutput` (8 targets {0.10..0.85}, alternating call/put, `RepresentativeFast` base/shifted snapshots): replay aggregate-only with `projection_solve_policy = CrossSectionalColdConfirm`; separately replay retained-leg with `Direct`; assert equal statuses/n_ok/n_failed, and cross-route economic parity `|Δbase_value|, |Δshifted_value|, |Δpnl| <= 1.0e-5 * value_scale`, `|Δdollar_delta| <= 1.0e-6 * max(1, |dollar_delta|)` (admitted strike slack — see Global Constraints gate 2).
- `TEST(Var, CrossSectionalPolicyIsAcceptedByConfigValidation)` — `PreparedVarPortfolio::create` and `replay_into` accept the new policy; the invalid-enumerator rejection (`static_cast<OptionDeltaSolvePolicy>(0xff)`) still fails (extend the assertions in the style of `Var.ValidationRejectsAbsoluteExpiryAndReportsMissingMarketsAndTimestamps`).
- `TEST(Var, CrossSectionalRowFailureFallsBackToScalarLegStatuses)` — a scenario whose shifted date is past expiry (mirror `Var.ReplayReportsOptionExpiryBeforeScenarioEndExplicitly` setup) run under the new policy reports the same `VarLegStatus::ExpiredBeforeShift` / scenario `LegFailure` as `FastScreenColdConfirm` does.

**Verification:**
- [ ] `powershell scripts\atx-build.ps1 check atx-vol\src\var.cpp`
- [ ] `powershell scripts\atx-build.ps1 build atx-vol-tests` then `powershell scripts\atx-build.ps1 -Ctest -R "^Var\."` and `powershell scripts\atx-build.ps1 -Ctest -R "^ContractProjection\."`
- [ ] `clang-format --dry-run -Werror` over touched files

**Acceptance:** new tests green; ALL pre-existing `Var.*` tests untouched and green (the new policy is opt-in at this point — default is still `FastScreenColdConfirm`).

---

### Task 6: VaR retained-leg path — shared resolver, bit-invariance, aggregate-vs-retained parity

**Context:** The retained-leg path (`evaluate_scenario` → `evaluate_option_leg`) must consume the SAME batch-resolved strikes as the aggregate path under the new policy; otherwise the SP100 bench's 1e-9 aggregate-vs-retained-leg gate (bench/var_bench.cpp `kMaxAggregateRelativeValueError`) cannot hold, because independently-run root solves land on different (equally valid) strikes. Retained-leg marks stay scalar (`SurfaceRef::evaluate`) — that path is the audit oracle and is not performance-critical.

**Files:**
- Modify: `atx-vol/src/var.cpp`
- Test: `atx-vol/tests/var_test.cpp`

**Design (exact):**
- `replay_into` builds `VarBatchScratch` per worker range whenever `leg_frames.empty() || config.projection_solve_policy == CrossSectionalColdConfirm` (today it builds scratch only for the aggregate path).
- `evaluate_scenario` gains a `VarBatchScratch *batch_scratch` parameter (nullptr for `Direct`/`FastScreenColdConfirm`, non-null under the new policy). Under the new policy it first resolves every `VarOptionGroup` via `resolve_group_contracts_cross_sectional` (surface lookup/timestamp/spot guards per group exactly as `evaluate_scenario_batched` does), then evaluates each option leader leg through a new `evaluate_option_leg_resolved(leg, /*resolved slot data*/, scenario, config, frame)` that skips the internal `resolve_var_contract` and prices scalar marks as today; duplicate legs keep flowing through `reuse_option_pricing`; stock legs unchanged. If ANY group fails to resolve, re-run the whole scenario's legs with the policy downgraded to `FastScreenColdConfirm` (same failure semantics as the aggregate `fallback()` in Task 5, keeping the two paths' statuses identical).
- `run_historical_var` needs no change (it calls `replay_into` per scenario; policy flows through `config.evaluation`).

**Tests (write first):**
- `TEST(Var, CrossSectionalReplayIsBitInvariantAcrossThreadCountsAndLegOutputIsOptional)` — clone `Var.ReplayIsBitInvariantAcrossThreadCountsAndLegOutputIsOptional` with `config.projection_solve_policy = CrossSectionalColdConfirm`: serial vs 4-thread retained-leg replay bit-identical per `expect_bit_identical` (frames and legs), aggregate-only economically equal per `expect_economically_equal` (1e-10 gate — achievable now that strikes are shared and only scalar-vs-laned mark evaluation differs, the same delta the existing test absorbs).
- `TEST(Var, CrossSectionalAggregateMatchesRetainedLegOutput)` — clone the assertion block of `Var.ScreenedBatchProjectionIsColdConfirmedAndMatchesRetainedLegOutput` under the new policy: same-route aggregate vs retained-leg at the existing 1e-7 * value_scale gates, every leg `base_delta` within `delta_tolerance` of target.
- `TEST(Var, CrossSectionalRetainedLegsAreColdConfirmedPerLeg)` — for each retained leg frame, independently assert `|fabs(base.delta(strike, base_time_to_expiry, side, ColdReference)) - target| <= delta_tolerance` using the scenario's base surface (correctness gate #1 at the VaR layer).

**Verification:**
- [ ] `powershell scripts\atx-build.ps1 check atx-vol\src\var.cpp`
- [ ] `powershell scripts\atx-build.ps1 build atx-vol-tests` then `powershell scripts\atx-build.ps1 -Ctest -R "^Var\."`
- [ ] `clang-format --dry-run -Werror` over touched files

**Acceptance:** new tests green; existing `Var.*` green; the bit-invariance property holds because scenario partitioning (`run_balanced_ranges` → `PricingExecutor::run_blocks`, contiguous per-worker scenario ranges) never splits a scenario, and the batch solver is composition-invariant (Task 3) — state that reasoning in a comment where scratch is created per range.

---

### Task 7: Default-policy flip + end-to-end `SurfaceDb` run under the new engine

**Context:** Make `CrossSectionalColdConfirm` the production default so `run_historical_var`, the bench fixture validation, and every default-config caller ride the new engine. The screened and direct routes remain selectable for comparison.

**Files:**
- Modify: `atx-vol/include/atx/vol/var.hpp` (`VarEvaluationConfig::projection_solve_policy{OptionDeltaSolvePolicy::CrossSectionalColdConfirm}` — update the adjacent comment: prepared tiers are no longer consulted for the root; the cross-sectional cold route is the default and every successful projection remains cold-confirmed)
- Test: `atx-vol/tests/var_test.cpp`

**Tests (write first):**
- `TEST(Var, DefaultSolvePolicyIsCrossSectionalColdConfirm)` — `VarEvaluationConfig{}.projection_solve_policy == OptionDeltaSolvePolicy::CrossSectionalColdConfirm`.
- Extend `Var.SurfaceDbRunSortsThreeDatesAndProducesAdjacentScenariosEndToEnd`: it already runs with a default `VarRunConfig` — after the flip it exercises the new engine end-to-end; assert it still passes unchanged. Add one explicit-policy variant assertion comparing `run_historical_var` results under `CrossSectionalColdConfirm` vs `Direct` at the cross-route 1e-5 economic gate (frames pairwise; risk statistics VaR/ES within the same relative gate).

**Verification:**
- [ ] `powershell scripts\atx-build.ps1 build atx-vol-tests`
- [ ] `powershell scripts\atx-build.ps1 -Ctest -R "^Var\."` and `powershell scripts\atx-build.ps1 -Ctest -R "^ContractProjection\."` — the FULL focused suites must be green under the new default (this is the moment thread-invariance, dedup, sign, statistics, and failure-status tests all re-certify the new engine)
- [ ] Full module gate: `powershell scripts\atx-build.ps1 -Ctest -L atx_vol_fast` then `powershell scripts\atx-build.ps1 -Ctest -L atx_vol_slow`
- [ ] Hygiene: `powershell scripts\atx-build.ps1 configure -Preset hygiene` + `powershell scripts\atx-build.ps1 build atx-vol-tests -Preset hygiene`
- [ ] `clang-format --dry-run -Werror atx-vol\include\atx\vol\var.hpp atx-vol\tests\var_test.cpp`

**Acceptance:** every gate green under the new default; no test weakened (tolerances may only be those introduced by this plan's cross-route gates).

---

### Task 8: Bench route `cross_cold` + single-shot iterate measurement (rel)

**Context:** Register the new engine as a third benchmark route beside `direct_cold`/`screened_cold` so all three are timeable on identical fixtures, then take the first single-shot measurement. The fixture build (`build_terminal_fixture` / `prepare_replayable_portfolio` in bench/var_bench.cpp) runs OUTSIDE timing and, after Task 7's default flip, itself validates the new engine against the retained-leg cold oracle at `kMaxAggregateRelativeValueError = 1.0e-9` — that gate must keep passing untouched.

**Files:**
- Modify: `atx-vol/bench/var_bench.cpp` — in `register_all()`, extend the route list:
  ```cpp
  {std::pair{"direct_cold", OptionDeltaSolvePolicy::Direct},
   std::pair{"screened_cold", OptionDeltaSolvePolicy::FastScreenColdConfirm},
   std::pair{"cross_cold", OptionDeltaSolvePolicy::CrossSectionalColdConfirm}}
  ```
  (benchmark names become `var/prepared/sp100_dispersion_terminal/ytd/thousands/cross_cold/t{1,4,8,16}`).

**Steps:**
- [ ] `powershell scripts\atx-build.ps1 configure -Preset rel -Bench` then `powershell scripts\atx-build.ps1 build atx-vol-projection-bench -Preset rel -Jobs 12`
- [ ] Single-shot iterate run (t8 first, then t1):
  ```powershell
  $env:ATX_SP100_SURFACE_DB='C:\atx-scratch\surface-db\sp100-2026'
  $env:ATX_VAR_BENCH_SINGLE_SHOT='1'
  .\build-rel\bin\atx-vol-projection-bench.exe `
    --benchmark_filter='^var/prepared/sp100_dispersion_terminal/ytd/thousands/cross_cold/t8/' `
    --benchmark_out='C:\atx\artifacts\var\sp100_dispersion_ytd_benchmark_cross_t8.json' `
    --benchmark_out_format=json
  .\build-rel\bin\atx-vol-projection-bench.exe `
    --benchmark_filter='^var/prepared/sp100_dispersion_terminal/ytd/thousands/cross_cold/t1/' `
    --benchmark_out='C:\atx\artifacts\var\sp100_dispersion_ytd_benchmark_cross_t1.json' `
    --benchmark_out_format=json
  ```
- [ ] Compare against Task 1's screened reference. Compute core-s/scenario = wall * 8 / 106 (t8) and wall / 106 (t1).
- [ ] If the number misses <= 1.0 core-s/scenario: report the miss with an evaluations profile before optimizing further — add a TEMPORARY stderr histogram (or use the opt-in `ATX_VOL_COUNTERS` ledger: `SurfaceGreekBatchDispatches` / `SurfaceGreekBatchLanes` per replay) of batch passes per converged row and scalar-fallback fraction across the 106 scenarios. Expected healthy profile: median <= 3 correction passes, fallback fraction < 2%. Remove any temporary instrumentation before finishing the task.

**Acceptance:** `cross_cold` fixture passes its 1e-9 oracle gate; single-shot t8 and t1 numbers recorded in the report; either the target is met (proceed to Task 10) or the profile identifies where the passes go (proceed to Task 9).

---

### Task 9 (conditional — only if Task 8 misses <= 1.0 core-s/scenario): determinism-preserving accelerants

**Context:** Cut delta passes without touching correctness gates or determinism. Cross-date warm-starting (seeding a date's solve from an adjacent date's roots) is FORBIDDEN: per-worker contiguous date ranges make it order- and thread-count-dependent, which breaks the pinned bit-invariance tests and scenario independence. All admissible accelerants must be functions of (reference portfolio, base surface) only.

**Files:** `atx-vol/src/var.cpp`, `atx-vol/src/contract_projection.cpp`, tests alongside.

**Candidate accelerants, in priority order (implement + measure one at a time, single-shot t8 between each; keep only wins):**
1. **Reference-anchored seed:** `PreparedVarPortfolio` already stores each option leg's reference-date solved log-moneyness (`VarReferenceLeg::log_moneyness`, computed once in `prepare_option`). Use it as the pass-0 candidate instead of (or blended with) the Black seed: it is date-independent (same seed for every scenario ⇒ deterministic and thread-invariant) and typically within a few vol points of the historical root, often converging in 1–2 passes. Plumb per-slot seed_k into `solve_american_delta_batch` via an optional `std::span<const double> seed_k_log` parameter (empty = Black seed). Test: pin bit-invariance + the cold oracle again with the seed active; pin that a deliberately terrible seed still converges (falls back to Newton/secant/scalar).
2. **Seed smile-refresh trim:** measure 1 vs 2 smile-refresh iterations in the batch seed (the scalar solver uses 2). Keep 2 unless 1 shows no pass-count regression on the SP100 profile.
3. **Group-level T dedup:** legs within a group sharing bit-identical `expiry_offset_ns` produce bit-identical T; `evaluate_batch` already hoists the T-bracket per bit-identical run, and `create` already sorts leader slots by `(uid, expiry_offset_ns)` — verify the sort actually yields maximal runs (it should; if a profile shows re-bracketing, fix ordering, don't add caching).
4. **Scalar-fallback budget tuning:** if the fallback fraction > 2%, raise `kMaxBatchDeltaPasses` by 2 before touching anything else (a laned pass is ~an order cheaper than a scalar fallback solve).

**Verification:** after each accepted accelerant — `powershell scripts\atx-build.ps1 -Ctest -R "^Var\."`, `-R "^ContractProjection\."`, and a fresh single-shot t8/t1 measurement; record the delta.

**Acceptance:** <= 1.0 core-s/scenario at t8 single-shot (or a documented report explaining the best-achieved number and the measured wall/pass breakdown if the milestone is still unreached — do NOT weaken gates to hit the number).

---

### Task 10: Final CV-gated numbers on `rel` and `rel-avx2` (t1/t4/t8/t16)

**Context:** Citable numbers per the repo benchmark policy: `bench_util.hpp::apply_common` (>=0.5 s warm-up, 5 repetitions, per-repetition rows, p95 + CV statistics), CV <= 5% per case, quiet host with the P-core lease. Measure both Release presets — `rel` and `rel-avx2` (global `/arch:AVX2`; the laned kernels are AVX2 in both, but globally vectorized surrounding code may differ).

**Steps:**
- [ ] Build both: `powershell scripts\atx-build.ps1 configure -Preset rel -Bench` + `build atx-vol-projection-bench -Preset rel -Jobs 12`; same with `-Preset rel-avx2`.
- [ ] For each preset exe (`build-rel\bin\atx-vol-projection-bench.exe`, `build-rel-avx2\bin\atx-vol-projection-bench.exe`), with `ATX_SP100_SURFACE_DB` set and `ATX_VAR_BENCH_SINGLE_SHOT` **unset** (`Remove-Item Env:ATX_VAR_BENCH_SINGLE_SHOT -ErrorAction SilentlyContinue`):
  ```powershell
  .\build-rel\bin\atx-vol-projection-bench.exe `
    --benchmark_filter='^var/prepared/sp100_dispersion_terminal/ytd/thousands/cross_cold/' `
    --benchmark_out='C:\atx\artifacts\var\sp100_dispersion_ytd_benchmark_cross_rel.json' `
    --benchmark_out_format=json
  # rel-avx2 run -> ..._cross_relavx2.json
  ```
  (the filter without a `/tN/` suffix covers all four worker counts in one invocation; each case carries its own 5 repetitions).
- [ ] Also capture a 5-rep `screened_cold` t8 run on `rel` for the before/after table.
- [ ] Verify every reported case's `cv` aggregate <= 5%; rerun noisy cases on a quieter host state.
- [ ] Tabulate: wall (median), p95, scenarios/s, core-s/scenario for t1/t4/t8/t16 × {rel, rel-avx2}; note which preset is the accepted citable configuration.

**Acceptance:** milestone check — median t8 core-s/scenario <= 1.0 AND median t1 wall <= ~106 s on at least one Release preset with CV <= 5%; JSON artifacts written; table recorded for Task 12.

---

### Task 11: Regenerate the accepted P&L trace + cumulative-P&L PNG from the new route

**Context:** The delivered chart and the final named trace must share provenance (status doc, "Current P&L result and artifacts"). The TSV is written by the bench fixture (env `ATX_VAR_PNL_TSV` / `ATX_VAR_FAILURE_TSV` in `prepare_replayable_portfolio`), which after Task 7 runs the new default engine and its 1e-9 oracle gate.

**Steps:**
- [ ] Single fixture-building run on the accepted preset:
  ```powershell
  $env:ATX_SP100_SURFACE_DB='C:\atx-scratch\surface-db\sp100-2026'
  $env:ATX_VAR_BENCH_SINGLE_SHOT='1'
  $env:ATX_VAR_PNL_TSV='C:\atx\artifacts\var\sp100_dispersion_ytd_pnl_cross.tsv'
  $env:ATX_VAR_FAILURE_TSV='C:\atx\artifacts\var\sp100_dispersion_ytd_failures_cross.tsv'
  .\build-rel\bin\atx-vol-projection-bench.exe `
    --benchmark_filter='^var/prepared/sp100_dispersion_terminal/ytd/thousands/cross_cold/t8/'
  ```
- [ ] `python atx-vol\bench\plot_var_cumulative_pnl.py C:\atx\artifacts\var\sp100_dispersion_ytd_pnl_cross.tsv C:\atx\artifacts\var\sp100_dispersion_ytd_cumulative_pnl.png`
- [ ] Visually inspect the PNG: same corrected economics as the accepted screened trace — cumulative P&L ending near +$6.55M (small drift vs $6,546,716 is expected and must be within the admitted cross-route economic error; report the exact final value and the delta), 12 visible breaks preserved, no billion-scale artifacts.
- [ ] Sanity-diff the TSV against `C:\atx\artifacts\var\sp100_dispersion_ytd_pnl_screened.tsv`: identical row count/dates; per-scenario pnl deltas small relative to scale.

**Acceptance:** TSV + regenerated PNG with matching provenance; final cumulative P&L recorded; fixture oracle gate (1e-9) passed during the producing run.

---

### Task 12: Update the status document + final gate sweep

**Context:** `atx-vol/docs/historical-var-engine-status.md` must reflect the new engine as the accepted route, with the measured numbers, and stop calling the module "in active verification".

**Files:** Modify `atx-vol/docs/historical-var-engine-status.md`.

**Steps:**
- [ ] Rewrite the affected sections: executive summary (new core-s/scenario), "Contract projection extension" (three-policy enum incl. `CrossSectionalColdConfirm` semantics + `solve_american_delta_batch`), "Measured performance" (Task 10 table: rel and rel-avx2, t1/t4/t8/t16, 5-rep median/p95/CV; keep the historical Direct/screened/rejected rows for context), "Current P&L result and artifacts" (`*_cross.tsv` + regenerated PNG provenance, final cumulative value), "Validation state" (all gates rerun and green: focused suites, full `atx_vol_fast`/`atx_vol_slow` labels, clang-format, hygiene preset, CV-gated bench, PNG), and replace "Recommended next performance design" with a short "Shipped design" description + any remaining ideas (e.g. further ISA work) clearly marked speculative.
- [ ] Keep the rejected-route records intact (prepared/fast marks 4.8%; grouped batch-root slower) — they are guardrails, not history to delete.
- [ ] Final sweep (all from worktree root):
  - `powershell scripts\atx-build.ps1 -Ctest -R "^Var\."`
  - `powershell scripts\atx-build.ps1 -Ctest -R "^ContractProjection\."`
  - `powershell scripts\atx-build.ps1 -Ctest -L atx_vol_fast`
  - `powershell scripts\atx-build.ps1 -Ctest -L atx_vol_slow`
  - `powershell scripts\atx-build.ps1 configure -Preset hygiene` + `powershell scripts\atx-build.ps1 build atx-vol-tests -Preset hygiene`
  - `clang-format --dry-run -Werror` over every file this plan touched
- [ ] Review checklist from `C:\atx\.agents\cpp\agent.md` §10 applied to the full diff.

**Acceptance:** status doc accurate and self-consistent with the artifacts; every gate green; branch ready for review/merge.

---

## Task Dependency Notes

- **Task 1** is independent and must complete first (stop-the-line on baseline failures).
- **Task 2 → 3 → 4** are strictly sequential (solver seams → core → tail).
- **Task 5** depends on 2–4; **Task 6** depends on 5 (shared resolver + fallback semantics); **Task 7** depends on 5–6.
- **Task 8** depends on 7 (default flip drives the bench fixture's oracle gate through the new engine). **Task 9** is conditional on Task 8's number.
- **Task 10** depends on 8 (and 9 if triggered). **Task 11** depends on 10 (accepted preset choice). **Task 12** is last.
- Parallelism: Task 2 may start while Task 1's benchmark is running (different build trees), but no implementation task may land before Task 1's gates are green. Tasks 3/4 tests can be drafted concurrently, but land sequentially.

## Risks / Rejected-Route Guardrails

1. **Do not re-tread the grouped scalar batch route.** The rejected experiment (57.717 s, 4.36 core-s/scenario) wrapped per-option scalar root solves in a group loop: every option still ran its own seed → Newton → bracket sequence with scalar cold delta evaluations, so the batch added orchestration cost while retaining nearly all root cost. The new design is different in kind, not degree: a **small fixed number of cross-sectional kernel passes** (1 seed pass + median ~2–3 correction passes, hard cap `kMaxBatchDeltaPasses = 6`), each pass ONE K4-reduced boundary solve per option amortized 4-wide through the laned AVX2 greek kernels, with per-option iteration replaced by shared passes over a shrinking compacted active set. Guardrail metric (Task 8): if the measured profile shows median correction passes > ~4 or scalar-fallback fraction > ~2%, stop and diagnose (seed quality, step clamps) instead of shipping another slow batch wrapper.
2. **Prepared/fast marks stay inadmissible for valuation** (4.8% aggregate error, rejected). The new route never consults the prepared tier at all: every `evaluate_batch`/`evaluate`/`delta` call passes `QueryExecution::ColdReference` explicitly. The SP100 snapshots are loaded `RepresentativeFast` — the explicit cold execution argument is what keeps the laned cold kernels engaged (`evaluate_batch` requires `execution == ColdReference` to take the laned/resolved cold routes on an accelerator-carrying surface); never pass `Configured` in the new engine.
3. **Laned-vs-scalar kernel gap vs the cold oracle.** The AVX2 greek kernels match scalar `american_greeks_al` to a documented economic gate (~1e-13-scale transcendental differences), not bit-exactly. A root accepted at the full 1e-7 tolerance from a laned residual could, in principle, sit marginally outside the *scalar* oracle tolerance. Mitigation is structural: internal batch acceptance at `tolerance/2` (5e-8 of headroom, orders above the kernel gap), with the scalar-oracle assertion pinned by `ContractProjection.BatchDeltaSolveIsColdConfirmedAgainstScalarDeltaOracle` and `Var.CrossSectionalRetainedLegsAreColdConfirmedPerLeg`. Never relax the half-tolerance margin to save a pass.
4. **Cross-route strike drift vs the 1e-9 bench gate.** Two solvers converging within the same delta tolerance land on slightly different strikes; comparing NEW aggregate vs OLD scalar retained-leg would blow the bench's 1e-9 gate. That is why Task 6 routes BOTH paths through one shared resolution per scenario (same-route parity stays ~1e-13-scale, gate intact) and the independent oracle is asserted at the delta level (gate 1) plus a cross-route economic gate at 1e-5 (gate 2). Do not "fix" a same-route parity failure by loosening `kMaxAggregateRelativeValueError`.
5. **Determinism/thread invariance.** Forbidden: any cross-date or cross-scenario warm start, any ordering dependent on worker partition, any shared mutable solver state. Allowed: reference-anchored seeds (date-independent, computed once at `create`). The batch solver's row order is fixed by `grouped_option_indices` (sorted `(uid, expiry_offset_ns)`), scenarios are whole-per-worker (`run_balanced_ranges`), and the laned kernels are pack-composition invariant — cite `PricedSurface.EvaluateBatchLanedGreeksPackCompositionInvariant` when reasoning about this in code comments.
6. **Allocation discipline.** The per-scenario hot loop must allocate nothing in steady state: all new columns live in `VarBatchScratch`/`AmericanDeltaBatchScratch`, sized once per worker range and reused across scenarios (matches the existing `make_var_batch_scratch` pattern and the coding standard's hot-path allocation rule).
7. **`evaluate_batch` structural rules.** Output spans must be exactly n-sized where required (iv/price/status/greeks for a FirstOrder request) and must not overlap inputs or each other — overlap is rejected before any write. Compacted pass buffers in scratch must therefore be distinct vectors, never subspans of one buffer that could alias.
8. **Bench comparability.** Numbers are only citable from `build-rel*/` preset builds via `scripts\atx-build.ps1` on a quiet host with the P-core lease; single-shot numbers are directional only. Never compare across hosts or against numbers taken with `ATX_VAR_BENCH_SINGLE_SHOT=1`.
