# Brief: task R-6

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task R-6: target-tracking optimiser `spo-v3` (S-8)

**Files:** `atx-impl/src/strategy_spo.cpp:976-1050,1020,1164`, `strategy_spo.hpp:141`, `strategy_nav_v7.cpp:737-739`, tests

**Problem (the registration):** minimise over w
`(gamma / 2) (w - w_aim)' Sigma (w - w_aim) + sum_i [s_i |w_i - w0_i| + eta_i |w_i - w0_i|^(3/2)] / H + sum_i b_i max(-w_i, 0)`
subject to net and beta limits, the 1% ADV trade limit and the locate mask. `w_aim = L x desired` from the accepted rule.
Sigma is atx-risk-v1.1 on the 4-year role. `gamma = S_prior / sigma_aim`, `S_prior` = 1.0 declared, `sigma_aim` the aim's
ex-ante volatility at the first decision. H = 20. The gross cap is set to the sanity bound 2 x L so it is slack. No alpha
vector is fitted.

- [ ] **Step 1:** tests `SpoV3.ZeroCostNoLimitsReturnsAimTo1e8`, `SpoV3.GrossCapIsSlackOnFixture`,
  `SpoV3.V1AndV2DigestsUnchanged`, `SpoV3.ReportsTrackingErrorAndShareAtTradeLimit`.
- [ ] **Step 2:** implement `--rule spo-v3 --spo-alpha implied-aim`. **Step 3:** root: build the risk model on the 4-year
  role (risk verb, new pin); identities (flag off; spo-v1 and spo-v2 digests); read the tripwire before any return; run.

**Acceptance:** dSR > 0 AND mechanics AND cost per traded dollar not higher AND tripwire clear.
**Expected [est]:** net +.03 to +.08 at 1x, more at 4x; cost per dollar -10 to -20%. **Cost:** 1 cell (N 46). This is spo
trial 3; ruling spo-b of v7 closed the line for v7 only.

