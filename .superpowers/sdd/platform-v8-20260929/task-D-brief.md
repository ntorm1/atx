# Brief: task D

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task D-1: NAV stage timers, log-return ring, construction grid. Task D-2: exposures verb

**Files:** `atx-impl/src/strategy_nav_replay.cpp:1014-1112`, `strategy_price_exposures.cpp:107-146`,
`atx-impl/tools/equity_strategy_targets.cpp`, tests

- [ ] **D-1 step 1:** tests `NavTimers.SumWithin5PctOfWall`, `LogRing.ExposuresBitIdenticalToWindowRecompute`,
  `ConstructionGrid.EachVariantEqualsItsStandaloneRun`.
- [ ] **D-1 step 2:** timers in `summary.json` (load, hash, exposures, construction, books, write); a per-session ring of log
  returns so each session is logged once; `--construction-grid grid.json` sharing loaded inputs and exposures across
  variants; books on a DetPool after the shared decision.
- [ ] **D-1 step 3:** root: the v7.1 cell's ten files byte-identical; timers reported.
- [ ] **D-2:** verb `exposures` per contract K2; test `Exposures.FitterFactorsEqualTo1e12`. The Python neutraliser is
  deleted only after the fitter reads the export and admission.json is unchanged.

