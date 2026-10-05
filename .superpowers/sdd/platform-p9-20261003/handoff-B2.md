# Lane B2 handoff: post-identity redo, BLOCKED at the merge (permission refusal), 2026-10-04

## Follow-up at the owner stop (2026-10-04; supersedes "Next round" below where they differ)

**Head:** the docs commit that adds this section, on the merge commit `c37ec671`. **Root SHA merged:** `5c57c1ef`
(`git merge feat/platform-p9-20261003`, plain; it holds E2's split, D2R and COVR). The merge is COMPLETE and committed;
no rebase, no history rewritten. No permission refusal this round. Tree clean after this commit.

**Conflict regions (ruling B2-14: each resolved by an Edit of that region; no checkout / restore / show-redirect):**

| file | region | side kept / how combined | why |
|---|---|---|---|
| `scripts/research_cycle.py` | 1: module docstring (ours: the 186-line usage text; root: E2's 3-line shim text) | root's (E2's) text; ours removed in chunks by Edit | E2 moved the usage text to `scripts/cycle/cli.py`; ours = merge-base text + B2's `exes.eval` entry, which is re-placed in `cycle/cli.py` after `exes_sha256` |
| `scripts/research_cycle.py` | 2: the module body (ours: the old 2,100-line code; root: E2's re-export imports) | root's re-exports, plus one line `from cycle.spec import EVAL_EXE  # noqa ...` | ours = merge-base code (E2 moved it to `scripts/cycle/**`) + B2-R3's hunks, each re-placed: `EVAL_EXE` after `EXE_NAMES` in `cycle/spec.py:81`; the `effective_exes` docstring and `names` line in `cycle/spec.py:417-425`; the three `summ_step` lines (`--engine-stats <exes.eval>`) in `cycle/phases.py:736-738` (+ `EVAL_EXE` in its `cycle.spec` import, `:21`). None dropped: `git diff 5c57c1ef c37ec671 -- scripts/research_cycle.py` is that one line |
| `scripts/tests/test_no_python_mirror.py` | ALLOWLIST head | ours (the six B2 statistics rows stay deleted, one comment line) + root's D2-3 comment; root's D3 / P10 rows untouched | G-P5: the deletion commit `598d2199` retired them |
| `scripts/wave_stage_record.py` | `verify`'s `problems` line | combined: `[exe_problem(exe, pins, SK.rule_cell_kind(w)), eval_problem(w, done)]` | root's E2-15 (c) argument and B2-10's check both kept (B2-11 below reduces `eval_problem`) |
| sprint-dir `handoff-B2.md`, `task-B2-report.md` (2 regions) | root held an older copy of the lane's own docs | ours (the lane's newer text) | lane-owned records |

Auto-merged: `scripts/tests/test_research_cycle.py` (B2-R3's exes dicts), `atx-engine/tests/CMakeLists.txt`. The merge
also carries root's deletion of 129 tracked `.mypy_cache` files under the sprint dir's `tools/` (none added by me).

**Pending items:**
- G-P5 deletion on the merged tree: PARTLY. In the tree (`598d2199` merged; mirror rows retired by the resolution).
  Not yet re-run on the merged tree: `test_no_python_mirror.py`, `test_nav_summ_engine.py::test_a_bin_without_the_verb_fails_closed`.
- B2-11 (re-review 1 R1 + R2): NOT STARTED (design: report, "B2-11 / R1 / R2"). Where:
  `wave_stage_record.py:27` drop `V2_SCHEMA`; `:104-128` `eval_problem` keeps only the spec key + pin check, keyed on
  `wave_kinds.is_v2(w.manifest)` (import `wave_kinds as WK`); `:177` `bundle_once(w, base, cell, verb=None)`: for v2
  refuse a verb on disk (`w.sha(path)`) != pin before the run, and after the run / on reuse
  `CV.check_verb(out, doc, (pin, ...))` -> `StageError(..., EXIT_PIN)`; `:214` `judge` passes the cell's
  (exes.eval, pin) to `bundle_once` in both branches and checks the cell's summ.json / pbo.json with
  `CV.check_summ_verb` after `scored_verdict`; `:248` `judge_plan` passes the verb to `bundle_argv`;
  `wave_steps.py:126` `bundle_argv(..., verb=None)` adds `--engine-stats <verb>` right after the script (v1 bytes
  unchanged). Tests: rewrite `scripts/tests/test_wave_eval_verb.py` on a real v2 manifest via `test_wave_kinds.py`'s
  `load_fixture` / `rerooted` / `world` / `wave` helpers (`:155-186`) in stage order; teeth by mutation.
- R1 / R2: = B2-11, NOT STARTED.
- PRE-15 survives the merge: YES by grep (`tot_minus_m` in `eval_report.cpp` x5, `nav_summ.py` x1, `dsr_total.py` x3;
  "prefix rule" in `eval_report.cpp` x3, `nav_summ.py` x3); its tests not re-run on the merged tree.
- D2R-2 (b): NOT STARTED (scan specs / templates / fixtures / drivers for abbreviated `nav_summ` flags first; then
  `allow_abbrev=False` and the e27b `--protocol` cases on D2R's `test_research_spec.py`).

**Tests actually run on the merged tree (before the merge commit; `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`, no
ATX_EQUITY_BIN):** `test_research_cycle.py` + `test_wave_eval_verb.py` + `test_cycle_scoring.py`: 99 passed, 3 skipped,
2 failed. The two failures are `test_cycle_scoring.py::test_verdict_dsr_is_the_ledger_variance_value_not_the_single_cell_lo_value`
and `::test_the_summ_runs_the_named_verb_and_the_verdict_refuses_another`: `FileNotFoundError` for
`build-equity/bin/atx-research-eval.exe` (no build tree in pool-14; they need the built verb). Suites are NOT known
green: no full `scripts/tests` run, no stage chain, no tools suites, no build on the merged tree.

**Next step for a fresh agent:** from this head (no re-merge unless root moved; if it did, `git merge
feat/platform-p9-20261003` again): implement B2-11 as above; G-P5 checks; D2R-2 (b) last; then build Debug + Release
(tags from `p9-b2-i`; the merge brought root's C++ changes, EXE-CUR), gtests eval / ledger / `TrialLedger*`, pytest
`scripts/tests` seeds 0 and 1 with `ATX_EQUITY_BIN` unset or a full bin, stage chain, tools suites, mirror guard; then
finish the report section "Post-identity redo" with the conflict table above.

**Where:** pool-14, branch `feat/p9-b2-20261003`. Head: the docs commit that adds this file (code commit of the round
`fe152fca`, on `4bed5c43`). History kept: `598d2199` (deletion) and `8fb17ad6` (B2-10) are on the branch; no rebase.
Report: `task-B2-report.md`, section "Post-identity redo" (end of file).

Earlier rounds are closed: the first report, fix round 1 (identity point `1b25b9a4`, merged by root at `c2416d62`),
B2-10, re-review 1 (`task-B2-rereview-1.md`, CHANGES_REQUIRED: R1, R2, S1-S7).

## Why the round stopped

`git merge e8e424b9` (plain; root's head at dispatch) conflicted in 3 files. Resolving `scripts/research_cycle.py`
takes E2's re-export shim (E2 moved the code to `scripts/cycle/`). The command `git checkout --theirs
scripts/research_cycle.py` was refused by the permission system ("Irreversible Local Destruction"). It was not retried
and not routed around (resume6 rule): any other resolution of that file reaches the same state. The merge was aborted
(`git merge --abort`), so the tree is at the pre-merge branch. The conflict list is in
`b2-evidence/redo-merge-conflicts.txt`, and the intended resolution is in the report.

**The owner must decide before the next round:** allow that resolution (`git checkout --theirs` on a conflicted
file during the merge), or have root or the owner resolve the file.

## Done this round (pre-merge branch; merges without a conflict)

- **PRE-15:** the verb row `dsr.tot.tot_minus_m` (DSR_hand at N = N_tot - M, same V), nav_summ's printed line and JSON
  key, the prefix rule labelled "prefix rule". gtest `EvalVerb.HandRowAtNTotMinusMLeavesOutTheCampaigns`, pytest in
  `test_dsr_total.py`. Teeth: mutating the N rule fails the gtest and 3 pytests.
- **Re-review 1 Suggested:** S2, S3, S5, S6, S7 done. S4 was already in root's `classes.json`.
- **Builds:**
  - `p9-b2-f` Debug, clean, `fe152fca`: verb `4bcfce3e...8ee2`.
  - `p9-b2-g` Debug: the teeth mutation, reverted.
  - `p9-b2-h` Release: see the report.
- **Tests:** see the report's "Evidence" section.

## Next round, in order (needs the merge)

1. `git merge <root's head>` (root `c8ddf736` already holds D2R and COVR). Resolve as the report's conflict table
   says:
   - `research_cycle.py`: E2's shim, plus `EVAL_EXE` in the `cycle.spec` re-export list;
   - B2-R3's hunks onto `scripts/cycle/{cli,spec,phases}.py`;
   - `test_no_python_mirror.py`: drop the B2 rows, keep root's D3 / P10 rows;
   - `wave_stage_record.py`: verify's problems line with both `SK.rule_cell_kind(w)` and `eval_problem`.

   Re-run `test_research_cycle.py::test_build_key_resolves_exe_dir` and `test_cycle_scoring.py` (the two verb tests
   need a built verb).
2. B2-11 (re-review 1 R1 + R2), as designed in the report:
   - verify keeps only the `exes.eval` key and pin check, keyed on `wave_kinds.is_v2`;
   - the judge's `bundle_argv` passes `--engine-stats <exes.eval>` for v2;
   - `bundle_once` refuses a verb on disk, or a `bundle.json` (fresh or reused), whose SHA-256 is not the
     `exes_sha256.eval` pin;
   - tests on a real `atx.research-wave/v2` manifest in the real stage order, with teeth by mutation.
3. G-P5 on the merged tree: `test_no_python_mirror.py` (the B2 rows retired), `nav_summ` failing closed without the
   verb.
4. D2R-2 (b), last, on D2R's version of `scripts/tests/test_research_spec.py`:
   - scan specs, templates, fixtures, drivers and argvs reaching `nav_summ` for abbreviated flags (stop if a pinned
     spec relies on one);
   - `nav_summ`'s `ArgumentParser(..., allow_abbrev=False)`;
   - update the e27b `--protocol` spelling cases (exact two-token and `=` forms read; an abbreviation refused, asserting
     the full `unrecognized arguments: <token>` message).
5. Rebuild after the merge (Debug and Release, next tags `p9-b2-i`, ...; EXE-CUR: root's `stats_ext.hpp` changed since
   `1b25b9a4`):
   - gtests: eval, ledger, and `TrialLedger*` (`atx-impl-tests`, not built this round: no atx-impl C++ changed);
   - pytest: `scripts/tests` under seeds 0 and 1, the stage chain, the tools suites.

## Notes

- Known reds on this pre-merge tree (both seeds):
  - `test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry`: E2's former red, cleared by E2's merge,
    which this branch lacks.
  - `test_cycle_e2e.py` canary: fails only when ATX_EQUITY_BIN names a bin without the IC / NAV exes (my first run); a
    host skip without it.
- `atx-impl/tools/test_holdout_gate.py` was not run (resume6: never run holdout_gate.py).
- Scratch: deleted by name at the end of the round (my `scratchpad/b2redo/`). The build trees `build-equity/` (Debug
  objects) and `build-equity-rel/` were deleted; the `mega-p9-b2-*` receipts and logs are kept in `build-equity/`.
