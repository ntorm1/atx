# Brief: task R-8-R-9

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task R-8 (optional): ex-ante risk target

Rule: `L_t = clip(sigma_star / (b x sigma_hat_t), .8 L, 1.25 L)`, sigma_star = 5.0%, b = 1.15 (declared bias),
sigma_hat from `book_variance` on the gross-1 current book, updated every 21 sessions. Files `strategy_nav_v7.cpp:312-384`.

**Acceptance:** realised volatility inside [.8, 1.2] x sigma_star in each year AND dSR not lower by more than one SE.
**Expected [est]:** Sharpe -.01 to -.04; net return up with the target. **Cost:** 1 cell (N 48).

### Task R-9 (optional): capacity frontier

Three cells at theta .03, .04, .05 with NAV 4x, on the final construction. Reported as a frontier. The $1bn book does not
change. **Cost:** 3 cells (N 49 to 51).

