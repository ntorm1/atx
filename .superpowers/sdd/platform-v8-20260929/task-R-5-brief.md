# Brief: task R-5

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task R-5: ADV holding cap (S-6)

**Files:** `atx-impl/src/strategy_target_replay.cpp:360-403`, tests

Rule: after the projection, clip `|desired_i| <= Q x ADV_i / (L x NAV)`, Q = .10; redistribute the clipped mass pro rata
inside the same side, one pass; report the residual breach.

- [ ] **Step 1:** tests `AdvHold.NoNameAboveCapAfterOnePassOrReported`, `AdvHold.SideGrossPreserved`,
  `AdvHold.LargeQIsByteIdentical`. **Step 2:** implement behind `--adv-hold-q .10`. **Step 3:** root runs the cell with
  `--capacity-curve`.

**Acceptance:** net Sharpe at 4x NAV higher AND net Sharpe at 1x not lower by more than one paired SE AND S3 not lower.
**Expected [est]:** -.02 to -.08 at 1x; +.05 to +.15 at 4x. **Cost:** 1 cell (N 45).

