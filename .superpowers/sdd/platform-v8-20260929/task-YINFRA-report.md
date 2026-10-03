# Task YINFRA report: one research wave, one command (lane YINFRA, 2026-10-02)

Worktree `C:/atx-wt/pool-15`, branch `feat/platform-v8-yinfra-20261002`, base `798d3b23`. Python only; no C++ built or
written; no real data run; nothing under `atx-db/`; no subagents. Every stage is tested on synthetic fixtures (a git
root per test, sessions 2020-2021, fake research_cycle / add-alpha / bounded runner / bundle processes).

## Commits

| item | commit | content |
|---|---|---|
| 1 wave manifest + driver | `487faf0b` | `atx-engine/tools/stage_chain.py` (+ test), `scripts/research_wave.py`, `wave_context.py`, `wave_stages.py`, `wave_steps.py`, `wave_rules.py`, `wave_manifest.py`, `wave_readers.py`, `wave_result.py`; `research_cycle.py` verb `wave`; `research_add_alpha.py --save-plan` |
| 2 candidate queue | `00722108` | `scripts/wave_queue.py`; verb `candidates`; preflight queue check; record stage sets statuses |
| 3 book scoreboard | `d610985f` | `scripts/wave_scoreboard.py`; verb `scoreboard` |
| 4 speed | `4af3ce8a` | manifest `speed` (reuse_screen_marginal, screen_first), replacing-wave pool-only marginal, `scoreboard --timings` |
| 5 plan + report | (this commit) | `docs/plans/2026-10-02-v8y-research-loop.md`, this report |

## 1. What was built (interfaces as coded)

**Generic** `atx-engine/tools/stage_chain.py`: `Stage(name, run, inputs, plan)`, `Chain(name, stages, state_dir)` with
`run(ctx, until=, log=)`, `state(ctx)`, `plan(ctx)`. Receipts `<state>/receipts/NN-<stage>.json` (written once);
`inputs` recorded and re-computed on every run: unchanged -> skipped, changed -> `ChainError` code 3 (never re-run
over a receipt); each stage's inputs include the SHA-256 of the previous ok receipt; a `StageError` writes
`NN-<stage>.failed-k.json` and the next run retries. Standard library only.

**Wave** `research_cycle.py wave {plan,run,status} MANIFEST [--root R] [--until STAGE] [--dry-run]`
(`scripts/research_wave.py`). Manifest `atx.research-wave/v1` (`scripts/wave_manifest.py`): wave, parent {spec,
library}, fields {dir, manifest_sha256}, library, candidates[] (id, dsl, dsl_sha256, theme, tier, prior_sign, citation,
origin, hypothesis, kind add|replace, replaces, rescreen, removes, fields, exception, ...) or rule_cell {template,
template_sha256, name, constants{set, flags}}, acceptance {rule, printed[]}, sign_rule, gross_match, budget {id,
admission_cap + admission_cycle_prefix, construction_cap}, ledger, expect {n_before}, out_dir, record {copy_to},
speed, b_suffix. Stages (`scripts/wave_stages.py`): preflight -> register -> screen -> spec -> run -> match -> verify
-> judge -> record (table in the plan doc, section 2). Named rules (`scripts/wave_rules.py`): sign `pm7-35`;
acceptance `pm7-34`, `v8-prereg-5`; criteria `turnover-per-gross-not-higher`, `capacity-4x-higher`, `cost-bps-lower`;
gross match `pm6-6` (tolerance .005, one correction), `none`; mechanics `v8-mech`. Readers (`scripts/wave_readers.py
mechanics|book --nav NAME=DIR --output FILE`) run under the bounded runner; `mechanics` writes construction keys only.
Result `atx.wave-result/v1` (`scripts/wave_result.py`) + `wave-log.md` (the ready-to-paste log section).

