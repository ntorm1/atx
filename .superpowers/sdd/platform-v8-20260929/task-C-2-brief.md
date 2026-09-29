# Brief: task C-2

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task C-2: report-only columns at the traded horizon

**Files:** `atx-impl/tools/alpha_report_card.py`, `atx-impl/tools/fit_composition_weights.py`, tests

**Interfaces:** adds to each card `ic_theta = sum over h of theta (1 - theta)^(h-1) m(h)`, theta .05, h 1..63, m(h) the
lagged one-day rank IC already in the card; adds to the admission table the column `f_theta` (factor return of the
theta-averaged sleeve book, HAC t). Also copies the K6 marginal IC columns into the card. All report-only.

- [ ] **Step 1:** tests `test_ic_theta_matches_hand_value`, `test_f_theta_is_report_only` (admission verdicts unchanged
  with the column on).
- [ ] **Step 2:** implement; declare in `v8-prereg.md` before the first read that neither column gates or selects.

