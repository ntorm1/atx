# Brief: task R-1

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

## 9. Waves 2 to 4: research trials

Rules for every R task:

- Parent = the last accepted cell. Construction cells run at L 1.247 unless the task changes it.
- Primary statistic: paired S2 net dSR against the parent on 2020-2023, with the pre-registered bootstrap.
- Acceptance is always "dSR > 0 AND mechanics AND the named mechanical criterion". The mechanical criterion is declared
  in advance and is what gives a sign-level result some meaning.
- Every cell also reports net Sharpe at 4x NAV and the year table.
- A rejected cell is not retried with other parameters in v8.

### Task R-1: composition v8 (S-1 + S-4 + tier re-grade)

**Files:** `atx-impl/src/strategy_ic_composition.cpp:174-196,235-239`, `atx-impl/tools/fit_composition_weights.py:193,1366,1399-1406`, tests

**Rule `ew-theme-std-v1` (the registration):**
1. Per date and theme, take the weighted mean of the theme's signed member ranks. Within-theme weights are proportional to
   the tier score in the registry.
2. Re-rank that theme composite across names with at least one present member (centred tied rank in [-.5, +.5]).
3. Blend = sum over themes of (1 / T) x re-ranked composite. Missing stays neutral; no redistribution inside a theme.
4. Member cap: no member's weight above 1 / (2T); the excess goes pro rata to the other themes.
5. Tier re-grades from the v8 literature, declared before any read: `res_mom_12_1` B+ to B-; `ear` B+ to C+;
   `sue` C+ stays; `ins_opp` B- to C+. No other tier changes. No tier comes from a TRAIN statistic.

- [ ] **Step 1:** tests `CompositionV8.IdentityWithReRankAndCapOffIsEwThemeV1` (byte-identical blend),
  `CompositionV8.OneMemberThemeHasSameDispersionAsOthers`, `CompositionV8.MemberCapRedistributes`,
  `test_tier_weights_sum_to_theme_share`.
- [ ] **Step 2:** implement behind `--composition ew-theme-std-v1`; weights file schema v2 with key `theme_standardise`.
- [ ] **Step 3:** root: identity cell, then the trial.

**Acceptance:** dSR > 0 AND mechanics AND planned turnover per unit gross not higher than the parent's.
**Expected [est]:** net Sharpe 0 to +.05; turnover -5 to -10%. **Cost:** 1 cell (N 41).

