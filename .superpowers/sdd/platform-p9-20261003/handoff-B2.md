# Lane B2 handoff: post-identity redo (first written 2026-10-04; newest section first)

## R10: fix round 2 (B2-N37) done (2026-10-08; dispatch R10-1; supersedes every section below)

**Head:** the docs commit that adds the "Second merge" note below, on the merge commit `96ff5aa0` (root `858a4fca`
merged: M4's D3, OD3P, AL-CLOCK; no conflict; tree `d64e548a` = `git merge-tree` recompute). **Root SHA merged
last:** `858a4fca`. Before it: `eef8aabd` (records) on `e4683013` (root `094bcd3b`: M3's C3b, D3W, ABS and the
SQL2-CLS rows; no conflict; tree = recompute). Lane code commit of the round: `71abeb2f`
(`atx-impl/tools/test_nav_summ_v8.py` only). Branch `feat/p9-b2-20261003`, pool-14. No rebase, amend or reset; tree
clean after this commit; nothing pending in the lane.

**Second merge (after the records commit; root had moved again in code):** `96ff5aa0` merges root `858a4fca`; it
changes nothing on the n37 path (root's `atx-impl/tools` changes are `carry_weights.py` and `test_od3_tooling.py`;
`atx-engine/src/research/eval` unchanged since `094bcd3b`). Re-run on `96ff5aa0` with the same `p9-b2-r` verb: the
test file with root's inputs, blind, 10 passed (`r10-pyt-v8-file-root-merged2.log`); synthetic 9 passed / 1 skipped
(`r10-pyt-v8-file-synthetic-merged2.log`).

**Ruling B2-N37: cause (b), arithmetic.** The verb's JSON doubles round-trip exactly into nav_summ (nlohmann printer,
`json.loads`), so no formatting fix exists; the residue is the engine's summation order / fma: 541 summ leaves and 2
PBO leaves differ, 0 beyond 1e-12 relative, **max |relative difference| 4.967e-14** (summ; PBO 3.104e-16). One row is
the self pair (the legacy argv lists its reference among the dirs), degenerate under B2-R1 (c): SE / t null plus the
`degenerate` / `one_minus_rho` keys (B2-6). The test now compares numerically at 1e-12 relative with that disclosure
in its docstring and pins exactly one degenerate row; the golden file and every expected hash are unchanged.

**Build:** Debug `p9-b2-r` of `atx-research-eval` only (source `73b4acfd`, dirty 0, exit 0; verb SHA-256
`71d19288182a94a025b90b135f4c12477406d65e0e94ce82af5e6fbe49fd91c7`; receipt copy in `b2-evidence/`). No second build
(cause (b): no C++). Next free tag `p9-b2-s`.

**Tests (TEST-1, the test file only):** red reproduced on the unchanged test (1 failed); fixed: n37 1 passed; whole
file with root's inputs 10 passed (before and after the merge); synthetic 9 passed / 1 skipped (before and after).
Teeth in memory through the test's own helpers: 8 / 8 breaking perturbations caught, a 5e-13 relative
perturbation accepted (inside the tolerance).

**Next:** re-review 3 (REV-FRESH, BASE `46ffe3dc`; the lane's change is `71abeb2f`, the rest root's history); then
root merges the round (MERGE-SHA) and re-runs the test file with its own verb, blind: the known-red set becomes empty.
Detail: `task-B2-report.md`, last section "Fix round 2 (B2-N37)"; evidence `b2-evidence/r10-*`.

**Disk:** after this commit, the pool-14 Debug object tree is deleted by name (1,229 MiB; receipts and logs kept)
and the session scratch dir `b2-r10/`. **Permission refusals:** none. **Slip:** the merge commit `e4683013` carries
git's default message without the trailer (not amended).

## R9 round: the two pending suites done (2026-10-07; dispatch R9-1; supersedes every section below)

**Head:** the docs commit that adds this section, on the merge commit `52cdcc8c` (root `685e238b` merged: prereg
round 2b-i, docs only, no conflict). **Root SHA merged last:** `685e238b`. Branch `feat/p9-b2-20261003`, pool-14. No
rebase, amend or reset; no code changed; tree clean after this commit; nothing pending in the lane.

