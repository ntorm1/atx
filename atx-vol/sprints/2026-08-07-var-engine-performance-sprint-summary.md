# VaR Engine Performance Sprint — Summary

**Dates:** 2026-08-02 → 2026-08-07
**Branch:** `var` (worktree `C:\atx-wt\var`), merged to `main` at `8637bdd` (engine) and `9bbabe7` (closeout)
**Plan:** `docs/plans/2026-08-02-var-engine-performance.md` · **Ledger:** `.superpowers/sdd/2026-08-02-var-engine-performance/progress.md` (var worktree)
**Method:** subagent-driven development — fresh implementer per task, spec + quality review per task, fix loops, controller as project manager.

## Goal

Take the historical-simulation VaR engine for American equity options
(`atx-vol`) from its screened scalar baseline (1.922 core-s/scenario on this
host; 2.85 in the original status doc) to ≤ 1.0 core-second/scenario on the
SP100 YTD fixture, without weakening any correctness gate.

## What was built

A cross-sectional batch inverse-delta solver, now the library default
(`OptionDeltaSolvePolicy::CrossSectionalColdConfirm`):

- **`solve_american_delta_batch`** (`contract_projection.{hpp,cpp}`): per
  (scenario × underlier group), a Black-style inverse-delta seed vector (two
  smile-refresh iterations mirroring the scalar solver), then laned cold
  American delta passes via `evaluate_batch(EvalField::FirstOrder, …,
  ColdReference)`, a closed-form-slope Newton pass, secant correction passes
  over a shrinking compacted active set, a hard cap of
  `kMaxBatchDeltaPasses = 8` total laned passes, internal acceptance at
  `tolerance/2`, and a scalar fallback tail in ascending row order.
- **Shared strike resolution** (`var.cpp`): both the aggregate
  (`evaluate_scenario_batched`) and retained-leg (`evaluate_scenario`) replay
  paths consume one `resolve_group_window_cross_sectional` per scenario, so
  aggregate-vs-retained parity holds at the fixture's 1e-9 gate. Any row
  failure downgrades the whole scenario to the scalar
  `FastScreenColdConfirm` route so per-leg statuses stay identical.
- **Determinism:** every accelerant is a function of (reference portfolio,
  base surface) only; cross-date warm-starting is forbidden; thread-count
  bit-invariance and batch composition-invariance are pinned by tests.
- **Bench route** `cross_cold` beside `direct_cold` / `screened_cold` in
  `var_bench.cpp`.

## Correctness gates (all intact, none weakened)

| Gate | State |
|---|---|
| Cold-confirm per strike: abs(abs(cold Δ(K)) − target) ≤ 1e-7, scalar ColdReference oracle | green (solver accepts at tolerance/2 internally) |
| SP100 aggregate vs retained-leg cold oracle, 1e-9 relative | **bit-exact** — every error counter identically zero (sse2) |
| Cold marks only for valuation (prepared/fast marks inadmissible, 4.8 % error) | enforced by explicit `ColdReference` at every call |
| Scenario independence + thread-count bit-invariance | pinned (`Var.CrossSectionalReplayIsBitInvariantAcrossThreadCounts…`) |
| Rejected routes stay rejected (grouped scalar batch-root; prepared marks) | recorded as guardrails in the status doc |

## Performance results

**Citable (5-rep, CV ≤ 5 %, quiet-window protocol, i7-1260P):**

| case | median wall | CV | core-s/scenario |
|---|---|---|---|
| rel (sse2) cross t4 | 26.357 s | 3.68 % | **0.995 — under the 1.0 target** |
| rel-avx2 cross t8 | 15.532 s | 4.96 % | 1.172 |

**Directional (CV-dirty or single-shot):** rel t1 54.0 s → 0.510; rel t8
14.94 s → 1.127; quiet single-shot t8 13.887 s → 1.048; single-shot t1
60.13 s → 0.567. Baseline: screened t8 single-shot 25.464 s → 1.922.
Directionally the route is ~1.6–1.9× the screened baseline at equal thread
counts (25.8 s → 13.9–17.1 s at t8), but the CV gate is only satisfied at
the two rows above.

