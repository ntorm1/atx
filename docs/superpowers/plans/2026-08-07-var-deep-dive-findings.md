# VaR Deep-Dive Findings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the confirmed findings of the 2026-08-07 deep-dive review of the historical-simulation VaR engine: honest P&L-track presentation, solver/projection correctness hardening, engine-level reporting and risk-metric completeness, invariant test backfill, and two guarded performance wins.

**Architecture:** All work happens in the `var` worktree (`C:\atx-wt\var`) on branch `var`, which already contains the merged `main`. The engine core (`atx-vol/src/var.cpp`, `contract_projection.cpp`, `historical_projection.cpp`) is extended additively; presentation fixes live in `atx-vol/bench/`. Every change is gated by the existing determinism and parity invariants.

**Tech Stack:** C++20 (MSVC, CMake presets), GoogleTest, Python 3 + pandas/matplotlib for bench plotting, pytest for python tests.

## Review reports (source of line-level detail)

The five review reports that motivate this plan live at
`C:\Users\natha\AppData\Local\Temp\claude\c--atx\5f2c9ec9-4217-440c-aad3-cfcca167737c\scratchpad\review\`:
`pnl-forensics.md`, `solver-correctness.md`, `projection-correctness.md`, `performance.md`, `feature-gaps.md`.
Task briefs cite them as [pnl], [solver], [proj], [perf], [gaps]. Implementers should read the cited report section before coding.

## Global Constraints

- Determinism invariants are inviolable: thread-count bit-invariance of replay frames, no cross-date warm starting, cold marks (`QueryExecution::ColdReference`) for all valuation in the VaR path.
- The SP100 aggregate-vs-retained 1e-9 relative parity gate must stay green; any task touching `var.cpp`/`contract_projection.cpp` must run `build\bin\atx-vol-tests.exe --gtest_filter=Var.*:ContractProjection.*` (all green) before committing.
- API changes are additive only. New struct fields get value-initialized defaults preserving current behavior. New enum values append (never renumber). Existing callers in-repo must be updated in the same commit.
- Build: `cmake --build build --target atx-vol-tests` from `C:\atx-wt\var` (Debug/dev preset build dir `build` already exists). Bench binary target for fixture runs: `var_bench` (see `atx-vol/bench/CMakeLists.txt`); fixture invocation cribbed from `C:\atx\artifacts\var\task10_runner.ps1`.
- clang-format every touched C++ file (`clang-format -i <file>`).
- Code, comments, commit messages: normal professional prose (no caveman compression).
- Performance keep/revert decisions use deterministic counters (pass counts, mark-pass counts), never wall clock alone — this host's wall clock is contamination-prone ([perf] finding 4, sprint history).

---

### Task 1: P&L track truth-in-labeling and decomposition

The `sp100_dispersion_ytd_cumulative_pnl.png` presents cumulated restruck-scenario P&L as "Historical Replay of the 2026-07-31 Portfolio". [pnl] shows 89% of the +$6.55M is re-basing resets (the book is restruck to reference |Δ|/relative-TTE every session, re-arming theta), only 11% is genuine held-profile drift. Fix the presentation: disclose semantics and plot the decomposition.

**Files:**
- Modify: `atx-vol/bench/plot_var_cumulative_pnl.py`
- Create: `atx-vol/bench/plot_var_cumulative_pnl_test.py`
- Regenerate: `C:\atx\artifacts\var\sp100_dispersion_ytd_cumulative_pnl.png` from `C:\atx\artifacts\var\sp100_dispersion_ytd_pnl_cross.tsv`

**Interfaces:**
- Produces: pure function `compute_decomposition(df) -> df` in the plot script, imported by the test. Input df columns: `base_date`, `shifted_date`, `base_value`, `shifted_value`, `pnl`, `cumulative_pnl`. Output adds columns: `rebasing_reset` (float, NaN on chain breaks), `cumulative_held_drift` (float).

**Definitions (exact):**
- Row i and i+1 are *chained* iff `shifted_date[i] == base_date[i+1]`.
- `rebasing_reset[i] = base_value[i+1] - shifted_value[i]` when chained, else NaN (history break).
- `cumulative_held_drift[k] = cumulative_pnl[k] + sum(rebasing_reset[i] for i < k if chained)` — the telescoped value change of holding the restruck-once profile across each chain, per [pnl] section "telescoping decomposition".

**Steps:**

- [ ] **Step 1:** Write `plot_var_cumulative_pnl_test.py` with a synthetic 4-row frame (rows 0-1 chained, break, rows 2-3 chained; hand-computed expected values) asserting: (a) `rebasing_reset` matches hand-computed `base_value[i+1] - shifted_value[i]` on chained rows and is NaN at the break; (b) the identity `cumulative_held_drift[last] == cumulative_pnl[last] + nansum(rebasing_reset[:-1])`; (c) `compute_decomposition` does not mutate its input.
- [ ] **Step 2:** Run `python -m pytest atx-vol/bench/plot_var_cumulative_pnl_test.py -v` — expect FAIL (function missing).
- [ ] **Step 3:** Refactor the script: extract `compute_decomposition(df)` implementing the definitions above; keep all existing I/O behavior.
- [ ] **Step 4:** Update the figure: title `"Characteristic-Restruck Historical Replay — 2026-07-31 SP100 Dispersion Profile"`; subtitle must state, verbatim concepts: restruck to reference |Δ| and relative expiry each session, re-sized to reference dollar delta, cumulative P&L compounds per-session restruck scenarios (not a held book), model mids with no transaction costs. Add a second line series `cumulative_held_drift` (labeled "held-profile revaluation drift") and a legend entry attributing the remainder to re-basing resets.
- [ ] **Step 5:** Run pytest — expect PASS. Regenerate the PNG from the cross TSV; verify with a quick visual read that both series render and the held-drift line ends near +$0.72M ([pnl] quantification).
- [ ] **Step 6:** Update `atx-vol/docs/historical-var-engine-status.md` P&L-artifact paragraph to name the two components (+$5.83M re-basing resets incl. break resets, +$0.72M held drift) and cite the restruck semantics.
- [ ] **Step 7:** Commit: `docs+bench(var): disclose restruck-replay semantics in cumulative P&L artifact and decompose drift`.

---

### Task 2: Solver and error-path hardening

Findings [solver] F1 (Important), F2, F4, F5.

**Files:**
- Modify: `atx-vol/src/contract_projection.cpp` (F2), `atx-vol/src/var.cpp` (F4, F5), `atx-vol/include/atx/vol/var.hpp` (new enum value), tests in `atx-vol/tests/contract_projection_test.cpp` and the var test file (locate via `grep -l "Var\." atx-vol/tests/*.cpp`).

**Interfaces:**
- Produces: `VarScenarioStatus::ArchiveError = 4` (appended). Later tasks (Task 4 TSV writer) must include it in `to_string`.

**Requirements:**
1. **F1 parity pin:** new test `ContractProjection.BatchLanedDeltaMatchesScalarColdOracleAtSolverRequestShape` — over the existing projection test fixture surface(s), for a grid of ≥ 200 (strike, expiry, side) rows, run the laned `FirstOrder` evaluation exactly as the batch solver requests it (same `GreekNeeds` reduction, `ColdReference`) and the scalar cold delta; assert `max |laned Δ − scalar Δ| ≤ 5e-8` ([solver] F1: the tolerance/2 acceptance headroom silently assumes this bound; today only a 1e-5 rel gate exists at priced_surface_test.cpp:848).
2. **F2 overflow guard:** in `solve_american_delta_batch` (contract_projection.cpp:524 area), reject `n > std::numeric_limits<std::uint32_t>::max()` with an explicit error `Status` before the cast. Test with a fabricated span size is impractical — assert the guard exists via a unit test on the extracted checking helper, or restructure the cast through a checked helper `checked_row_count(std::size_t) -> Result<std::uint32_t>` and unit-test that helper directly with `size_t{1} << 32`.
3. **F4 archive-error classification:** in `load_snapshot` (var.cpp:265-285), corrupt-archive / I/O failures must map to `VarScenarioStatus::ArchiveError`, not `MarketUnavailable`/`SurfaceUnavailable`, so `ExcludeFromDistribution` cannot silently absorb infrastructure faults. Under `VarScenarioFailurePolicy::ExcludeFromDistribution`, `ArchiveError` scenarios still fail the run (structural, not market). Test: point a run at a truncated/corrupt archive fixture file (create a temp copy and truncate it) and assert the run fails with the new status rather than excluding.
4. **F5 structural classification:** non-monotone archive timestamps (var.cpp:1543-1550) report `TimestampMismatch`, not `InvalidValue`. Adjust the existing pinning test if one asserts the old label; otherwise add one.

**Steps:**
- [ ] Write failing tests for items 1-4 (names above; F5 test `Var.NonMonotoneArchiveTimestampsReportTimestampMismatch`).
- [ ] Run `build\bin\atx-vol-tests.exe --gtest_filter=ContractProjection.*:Var.*` — new tests FAIL, existing green.
- [ ] Implement the four changes; update `to_string(VarScenarioStatus)`.
- [ ] Full filter green; clang-format; commit `fix(vol): pin laned-vs-scalar delta bound, guard batch row overflow, classify archive and timestamp faults` .

---

### Task 3: Projection-path safety knobs and silent-bias telemetry

Findings [proj] I1, I2, I3, I4, I5 + cheap minors (base-IV aliasing var.cpp:1035, ExpiredBeforeShift ordering var.cpp:686 — fix only if zero-risk, else record as deferred in the report file).

**Files:**
- Modify: `atx-vol/include/atx/vol/var.hpp`, `atx-vol/src/var.cpp`, `atx-vol/include/atx/vol/historical_projection.hpp`, `atx-vol/src/historical_projection.cpp`, tests.

**Interfaces (exact additions):**
```cpp
// var.hpp — VarRunConfig additions (defaults preserve current behavior):
//   Maximum calendar-day gap between base and shifted session for a transition
//   to enter the distribution. 0 disables the guard (current behavior).
int max_session_gap_days{0};
//   Fail the run when more than this fraction of scenarios is excluded under
//   ExcludeFromDistribution. 1.0 disables the guard.
double max_excluded_fraction{1.0};

// var.hpp — HistoricalVarResult additions:
std::size_t n_gap_skipped{0};
std::size_t n_excluded_from_distribution{0};
std::size_t n_tenor_extrapolated_legs{0};   // legs whose solve or valuation used tenor extrapolation, either side

// var.hpp — VarLegFrame addition (append, keep operator== defaulted):
//   Bit 0: base-side tenor extrapolation; bit 1: shifted-side tenor
//   extrapolation; bit 2: restrike root beyond max_restrike_abs_log_moneyness.
std::uint8_t diagnostic_flags{0};

// VarEvaluationConfig addition:
//   Restrike roots with |log-moneyness| beyond this bound fail the leg with
//   InvalidDelta instead of pricing on pure wing extrapolation.
double max_restrike_abs_log_moneyness{5.0};

// historical_projection.hpp — evaluate_into gains an execution parameter:
Status evaluate_into(..., QueryExecution execution = QueryExecution::Configured);
```

**Requirements:**
1. **I1 gap policy:** `run_historical_var` skips adjacent transitions whose calendar gap exceeds `max_session_gap_days` (when > 0), counts them in `n_gap_skipped`. Test: fixture date list with an induced 10-day hole, config gap 5 → transition skipped and counted; gap 0 → current behavior (bridged) preserved.
2. **I2 cold knob:** `PreparedHistoricalProjection::evaluate_into` accepts `QueryExecution`; `projected_historical_var` and any VaR-path caller pass `ColdReference` explicitly. Default parameter `Configured` keeps non-VaR callers unchanged. Test pins that the VaR path requests cold execution (assert via the existing counters/mark-source plumbing — see [proj] I2 for the leak site, historical_projection.cpp:70).
3. **I3 tenor-extrapolation telemetry:** plumb the existing `extrapolates_tenor` signal ([proj] I3, vol_curve.cpp:329-340) into `VarLegFrame::diagnostic_flags` bits 0/1 and aggregate `n_tenor_extrapolated_legs`. No behavior change — telemetry only.
4. **I4 restrike wing bound:** enforce `max_restrike_abs_log_moneyness` at restrike-root acceptance (contract_projection.cpp:139-168 consumer side in var.cpp); default 5.0 matches today's implicit bound so fixture behavior is unchanged; bit 2 set when a root is within (0.8×bound, bound] as an early-warning flag. Test: synthetic deep-wing target delta produces InvalidDelta beyond bound.
5. **I5 exclusion accounting:** populate `n_excluded_from_distribution`; enforce `max_excluded_fraction`; statistics unchanged otherwise. Test: run with one induced failing scenario under ExcludeFromDistribution → counter 1; with `max_excluded_fraction = 0.0` → run fails.
6. Aggregate-vs-retained parity and thread-invariance filters green (`Var.*`), since `VarLegFrame` layout changed.

**Steps:** failing tests → run (FAIL) → implement → `Var.*:ContractProjection.*:HistoricalProjection.*` green → clang-format → commit `feat(vol): session-gap policy, cold-execution knob, extrapolation and exclusion telemetry in VaR path`.

---

### Task 4: Engine-level scenario reporting and attribution API

Finding [gaps] 6, 7: all per-scenario TSV accounting lives in `var_bench.cpp:156-272, 676-692`; attribution is absent (scenario tail index discarded, var.cpp:1427).

**Files:**
- Create: `atx-vol/include/atx/vol/var_report.hpp`, `atx-vol/src/var_report.cpp`, `atx-vol/tests/var_report_test.cpp`
- Modify: `atx-vol/bench/var_bench.cpp` (consume the new API; TSV schema/columns byte-stable vs current output), `atx-vol/CMakeLists.txt` (add sources).

**Interfaces (exact):**
```cpp
namespace atx::vol {
// Book-construction accounting the engine cannot know; supplied by the caller.
struct VarExclusionSummary {
  std::size_t source_option_lots{0};
  std::size_t coverage_excluded_option_lots{0};
  std::size_t delta_boundary_excluded_option_lots{0};
  std::size_t replay_excluded_option_lots{0};
  std::size_t stock_hedges{0};
};

// Writes the per-scenario TSV exactly as var_bench emits today (same header,
// same column order, same formatting), sourced from result.frames /
// result.leg_frames. leg-derived max_abs_leg_* columns require
// retain_leg_frames; without them those columns are empty strings.
Status write_var_scenario_tsv(std::ostream &out, const HistoricalVarResult &result,
                              const VarExclusionSummary &exclusions);

struct VarUnderlierAttribution {
  std::string underlier{};
  double total_pnl{0.0};
  double worst_scenario_pnl{0.0};
  std::int64_t worst_scenario_base_ts_ns{0};
};
// Requires retain_leg_frames; sorted by ascending total_pnl (worst first).
Result<std::vector<VarUnderlierAttribution>> attribute_by_underlier(const HistoricalVarResult &result);
} // namespace atx::vol
```

**Steps:**
- [ ] Golden test: build a tiny synthetic `HistoricalVarResult` (2 scenarios × 2 legs, hand-filled), assert `write_var_scenario_tsv` output equals a checked-in expected string, and `attribute_by_underlier` returns hand-computed totals, worst scenario, and ordering. Include a `VarScenarioStatus::ArchiveError` frame to cover Task 2's enum in `to_string`.
- [ ] Run (FAIL) → implement → PASS.
- [ ] Refactor `var_bench.cpp` to call the API. Verify schema stability: regenerate the TSV for any locally runnable config OR diff the header line + a formatting unit test against `C:\atx\artifacts\var\sp100_dispersion_ytd_pnl_cross.tsv` header.
- [ ] `Var.*` green; clang-format; commit `feat(vol): engine-level VaR scenario TSV writer and per-underlier attribution`.

---

### Task 5: Risk-metric completeness and backtest validation statistics

Finding [gaps] 1, 2, 4 (statistics side): engine emits one nearest-rank quantile at one confidence; no ES weighting options; zero validation statistics anywhere.

**Files:**
- Create: `atx-vol/include/atx/vol/var_validation.hpp`, `atx-vol/src/var_validation.cpp`, `atx-vol/tests/var_validation_test.cpp`
- Modify: `atx-vol/include/atx/vol/var.hpp` + `var.cpp` (multi-confidence + weighting), `atx-vol/bench/var_bench.cpp` (print the block), `atx-vol/CMakeLists.txt`.

**Interfaces (exact):**
```cpp
// var.hpp:
struct VarWeighting {
  // 1.0 = equal weights (current behavior). Otherwise BRW/EWMA weight
  // lambda^(age) normalized, age 0 = most recent scenario by shifted_ts_ns.
  double ewma_lambda{1.0};
};
// Weighted quantile: sort losses ascending, accumulate normalized weights,
// VaR = first loss whose cumulative weight >= confidence; ES = weighted mean
// of losses >= VaR (weights renormalized over that tail).
[[nodiscard]] Result<VarRiskStatistics>
historical_var_statistics(std::span<const VarScenarioFrame> frames, double confidence,
                          const VarWeighting &weighting);
[[nodiscard]] Result<std::vector<VarRiskStatistics>>
historical_var_curve(std::span<const VarScenarioFrame> frames,
                     std::span<const double> confidences, const VarWeighting &weighting = {});

// var_validation.hpp:
struct KupiecResult { double lr_pof{0.0}; double p_value{0.0}; std::size_t n_obs{0}; std::size_t n_breaches{0}; };
// LR_pof = -2 ln[ (1-p)^(n-x) p^x ] + 2 ln[ (1-x/n)^(n-x) (x/n)^x ], chi-square 1 dof.
// 1-dof survival: p = erfc(sqrt(LR/2)). Edge cases x==0 and x==n handled by limits.
[[nodiscard]] Result<KupiecResult> kupiec_pof(std::size_t n_obs, std::size_t n_breaches, double var_confidence);

struct ChristoffersenResult { double lr_independence{0.0}; double p_independence{0.0};
                              double lr_conditional_coverage{0.0}; double p_conditional_coverage{0.0}; };
// First-order Markov independence LR (1 dof, survival erfc(sqrt(LR/2)));
// conditional coverage = LR_pof + LR_ind (2 dof, survival exp(-LR/2)).
[[nodiscard]] Result<ChristoffersenResult>
christoffersen(std::span<const bool> breach_sequence, double var_confidence);
```

**Reference values for tests (hard-assert, tol 1e-3):** Kupiec with n=250, x=5, p=0.01: LR_pof = 1.9568, p ≈ 0.1618. Christoffersen independence on the 10-obs sequence `0,0,1,1,0,0,0,1,0,0` — implementer computes the expected LR by hand in the test comment (n00,n01,n10,n11 = 5,2,2,0 transition counts from the 9 transitions) and cross-checks numerically against `scipy.stats.chi2.sf` in a throwaway python session before pinning.

**Steps:** failing tests (including: ewma_lambda=1.0 exactly reproduces the existing unweighted `historical_var_statistics` on a 20-frame synthetic set; curve is monotone in confidence) → implement → green → wire a `validation:` block into var_bench terminal output (breach sequence = scenarios with loss > VaR at the run confidence) → `Var.*` green → clang-format → commit `feat(vol): weighted VaR/ES, multi-confidence curve, Kupiec and Christoffersen validation statistics`.

---

### Task 6: Invariant test backfill

[solver] "claimed-but-untested invariants" — the load-bearing four, as pure test additions (no production code changes; if a test exposes a real defect, STOP and report BLOCKED rather than patching production here).

**Files:**
- Modify/Create tests only: `atx-vol/tests/contract_projection_test.cpp`, var test file.

**Tests to add:**
1. `ContractProjection.BatchSolveIsPackCompositionInvariantAtSolverRequestShape` — same rows solved as one batch vs split into two packs produce bit-identical deltas/strikes (FirstOrder + reduced GreekNeeds shape).
2. `Var.GenuineSolverRowFailureDowngradesWholeScenarioToScalarRoute` — inject a row the batch solver cannot converge (e.g. unattainable target delta near the boundary admitted by config) and assert the whole scenario's frames are byte-identical to a forced `FastScreenColdConfirm` run.
3. `Var.ScalarFallbackTailFiresThroughVarEngine` — construct a book where at least one row exhausts `kMaxBatchDeltaPasses` (deep wing target within bounds) and assert the fallback counter increments and results equal the scalar route bit-exactly.
4. `Var.ThreadCountBitInvarianceHoldsWithDowngradedScenariosPresent` — extend the existing thread-invariance test's book with the downgrade-inducing row from (2); frames byte-identical across t1/t2/t4.

**Steps:** write → run (each must PASS against current production code; a FAIL is a discovered defect → BLOCKED protocol) → clang-format → commit `test(vol): pin pack-composition, downgrade, fallback-tail, and mixed-downgrade thread invariants`.

---

### Task 7: Base-mark harvest from solver passes (guarded perf)

[perf] finding 1 (High, ~18-22% core time): the laned FirstOrder passes already compute the cold American mark at every accepted strike (`laned_greek_run.hpp:154-184` writes `AmericanGreeks::price`; acceptance at contract_projection.cpp:617 is at exactly the evaluated strike), then var.cpp:1038 recomputes base marks in a dedicated Price pass.

**Files:**
- Modify: `atx-vol/include/atx/vol/contract_projection.hpp`, `atx-vol/src/contract_projection.cpp` (optional harvested-price output span), `atx-vol/include/atx/vol/var.hpp` (config knob), `atx-vol/src/var.cpp` (consume; both aggregate and retained routes), tests.

**Interfaces:**
```cpp
// VarEvaluationConfig addition:
enum class VarBaseMarkSource : std::uint8_t {
  DedicatedPricePass = 0,   // current behavior, default
  HarvestedFromSolver = 1,  // reuse the solver's accepted-strike cold price
};
VarBaseMarkSource base_mark_source{VarBaseMarkSource::DedicatedPricePass};
```

**Requirements:**
1. `solve_american_delta_batch` optionally returns the accepted-strike cold price per row (harvested at acceptance; scalar-fallback rows carry the scalar route's price). No extra passes.
2. Bit-parity pin FIRST: test `ContractProjection.HarvestedSolverPriceIsBitIdenticalToDedicatedPricePass` over ≥200 fixture rows. If bit-identical: proceed. If not bit-identical but within 1e-9 relative: keep the feature behind the knob, default stays `DedicatedPricePass`, record the measured max gap in the test as a `<=` pin, and note in the report file that a default flip needs the fixture P&L parity rerun.
3. With the knob on: `Var.*` suite green including aggregate-vs-retained parity; the retained-leg route consumes the same harvested value so parity is by construction, not coincidence.
4. Counter evidence: run the bench fixture once (dev-counters or rel preset; crib `task10_runner.ps1`) with knob on vs off and record the base-mark-pass counter delta in the commit message. Wall clock optional/informational only.
5. Default flip to `HarvestedFromSolver` ONLY if: bit-parity test green AND full `Var.*:ContractProjection.*` green AND fixture parity counters zero. Otherwise leave default off and record.

**Steps:** parity pin test → harvest plumbing → knob-on tests green → counter run → (conditional) default flip → clang-format → commit `perf(vol): harvest solver-pass cold marks for VaR base valuation behind VarBaseMarkSource` (+ second commit for the default flip if taken).

---

### Task 8: Dynamic scenario scheduling and bench integrity

[perf] findings 2, 4: static `run_balanced_ranges` (var.cpp:70-91) gates t8 wall on E-cores (~10-20%); bench divides by requested workers even when `ATX_VOL_FIT_WORKERS` shrinks the pool; `dump_counters` exists but is never wired into `run_terminal_var`.

**Files:**
- Modify: `atx-vol/src/var.cpp` (scenario loop → `PricingExecutor::run_dynamic` with per-worker scratch keyed on stable worker_id), `atx-vol/bench/var_bench.cpp` (resolved_workers surfaced in the JSON/terminal output; wire `dump_counters` into `run_terminal_var`), tests.

**Requirements:**
1. Per-scenario evaluation is pure w.r.t. shared state, so dynamic assignment must not change any output byte. Extend/duplicate the existing thread-invariance test to cover the dynamic path across t1/t2/t4 AND assert frames from the dynamic path are byte-identical to the static path's (pin the refactor, then delete the static path or keep it test-only — implementer's choice, but only one production path remains).
2. Task 6's test 4 (mixed downgraded scenarios) must stay green on the dynamic path.
3. Bench: emit `resolved_workers` (actual pool size) alongside requested; metric denominators use resolved. `dump_counters` output lands in the terminal report.
4. No new config knob — scheduling is an implementation detail under pinned bit-invariance.

**Steps:** invariance tests first (against static path, PASS) → swap to run_dynamic → same tests green unchanged → bench changes → `Var.*` green → clang-format → commit `perf(vol): dynamic scenario scheduling under pinned bit-invariance; bench resolved-worker and counter reporting`.

---

## Explicitly deferred (parked, with rationale — do not implement)

- Held-book aged replay mode (true buy-and-hold P&L of the 7/31 book): methodology feature, L effort, needs position aging/expiry handling across the window. The Task 1 decomposition discloses the distinction.
- Relative-shock / today-anchored scenario mode; multi-year & stressed-VaR scenario sets: L, needs data + methodology sign-off ([gaps] 2, 5).
- Transaction-cost modeling on the restruck roll ([pnl] Important): methodology, needs cost model inputs.
- Production driver/CLI + incremental caching ([gaps] 8, 9): separate product sprint.
- Coverage-rate raise (70.9% exclusions are book-construction screens in the bench fixture, not engine limits): revisit with the driver work.

## Self-review notes

- Every [proj]/[solver] Important maps to Task 2 or 3; [pnl] Criticals map to Task 1; [gaps] statistics/reporting blockers map to Tasks 4-5; [perf] 1-2-4 map to Tasks 7-8; [perf] 3 (measured-gamma slope) intentionally omitted — Medium impact, touches the solver's convergence economics right after Task 7 changes it; parked for a follow-up sprint with fresh counters.
- Type consistency: `VarScenarioStatus::ArchiveError` introduced in Task 2, consumed by Task 4's `to_string` coverage; `diagnostic_flags` introduced Task 3, not referenced later; `VarWeighting` self-contained in Task 5; Task 7's knob independent of Task 8.