**Queue** `research_cycle.py candidates {new,validate,list,pin,emit}` (`scripts/wave_queue.py`), files
`scripts/specs/v8/candidates/<id>.json` (`atx.wave-candidate/v1`): status proposed -> pinned -> screened | admitted |
dropped | in-book, wave, history. Refuses a duplicate DSL hash (queue or alpha registry under another id), an id
registered with another DSL, a second variant of one hypothesis without `ruling`; `emit --head H --select ids --output
M [--after wave-result.json]` writes a validated manifest of pinned candidates (parent and N from the previous result).

**Scoreboard** `research_cycle.py scoreboard [--results GLOB ...] [--ledger P] [--markdown F] [--json F] [--timings]`
(`scripts/wave_scoreboard.py`): reads wave-result.json files and the ledger only; prints the accepted lineage and every
wave (S2 net SR, net and gross annual, 4x net SR, tau, max DD, N, dSR, bundle p, ledger DSR); checks each cell's ledger
line and s2_net_sr (exit 1 on a mismatch; 2 on conflicting copies of one wave).

## 2. How root verifies

- Tests: `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests` and
  `... atx-engine/tools/test_stage_chain.py` (results in section 6).
- Identity (flag-absent): no existing command changed. `research_cycle.py` gains three dispatch lines per verb (`wave`,
  `candidates`, `scoreboard`) before the argparse of plan/run/status/lock (its docstring and `--help` unchanged);
  `research_add_alpha.py` gains `--save-plan`, and `test_add_alpha_save_plan_is_opt_in` asserts that without it the
  files add-alpha writes are the same set with the same bytes. Check with `git diff 798d3b23 -- scripts/research_cycle.py
  scripts/research_add_alpha.py` (additions only).
- Dry run on the real tree (reads specs only): write a manifest (example in the plan doc), then
  `python scripts/research_cycle.py wave plan <manifest>`; checked here against `x-theme-erc-gm.json` as parent: every
  stage's argv printed, nothing created.

## 3. Commands root will use

```
python scripts/research_cycle.py candidates pin --id ID [--id ID ...] --by PM --ruling R
python scripts/research_cycle.py candidates emit --head H.json --select a,b,c --output scripts/specs/v8/waves/W.json --after build-equity/waves/<prev>/wave-result.json
git add scripts/specs/v8/waves/W.json && git commit -m "wave W: manifest"
python scripts/research_cycle.py wave plan scripts/specs/v8/waves/W.json
python scripts/research_cycle.py wave run  scripts/specs/v8/waves/W.json [--until screen]
python scripts/research_cycle.py scoreboard --timings
```

## 4. Speed findings (evidence: integration-log.md)

