# Brief: task A-3

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task A-3: research_cycle plumbing

**Files:** `scripts/research_cycle.py:100-102,591-635,936-952`, `scripts/run_bounded_research.py:91-93`, tests

- [ ] **Step 1:** tests `test_stage_inputs_map_to_flags` (the ten L9 inputs), `test_clean_check_pathspec` (a file written
  under `.superpowers/` mid-run does not stop the cycle; an edit under `atx-impl/` does), `test_dsr_n_from_ledger`,
  `test_every_phase_has_receipt`, `test_ref_skipped_when_fields_unchanged`, `test_no_git_only_outside_repo`,
  `test_build_key_resolves_exe_dir`.
- [ ] **Step 2:** implement: INPUT_KEYS `sec_identity_bridge`, `earnings_calendar`, `insider`, `sec_filings`, `thirteenf`,
  `ftd`, `regsho_threshold`, `security_master`, `short_volume_ext`, `reuse_fields`; clean check scoped to `atx-core`,
  `atx-tsdb`, `atx-engine`, `atx-impl`, `scripts`, CMake files; `dsr_n: "ledger+1"`; `build: "equity-rel" | "equity"`;
  cache and fit roots derived from the role SHA when the spec omits them; `out_root` spec key; the ledger copied into the
  sprint directory after each cycle.
- [ ] **Step 3:** root: `run scripts/specs/v71.json --suffix p8` reproduces the v7.1 cell's ten daily and events files.

