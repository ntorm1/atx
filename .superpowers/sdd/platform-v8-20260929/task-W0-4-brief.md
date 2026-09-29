# Brief: task W0-4

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task W0-4: baseline cells

| cell | book | role | protocol | purpose | N after |
|---|---|---|---|---|---|
| B0a | v7.1, ew-theme-v1, aim-partial-v5, L 1.247 | lo1, 2020-2023 | as v7 | continuity: the accepted book on the new window | 38 |
| B0b | same | lo3, 2020-2023 | as v7 | V7-F restated on the new window | 39 |
| B0c | winner of B0a / B0b | same role rebuilt with `--delisting-returns` | delisting returns on; warm start from the role's pre-2020 sessions | protocol correction (C-1, C-7); the v8 baseline | 40 |

- [ ] **Step 1:** declare in `v8-prereg.md` that B0c is the baseline whatever its sign, and that B0b is accepted on paired
  S2 net dSR > 0 against B0a and mechanics.
- [ ] **Step 2:** lane D implements the warm start as its first commit (task D-0): NAV flag `--warm-start-sessions 60` in
  `strategy_nav_replay.cpp` (decide from session `score_begin - 60`, score from `score_begin`). Tests
  `NavWarmStart.FlagOffIsByteIdentical`, `NavWarmStart.GrossAtFirstScoredSessionWithin5PctOfSteadyState`. Root builds and merges it
  before B0c.
- [ ] **Step 3:** run B0a, B0b, B0c through `research_cycle.py run scripts/specs/v8/base-*.json`.
- [ ] **Step 4:** report each cell with a year table (2020, 2021, 2022, 2023: net Sharpe, return, volatility, turnover,
  cost per dollar) and the Appendix A block. Re-run on the 4-year role every ledgered v7 cell that is reproducible from a
  spec or from its recorded argv (v6.1, v7.0, v7.0-lo3, C1-C3, spo-v1, spo-v2), for the cross-trial variance of OD-4. These re-runs are the same trials on a longer
  window and do not raise N.

**Acceptance:** three cells ledgered; mechanics pass (gross, net, turnover limits as v7); B0c named as parent of R-1.

---

