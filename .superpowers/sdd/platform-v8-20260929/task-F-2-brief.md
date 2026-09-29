# Brief: task F-2

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task F-2: marginal IC verb

**Files:** create `atx-impl/src/strategy_marginal_ic.{cpp,hpp}`, `atx-impl/tests/strategy_marginal_ic_test.cpp`; modify
`atx-impl/tools/equity_strategy_ic.cpp`

**Interfaces:** `atx-equity-strategy-ic marginal --candidate-cache DIR --library L --pool combined.json [--themes]
--output DIR` writes K6. For each date: residualise the candidate's centred rank on the library composite and the theme
composites (at most 11 regressors) with `combine/orthogonalize.hpp::residualize_signal`; rank IC of the residual against
the h 21 label; HAC t (Bartlett, lag 21).

- [ ] **Step 1:** tests `MarginalIc.PoolPlusNoiseHasZeroMarginalWithin2Se`, `MarginalIc.PlantedOrthogonalRecovered`,
  `MarginalIc.StreamsByDateUnder600MiB`.
- [ ] **Step 2:** implement, streaming by date over cached payloads. **Step 3:** root: run on the 48 v7.1 candidates; the
  file is an input to G-1.

