# Lane B2 handoff: post-identity redo, BLOCKED at the merge (permission refusal), 2026-10-04

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
