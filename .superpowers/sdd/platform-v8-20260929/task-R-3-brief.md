# Brief: task R-3

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task R-3: persistence gain on the aim (S-2 option A)

**Files:** none (code exists: `fit_composition_weights.py:1180-1186,1409-1418`); `scripts/specs/v8/aim-gain.json`

Rule: `ew-theme-aim-v1` gains, g_k = theta x sum over j of (1 - theta)^j rho_k(j), from rank autocorrelation only, applied
on top of the R-1 weights; theta .05; gains clipped to [.05, 1] as coded.

- [ ] **Step 1:** test `test_aim_gain_composes_with_theme_std`. **Step 2:** root runs the cell.

**Acceptance:** dSR > 0 AND mechanics AND net Sharpe at 2x NAV not lower AND turnover lower.
**Expected [est]:** net -.05 to +.05 at 1x, positive at 2x and above; turnover -15 to -30%. **Cost:** 1 cell (N 43).

