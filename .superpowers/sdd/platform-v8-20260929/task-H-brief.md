# Brief: task H

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

## 10. Wave 5: history and mining (gated)

### Task H-1: era shards (needs OD-3 to read; tooling needs no ruling)

**Files:** `scripts/research_cycle.py` (`roles:` loop), `atx-impl/tools/nav_summ.py` (pooling), `fit_composition_weights.py` (pooled factor returns), tests

Roles E1 2014-2016 and E2 2017-2019, each with a 399-session warm-up and today's geometry. The 4-year role is E3.
Each era runs fields -> u -> w -> nav on shared, role-keyed caches. The fitter and `nav_summ` pool the eras in date order.
Memory per process stays at today's level. E3 stays byte-identical.

- [ ] **Step 1:** tests on the fixture split in two eras: `test_pooled_summary_over_one_era_equals_single`,
  `test_each_era_has_receipt_and_ledger_line`. **Step 2:** implement. **Step 3:** data audit without returns: coverage of
  every field by year 2014-2019, share of delisted names present, first valid date per signal. The audit opens no return.

**If OD-3 is granted:** one pre-registered read of frozen V8-F and of B0c on E1 + E2, with the registered expectation that
Sharpe is lower than on 2020-2023. The read costs 1 trial.

### Task H-2: AuditExact cost measurement

Root measures cold VM seconds of library v7.1 in AuditExact on E3 and the fixture. The date-blocked runner (platform
review P-5b) is planned only if AuditExact costs less than 3x ResearchFast.

### Task H-3: mining verb, built and self-tested (OD-7 to run)

**Files:** create `atx-impl/src/strategy_mine.{cpp,hpp}`, `atx-impl/tools/equity_strategy_mine.cpp`; modify
`atx-engine/src/factory/search_driver.cpp:321`, `op_catalog.cpp:83`, `fidelity.cpp:94`

Glue G1 to G8 of the engine review: role and fields panel adapter lifted from the IC runner; eligibility mask in the search
engines; fitness on the mega IC kernel with the K6 marginal term; literature operators in the catalogue; racing on
instrument strides only; registry persisted per campaign with its chain head copied to the cycle ledger; admission rule
`mined-v1`.

`mined-v1` (declared now, used only under OD-7): promotion needs marginal IC HAC t at or above the Bonferroni value for the
campaign's effective trial count (3.5 at 100, 4.1 at 1,000, 4.6 at 10,000); sign frozen from the discover window; one
confirm read at HAC t 2.0 with Benjamini-Yekutieli p at or below .10; `abs(rho)` at most .70 to every member; all mined
members share one theme.

- [ ] **Acceptance (fixture only):** 3 planted signals promoted, the planted copy rejected by the marginal term, no noise
  expression promoted in 5 seeds; same seed twice gives the same registry chain head at 1 and 4 workers; registry count
  equals evaluated + racing-rejected + screen-rejected.

---