**Why t8 misses 1.0 (Task 9 profile, deterministic counters):** delta-solve
is 55.8 % of thread-summed core-time and the two cold mark passes are a
44.1 % floor no solver accelerant touches; the `wall×8/106` metric charges
SMT/E-core threads as full cores on the 4P+8E part. Accelerants measured and
**reverted** on evidence: reference-anchored seed (4.66 passes/row, 9.6 %
fallback), single smile-refresh seed (4.32 passes/row). Kept: pass budget
6 → 8 (fallback 2.21 % → 0.43 %, ≈ −5.5 % work).

## P&L validation (Task 11)

Cross-route trace `sp100_dispersion_ytd_pnl_cross.tsv` + regenerated
`sp100_dispersion_ytd_cumulative_pnl.png` (`C:\atx\artifacts\var\`):
cumulative **+$6,546,715.73** vs the accepted screened trace's $6,546,716 —
delta **$0.27** over the year, max per-scenario |Δpnl| $0.41; 106 scenarios,
12 history breaks, max drawdown −$1.10 M; producing run passed the 1e-9
oracle gate with all parity counters zero.

## Notable incidents

1. **Task 8 "engine defect" that wasn't.** After the default flip, the bench
   fixture's 1e-9 gate failed on every route. Root cause was the fixture,
   not the engine: it reused retained-leg frames replayed on the superset
   book as the oracle for the filtered book, and under whole-scenario
   fallback per-leg results are composition-dependent (89/106 scenarios
   mismatched on fingerprints). Fixed by a fresh same-book retained replay
   (`5630362`), regression-pinned at unit scale.
2. **Ambient host load** repeatedly contaminated wall-clock numbers (CVs to
   41 %). All keep/revert decisions were made on bit-deterministic pass
   counters instead; only CV-clean rows are cited.
3. **Merge restoration:** `main` had dropped the three VAR build
   registrations as dangling (`9e03dcc`) while the sources lived only on the
   var branch; the merge (`8637bdd`) restored them.

## Validation state

Focused suites green on the merged tree (`^Var\.` 21/21,
`^ContractProjection\.` 17/17); hygiene-preset build clean; clang-format
clean over every plan-touched file. Full `atx_vol_fast`/`atx_vol_slow`
sweeps intentionally not run (user directive: focused groups). Deferred
failures triaged: `VolUmbrella.TierAIsClosedUnderInclusion` was a
pre-existing fork-point failure fixed upstream (`be98049`, passes on main);
`SurfaceV2Provenance.ValidationFallbackAdmissionRecordsTheServedFamily`
fails identically on the fork tree and current main — pre-existing
convex-dense admission defect in the fitting/QP domain, no overlap with any
sprint file, left to that workstream.

## Commit map (var branch)

`46dc152` baseline import → `6d6443e` policy enum + helpers → `67426a3`
batch solver core → `b6728c9` fallback tail → `f28dfcd` plan doc → `6560752`
aggregate path → `5ac6257` retained-leg shared resolver → `c0fcc0f` default
flip → `f70cded` fingerprint-check fix → `0ddcfc8` cross_cold bench route →
`5630362` fixture-oracle fix → `f7087c2` pass budget 6→8 → `aed4975` status
doc rewrite.

## Remaining work (ledger resume points)

- Quiet-host re-runs for the 7 CV-dirty Task 10 cases (rel t1/t8/t16,
  screened t8, rel-avx2 t1/t4/t16) until CV ≤ 5 %, then the final Task 10
  table.
- Full `atx_vol_fast` / `atx_vol_slow` label sweeps.
- Final whole-branch review (most-capable-model, over merge-base..HEAD,
  pointed at the ledger's deferred-minors list).
- Speculative performance ideas (status doc "Shipped design"): mark-pass
  cost reduction, further ISA work, E-core-aware partitioning.
