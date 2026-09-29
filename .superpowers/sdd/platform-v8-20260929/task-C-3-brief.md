# Brief: task C-3

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task C-3: reuse for SEC and holdings field modules

**Files:** `atx-engine/tools/prepare_research_fields.py:501-505,525-528,2917-2923`, `research_fields_sec.py`,
`research_fields_holdings.py`, tests

- [ ] **Step 1:** tests `test_reuse_self_reports_63_reused`, `test_one_producer_edit_recomputes_one_field` (an edit in
  `build_ftd` recomputes only `ftd_shares_ratio21`).
- [ ] **Step 2:** each module exports `PRODUCERS = {group: (entry functions,)}`; `producer_fingerprints` runs over that
  module's AST; the stage manifest SHA is the source check.

**Target:** a one-field build 59-121 s -> about 10 s (est.).