**TEST-1:** the PM's TEST-1 message arrived after `p9-b2-q`, the gtests and both suites below had finished
and been committed (`fda4c27b`); nothing was re-run; the whole `scripts/tests` and the stage chain are deferred
to the gate (TEST-1), whose runs are the runs of record (the lane's runs stay as its evidence).

**Done:** configure `equity-dev` (the object tree had been deleted at the R8 stop); Debug `p9-b2-q` at `52cdcc8c`
(receipt `build-equity/mega-p9-b2-q-receipt.json`, copy in `b2-evidence/`; dirty 0, exit 0, 466 TUs; verb
`atx-research-eval` SHA-256 `80a73375313272011d4fcb7a90caba1b46cab371da899ddfeed560222cc0142d`, re-checked unchanged
after the last suite); gtests eval 26 / 26, ledger 16 / 16, `atx-impl-tests` `*Ledger*:EvalVerb*` 29 / 29;
`scripts/tests` seed 1 527 passed / 5 skipped; `atx-engine/tools/test_stage_chain.py` seeds 0 and 1, 10 passed each.
0 failed; the five skips are the named environment skips of seed 0; ENV-1 not seen. Logs `b2-evidence/r9-*`; record
in `task-B2-report.md`, section "R8 round" (the two cells filled) and "R9 completion".

**Next:** re-review 2 (B2-SEQ: the suites are recorded, it may start; REV-BASE `26fe8d50`; `52cdcc8c` is one more
merge with no conflict). The pool-14 Debug object tree is deleted by name after this commit (`mega-p9-b2-*` receipts
and logs kept). Next free tag `p9-b2-r`.

## R8 round: OWNER STOP with two suite runs pending (2026-10-06; dispatch R8-1; supersedes every section below)

**Head:** the docs commit that adds this section, on the merge commit `1b15330d`. **Root SHA merged last:**
`fd084e78` (SQL3 merged in root during the round). Branch `feat/p9-b2-20261003`, pool-14. No rebase, amend or reset;
tree clean after this commit. No code changed this round; nothing is WIP.

**Commits of the round:** `426721b2` (DIRTY-1: the R7 record and 11 untracked R7 logs, as they stood); `7832719f`
(merge root `1b64ce45`, no conflict); `1b15330d` (merge root `fd084e78`, no conflict); the docs commit.

**Done:** every item of `r7-dispatch.md` "B2": the merges (B2-14; conflict tables of all five merges in the report),
B2-11 with re-review 1 R1 / R2 (`d8f80c7a`), the test rewrite on a real v2 manifest, G-P5 on the merged tree,
PRE-15 (`fe152fca`), D2R-2 (b) (`34838462`), builds, and every named suite on `7832719f`; on `1b15330d` every suite
except the two below. Not shown: B2-11's teeth by mutation (the R7 mutation Edit was refused; PERM-1: not retried;
the reviewer may try its own).

**Not done (pending; the only next step):** on the final tree `1b15330d` (or on root's then-current head, merged with
`git merge feat/platform-p9-20261003`):
1. `scripts/tests` under PYTHONHASHSEED 1 (my run was stopped at the owner stop at about 58 %, no failure printed;
   `b2-evidence/r8-pytest-scripts-o-s1.log`, marked STOPPED);
2. `atx-engine/tools/test_stage_chain.py` under seeds 0 and 1 (not started).
Both need the Debug verb `build-equity/bin/atx-research-eval.exe` (the object trees were deleted at this stop: build
Debug first with tag `p9-b2-q`, targets as the receipts list). Command form: `PYTHONDONTWRITEBYTECODE=1
PYTHONHASHSEED=1 "C:/Program Files/Python312/python.exe" -m pytest -q -rs -p no:cacheprovider --basetemp <scratch>
scripts/tests`, ATX_EQUITY_BIN unset; then fill the two "pending" cells of the report's R8 pytest table
(`task-B2-report.md`, section "R8 round", table row "the same / seed 1" and the stage-chain row).

**Builds used (pool-14):** `p9-b2-m` (Debug) / `p9-b2-n` (Release) at `7832719f`; `p9-b2-o` (Debug) / `p9-b2-p`
(Release) at `1b15330d`; all dirty 0, exit 0. Release builds were no-ops (exes byte-equal to `p9-b2-l`); the Debug
ones relinked only because the shared `C:\atx-cache\deps\spdlog-build` had been refreshed by another lane. Verb
SHA-256 at `1b15330d`: Debug `b2ee9b269eb847325848bdbe67907ecd987eeeda6ccdf4c26188b8d9c71a7529`, Release
`bef0c0cd5b1d47f266d5b2c2c5f28a60718158bcfd8a99486b47dbe2bcf883d1`. Receipts kept (`build-equity/mega-p9-b2-*`,
copies of i..p in `b2-evidence/`). Next free tag `p9-b2-q`.

**Tests actually run (0 failed in every finished run; logs `b2-evidence/r8-*`):**
- on `7832719f`: gtests eval 26, ledger 16, `atx-impl-tests` `*Ledger*:EvalVerb*` 29 (Debug and Release);
  `scripts/tests` seed 0 520 passed / 4 skipped, seed 1 520 / 4; stage chain 9 per seed; tools + fixtures 274 passed /
  1 skipped (Debug seed 0, Release seed 1); mirror guard 7; fail-closed 1. One void seed-0 run (the Debug verb was
  relinked under it), re-run whole.
- on `1b15330d`: the same gtests 26 / 16 / 29 (Debug and Release); `scripts/tests` seed 0 527 passed / 5 skipped (the
  new skip: SQL3's `test_research_store_hook.py:381`, `atx-research-store.exe` not a B2 target); tools + fixtures 274
  / 1 (Debug seed 0, Release seed 1); mirror guard 7; fail-closed 1. Seed 1 stopped, chain not run (above).

**For re-review 2 (REV-BASE):** BASE `26fe8d50`; check every conflict region of `c37ec671`, `b1df1829`, `c3201a60`
(and the clean `7832719f`, `1b15330d`). For root's merge: the report's section "What root re-proves at merge" (TRAIN
identity procedure; keep-both points with D3 `test_wave_kinds.py:171`, D3W `eval_report.cpp` PRE-15 hunks, C3b
`scripts/cycle/spec.py`).

**Open for the PM:** (1) D3W-EVAL assumed `eval_report.cpp` is not in B2's list; PRE-15's hunks are there (keep-both
at merge). (2) The shared Debug `spdlog-build` dir lets one pool's build relink another pool's exes (new SHA-256s with
no source change). (3) Permission refusal this round (PERM-1, not retried): a Write of a NEW scratch draft
`scratchpad/b2-r8/report-r8.md` was refused ("Subagents should return findings as text, not write report files"); the
lane records were then written by Edits of the existing `task-B2-report.md` and `handoff-B2.md`.

## Follow-up at the owner stop (2026-10-04; superseded by "R8 round" above)

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
