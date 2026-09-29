# Brief: task B-3

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task B-3: move-only split of the IC runner and per-candidate plan rows

**Files:**
- Modify: `atx-impl/src/strategy_ic_runner.cpp` (2,568 lines) split at the seams in platform review P-13 into
  `strategy_ic_library.cpp` (library and field plan), `strategy_ic_admission.cpp`, `strategy_ic_signal_cache.cpp`,
  `strategy_ic_result_cache.cpp`, `strategy_ic_runner.cpp` (score_role and CLI); one header `strategy_ic_detail.hpp`
- Modify: `atx-impl/CMakeLists.txt` source list and the `/O2` list at `:87-94`

**Interfaces:** produces contract K1.

- [ ] **Step 1:** test `StrategyIcRunner.PlanOnlyPrintsCandidateRows`: a 3-candidate library yields 3 rows with the fields of K1.
- [ ] **Step 2:** move code without edits; update the `dsl_vm_sources` and `ic_result_sources` pins in the same commit.
- [ ] **Step 3:** add the `candidates[]` array to the `--plan-only` JSON.
- [ ] **Step 4:** root: build, 46 runner tests, e2e fixture SHAs unchanged, v7.1 u pass byte-identical.