1. Marginal IC verb twice per wave with a drop (report only): X-2 141.1 + 134.6 s (lines 5622, 5647), X-3 175.5 +
   167.0 (5723, 5741), X-4 157.7 + 143.8 (5814, 5830): 57-59% of the phase wall. **Implemented** `reuse_screen_marginal`
   (the b cell carries the screen's rows; saves 134.6-167.0 s per such wave). C++ design: `--candidates` subset.
2. NAV replays: calibration + matched (X-3 41.6 + 45.5, 5744-5745; X-5 45.1 + 45.4; X-6 47.1 + 46.7) or nav + ref (X-2
   41.5 + 43.3, 5649-5651). Nothing safe in Python beyond resume (the calibration is the cell when matched; ref skipped
   on equal fields). C++ design: in-process `--calibrate-gross` without the capacity books; NAV stays Debug (Release
   differs by one ULP, 3359-3365).
3. IC passes on the Debug build: u twice + w (X-3 16.2 + 26.6 + 34.7; X-4 18.2 + 26.3 + 41.5). **Implemented**
   `screen_first` (the b library's u pass is the screen's, without the unread blend: X-3's b u pass was 26.6 s vs the
   screen's 16.2 s). Recommendation (no code): the Release IC exe is byte-identical for u and w and 59% / 82% faster
   (3344-3358); a cell spec can name `exes.ic` on `build-equity-rel` with `exes.nav` on Debug.

## 5. Deviations, cross-lane edits, open risks

- Deviations: the brief's stage list gains `register` (split from screen so a failed screen resumes without re-running
  add-alpha) and `match` (gross matching between run and verify). The driver commits the files a stage writes (exactly
  those paths) because the bounded runner refuses a dirty code pathspec; commit messages carry no trailer (tool commits).
  The replacing-wave pool-only marginal (PM6-8 (i), PM7-32) is applied by code in register (the integrator's X-2 / X-4
  hand fix). The mechanics reader reads the NAV's accounting checks from the summary's `accounting_checks` block
  (`strategy_nav_replay.cpp` risk_json); the book reader's gross of cost is net + annualised |trade cost|, |borrow|,
  |long financing| (the integrator's cellstats arithmetic: 4.64 + .74 + .34 + .20 = 5.92 for X-2).
- Cross-lane edits: `scripts/research_cycle.py` (3 dispatch blocks), `scripts/research_add_alpha.py` (`--save-plan`).
- Risks: (a) not run on real data: the first real wave should be run stage by stage (`--until`) with root reading each
  receipt; (b) the seal scan stops on any date token at or after 2024-01-01 in a run log, so a tool that prints today's
  date would stop the wave (none did in X batch 1, per the log's scans); (c) the budget counts admission lines by cycle
  prefix, and a re-screen-only wave's lines count like additions (as the ledger records them); (d) `max_rho_member` in
  carried marginal rows may name a dropped string (labelled in the result); (e) the capacity 4x row is read as the row
  of multiple 4 (the primary book's when several): confirm on the first real `capacity_curve.csv`; (f) the readers and
  the record stage use `nav_summ` / `backtest_integrity` as they are; a change there changes what the wave reads.

## 6. Tests

New (31): `scripts/tests/test_research_wave.py` (15), `test_wave_queue.py` (4), `test_wave_scoreboard.py` (3),
`test_wave_speed.py` (4), `atx-engine/tools/test_stage_chain.py` (5).

- `scripts/tests` (whole, no `ATX_EQUITY_BIN`): **273 passed, 4 skipped, 0 failed** (424.7 s) = the base's 247 passed /
  4 skipped + the 26 new; the base run here before any change: 247 passed, 4 skipped.
- `atx-engine/tools/test_stage_chain.py` + the four new scripts files after the last edit: **31 passed**.

## 7. Review fixes (review-yinfra.md, MERGEABLE_WITH_FIXES; 7 MAJOR + 11 MINOR, in order)

Every finding was read against its cited lines and confirmed; none is contested. New tests:
`scripts/tests/test_wave_review.py` (MAJOR) and `scripts/tests/test_wave_review_minor.py` (MINOR).

| # | finding | commit | test(s) |
|---|---|---|---|
| 1 | budget = hand count | `fbc87f2c` | `test_x7_passes_preflight_on_the_real_ledger_format_25_of_33` (real admission-line layout: X-2 5, X-3 8, their b / -gm cells, X-4's 9 and X-4b's 3 re-screens with `libraries/v8x4*.json` `rescreens`, v80 lines; X-7's 12 strings: used 13 + new 12 = 25 of 33, passes), `..._over_its_cap_is_refused_with_the_hand_numbers`, `test_new_trials_leave_out_rescreens_other_origins_and_trials_already_ledgered`, `test_a_fresh_state_dir_after_the_screen_counts_no_string_twice`; + `d4b28b5d` `test_the_driver_counts_trials_by_the_gates_own_trial_id_rule` |
| 2 | sign rule on mixed waves | `4fcd8dde` | `test_a_mixed_wave_keeps_its_rescreen_admitted_with_the_prior_sign`, `test_a_string_without_an_admission_row_stops_the_screen`; `test_sign_rule_pm7_35_as_ruled` (no row -> RuleError) |
| 3 | commit only the expected set | `64bd453c` | `test_a_local_edit_between_stages_is_refused_not_committed`, `test_a_file_the_stage_does_not_write_blocks_its_commit`, `test_a_retry_after_the_stage_failed_commits_exactly_its_files` |
| 4 | seal scan covers every log | `9e6057ea` | `test_the_screen_librarys_logs_are_scanned_in_a_b_library_wave`, `test_every_command_console_is_kept_and_scanned`, `test_the_record_stage_rescans_after_the_judge_before_the_hidden_data_line`, `test_the_hidden_data_line_counts_the_records_scan` |
| 5 | predecessor digests, binding | `2697c3e1` | `test_a_library_spec_edited_after_register_is_never_screened`, `test_a_file_a_done_stage_recorded_that_moves_is_stale_on_resume`, `test_verify_compares_the_navs_binding_with_the_cell_spec` |
| 6 | marginal reuse | `0116eef0` | `test_carried_rows_hold_only_the_per_row_fields`, `test_reuse_is_off_when_the_screens_marginal_mode_differs` |
| 7 | out_dir / copy_to | `b74f4c46` | `test_out_dir_and_copy_to_inside_the_code_pathspec_are_refused`, `test_a_manifest_with_its_state_in_the_code_pathspec_never_runs` |
| 8, 9 | date forms, allow list; guard lines, `--seal-allow` | `c85bf157` | `test_the_scan_reads_compact_year_and_quarter_forms`, `test_seeds_and_guard_refusals_are_classified_not_hits`, `test_a_seal_allow_ruling_passes_verify_and_is_carried_to_record` |
| 10 | binding of the last completed NAV attempt | `cadd0b53` | `test_verify_reads_the_binding_of_the_last_completed_nav_attempt` |
| 11 | any exception -> failed receipt; lock | `881a9ea9` | `test_stage_chain.py`: `test_any_exception_leaves_a_failed_receipt_with_code_4`, `test_a_second_run_of_one_state_dir_is_refused_while_the_lock_is_held` |
| 12 | rule wave resumes after `lock --write` | `7245cfa0` | `test_a_rule_wave_resumes_after_lock_write_rewrote_its_cell` |
| 13 | `--root` to research_cycle / add-alpha | `77e5114f` | `test_research_cycle_and_add_alpha_get_the_waves_root` |
| 14 | bind daily / capacity series | `1a350ebc` | `test_readers_and_bundle_bind_the_daily_series_and_capacity_curve`, `test_a_bundle_whose_run_did_not_bind_the_series_now_is_refused` |
| 15 | kept, not admitted -> SCREENED | `50319bf0` | `test_wave_queue.py::test_a_kept_string_the_gate_did_not_admit_is_screened_not_admitted` |
| 16 | plan() uses the input diff | `2434f296` | `test_stage_chain.py::test_plan_marks_a_stale_receipt_not_done` |
| 17 | hand heading + budget line | `7732a3dc` | `test_the_log_section_has_the_hand_heading_and_the_budget_line` |
| 18 | `--save-plan` stdout compared | `be696053` | `test_research_wave.py::test_add_alpha_save_plan_is_opt_in` |
| PM8-12 | split by stage; call, do not re-implement | `5c21fd2a`, `d4b28b5d` | the same 56 wave tests; trial-id rule test above |

Readings to note (each inside the finding's fix, none a dissent):
- #1: a re-screen is a line whose candidate is in its cycle library's `rescreens` (cycle_admission writes no flag; a
  `-gm` cycle reads its library's); the ledger already drops a b / -gm cell's repeated trial_id, so X's v8x lines are
  5 + 8 + 9 = 22 (the review's 22 + 12 = 34); the new code counts 13. `budget.admission_origin` is optional.
- #3: every committing stage refuses a dirty code pathspec at its start, file by file (`-uall`); the one exception is a
  retry after that stage's own failed receipt, which may find only the files that stage writes (otherwise a failed
  add-alpha could never resume without hand cleanup). Anything else, e.g. a local edit to research_add_alpha.py, exits 3.
- #5: register declares no inputs of its own: its predecessor's digests are preflight's inputs, which the chain
  re-checks first on every run. A pending stage is checked against the predecessor's record, a done one also against
  its own receipt.
- #9: refusal lines are classified (every attempt is still scanned: a failed attempt may have read data first): the
  seal date itself on a line naming the seal is a "seal reference", any other date there is a hit. The ruling is the
  CLI `--seal-allow TOKEN=RULING` (recorded in the verify receipt, honoured by record); no manifest field.
- #14: every `daily_*.csv` of each NAV dir (all scenarios) and `capacity_curve.csv` are bound; the bundle check reads
  the last completed bundle run's receipt.
- Section 5 risks (b), (c), (d) are closed by #8/#9, #1 and #6.

Files over ~400 lines (PM8-12): none. `wave_stages.py` (824 after the fixes) is split by stage: `wave_stage_preflight.py`
191, `wave_stage_library.py` 201, `wave_stage_cell.py` 151, `wave_stage_record.py` 208, `wave_stage_util.py` 87,
`wave_stages.py` 54 (the table); the largest new file is `wave_fixture.py` 349.

Tests (final code, `d4b28b5d`): `scripts/tests` (whole) **see the final line below**; `atx-engine/tools` (whole):
**308 passed, 6 subtests passed** (357.0 s, at `5c21fd2a`, after the last stage_chain change).

## 8. Core vs wrapper (Ruling PM8-12)

No C++ was written in this pass (lane rule). Every function in the new modules that computes rather than orchestrates:

| function (module) | what it computes | verdict |
|---|---|---|
| `sign_pm7_35`, `screen_decision` (wave_rules) | PM7-35 keep / drop table over the fit's admission rows (status, runner sign, prior) | a rule, no statistic. Belongs with the admission step that computes `status` / `sign_agrees` (today `atx-impl/tools/fit_composition_weights.py`; C++ owner once ported: atx-engine `factory` admission) as a decision column the driver reads. Wrapper until then. |
| `mechanics_check` (wave_rules) | the v8-mech limits on the reader's keys | a limit table over engine outputs. C++ owner: `atx-impl` `strategy_nav_replay` summary (it already emits `meets_daily_turnover_*`; add the leverage limits). |
| `gross_matches`, `matched_leverage` (wave_rules) | PM6-6: \|G - G_parent\| <= .005; L' = L x G_parent / G at 4 decimals | arithmetic on a NAV statistic. C++ owner: `strategy_nav_replay --calibrate-gross` (speed finding 2: one process instead of calibration + matched run). |
| `criteria_rows`, `judge` (wave_rules) | printed criteria (tau per gross, 4x net SR, cost bps) and the acceptance rule (dSR > 0 and mechanics) | a rule over statistics nav_summ computes. Belongs in research_cycle's verdict (`cycle_verdict.py`) as named acceptance rules: Python core, not C++. |
| `mechanics`, `book` (wave_readers) | mostly `nav_summ.construction_stats` (called, not re-implemented); new: gross by calendar year, max gross and max \|net\|, gross of cost (net + annualised costs), tau per gross, the 4x row of `capacity_curve.csv` | statistics. C++ owner: `strategy_nav_replay` summary.json (it accumulates gross.mean and per-year books already): emit annual gross mean, max gross / \|net\| and gross by year; the readers then copy keys. |
| `admission_used`, `admission_new`, `budget_check`, `rescreens_of` (wave_stage_preflight) | counts ledger lines (`backtest_integrity.trial_counts`); trial ids by `cycle_admission.trial_id` (now called, `d4b28b5d`) | bookkeeping over the Python ledger: wrapper-legit (could become a `backtest_integrity` admission-budget count; not C++). |
| `tokens`, `scan` (wave_seal) | date tokens in logs against `research_window` (`period_begin`, `is_sealed`, now called) | an audit of run artifacts, not research: wrapper-legit. |
| marginal mode / reuse (`spec_stage`), `marginal_rows` (wave_result) | which rows are carried; the mode comparison | plumbing; the numbers are `strategy_marginal_ic.cpp`'s. Wrapper-legit; the C++ `--candidates` subset (speed finding 1) would replace the reuse. |
| `board`, `lineage`, `checks`, `timing` (wave_scoreboard) | joins results and the ledger; equality checks; sums of wall seconds | reporting: wrapper-legit. |
| `build`, `log_section`, `budget_line` (wave_result); stage_chain; wave_queue | records, formatting, orchestration, queue bookkeeping | wrapper. |
