# Brief: task V

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task V-1: validation kit. Task V-2: hidden-block gate (tool only)

**Files:** move `.superpowers/sdd/mega-alpha-20260926/studies/{nav_summ,backtest_integrity}.py` to `atx-impl/tools/`;
create `atx-impl/tools/holdout_gate.py`; tests

- [ ] **V-1 step 1:** tests `test_legacy_n37_numbers_reproduced` (the moved scripts give `mega-nav-v71-summ-n37.json`
  byte for byte), `test_year_table`, `test_bundle_verdict`, `test_origin_class_in_ledger_line`,
  `test_variance_from_rerun_cells`.
- [ ] **V-1 step 2:** implement: window read from `research_window.py`; pre-registered bootstrap seed 20260929 and block
  21 for the existing circular block bootstrap and Ledoit-Wolf test; `--bundle BASE FINAL` cumulative paired test; year
  table per cell; ledger line fields `origin`, `window_id`; defect rule (Appendix A, rule 7).
- [ ] **V-2:** `holdout_gate.py --deploy MANIFEST --thresholds FILE --owner-ruling FILE` returns two bits, `pass_2024` and
  `pass_2025_onward`, and nothing else. It refuses without an owner ruling file. It is built and tested on the fixture.
  **It is not run on real data in this sprint.**

