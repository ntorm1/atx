# Brief: task B-1

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task B-1: incremental u pass

**Files:** `atx-impl/src/strategy_ic_runner.cpp`, `atx-impl/tools/equity_strategy_ic.cpp`, `atx-impl/tests/strategy_ic_runner_test.cpp`

**Interfaces:** produces flag `--no-composition`; summary key `"composition": "skipped"`. Consumed by A-3 (`run --screen`).

- [ ] **Step 1:** tests `NoComposition.SkipsBlendAndCombinedRows`, `NoComposition.HitWithIcResultIsNotLoaded`,
  `NoComposition.MemberRowsByteIdenticalToDefault`, `StrategyIcRunner.CacheMissOnRoleChange`.
- [ ] **Step 2:** implement: skip `IcComposition`, `__combined__` rows, planned targets, `--save-combined`; refuse
  `--no-composition` together with `--composition-weights`.
- [ ] **Step 3:** root: v7.1 u pass with the flag; `orientations.json` and member rows of `train_daily_ic.csv`
  byte-identical to `mega-v71-train-u-1`.

**Target:** u 23.0 s -> about 6 s (est.).

