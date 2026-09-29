# Brief: task R-4

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task R-4: rank hysteresis

**Files:** `atx-impl/src/strategy_target_replay.cpp:184-205,226-280`, `strategy_target_replay.hpp`, tests

**Rule `hold-band-v1` (the registration):** a name's desired weight moves only when its centred rank of the blend leaves a
band around the rank at which its current aim was set. Band half-width b = .10 in centred-rank units (rank range 1.0).
Inside the band the previous desired weight is kept. Names that leave the universe follow the exit rule unchanged.

```cpp
// desired_prev[i], rank_set[i]: state carried per name. rank_now[i] in [-0.5, +0.5].
for (usize i = 0; i < n; ++i) {
  const bool moved = !std::isfinite(rank_set[i]) || std::abs(rank_now[i] - rank_set[i]) > band;
  if (moved) { desired[i] = desired_fresh[i]; rank_set[i] = rank_now[i]; }
  else       { desired[i] = desired_prev[i]; }
}
// then the existing demean, neutralise and gross-1 rescale
```

- [ ] **Step 1:** tests `HoldBand.ZeroBandIsByteIdenticalToV5`, `HoldBand.NameInsideBandKeepsDesired`,
  `HoldBand.StateSurvivesMissingDay`, `HoldBand.DecideVerbCarriesState` (the daily decide path reads `rank_set` from the
  holdings file).
- [ ] **Step 2:** implement behind `--hold-band .10`. **Step 3:** root: identity cell with band 0, then the trial.

**Acceptance:** dSR > 0 AND mechanics AND turnover at least 15% lower.
**Expected [est]:** turnover -20 to -40%; gross Sharpe -2 to -5%. **Cost:** 1 cell (N 44).

