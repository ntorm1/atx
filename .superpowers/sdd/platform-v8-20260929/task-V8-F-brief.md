# Brief: task V8-F

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task V8-F: cumulative test and freeze

- [ ] **Step 1:** the last accepted cell is V8-F. Run `nav_summ.py --bundle B0c V8-F`.
- [ ] **Step 2:** freeze gate, declared now: S2 net Sharpe on 2020-2023 at least 1.0; mechanics; cumulative paired dSR
  against B0c above 0 with bootstrap p below .10; cell-count DSR at least .95 under OD-4. The effective-N DSR, PBO, PSR
  and MinTRL are reported beside it.
- [ ] **Step 3:** if the DSR gate is unmet, the scorecard says so and names OD-3 (history) as the lever.

---

