# Handoff OD3P (lane, pool-21): RESUME 7 round, 2026-10-05

- Branch `feat/p9-od3p-20261004`, lease run id `p9-od3p-20261004` (leases not touched). Base `5c57c1ef`.
- Round 1 (OWNER STOP, 2026-10-04): code `937865f0`, merge of root `9c7aa654` = `09e2350a`, WIP records `80484f0b`.
- RESUME 7 round (dispatch R7-1, section OD3P): merge of root `4dc831ac` = `e809271f` (no conflict); OD3P-1 + ALCLOCK-2
  = `9be598cf`; runbook = `05e2a373`; merge of root `ebc918c3` = `e9bedcc7` (no conflict; the suites ran on it);
  then the commit with the report, this handoff and the suite logs (see `task-OD3P-report.md`, "Root merges").
- No build, no exe, no real data, no statistic read. `holdout_gate.py` never run.
- Interpreter: `python` on PATH is the atx-db venv; use `C:/Program Files/Python312/python.exe`.

## State of the dispatch items

1. Merge root, re-run the two test files: DONE (53 passed on `e809271f`).
2. OD3P-1: DONE. `carry_weights.py` zeroes nothing (every weights byte = the carry without the list; `carry.json`
   records the list; the theme_standardise refusal is gone); `research_roles.py` `RolesCycle.absent_members` passes
   `--absent-members <list> --absent-members-sha256 <pin>` to the u and w IC passes of each history role, binds the
   list, and refuses (exit 2) an IC exe without `absent-members-v1` and a drop with no history role. Contract for the
   C++ lane: `od3p-absent-contract.md` (WRITTEN). Tests on fakes; 22 teeth mutations caught
   (`od3p-evidence/teeth-r7.log`). file:line: `atx-impl/tools/carry_weights.py:24-38` (doc), `:154-164`
   `check_drop`, `:195-205` (record, no zeroing); `scripts/research_roles.py:29-37` (doc), `:82-84` constants,
   `:342-344` no-history refusal, `:389-409` `absent_members`, `:441` its call; tests
   `scripts/tests/test_research_cycle_od3.py:221-450`, `atx-impl/tools/test_od3_tooling.py:162-245`.
3. ALCLOCK-2: DONE (`info-clock-rate-v1` in `rules`, 2019; `mv-aim-v1` alone pending).
4. Runbook `p9-od3-runbook.md`: DONE.
5. Suites: DONE on `e9bedcc7` (root `ebc918c3`): lane files 61 passed; `test_no_python_mirror.py` 7 passed; whole
   `scripts/tests` seed 0: 543 passed, 4 skipped; seed 1: 543 passed, 4 skipped; `test_stage_chain.py` 9 passed. The
   classes guard flags `atx.carry-weights/v1` and `atx.od3-flagged-members/v1` (round-1 literals of
   `carry_weights.py`): rows proposed in the report for root (SQL2-CLS). The RESUME 7 OWNER STOP arrived after the
   suites had ended; no later root merge.
6. T2: PENDING on B2 (not in root). To do after B2's redo is merged in root: in `nav_summ` (B2's file),
   `--pool-one-sided` calls the engine verb's paired path with `one_sided=True` on the pooled pair plus the verdict
   dict (as `bundle()` does, `nav_summ.py:205-210` `paired_of(..., one_sided=...)` at `4dc831ac`); tests against a
   fake verb, and against the real one if built: the branch's T2 test, the T5 test and the flag-absent identity test,
   all re-done on the verb. T3 is dropped. Then re-run the suites and update the report.
7. Report `task-OD3P-report.md` and this handoff: DONE.

## R8 round (dispatch R8-1, section OD3P; 2026-10-06)

- DIRTY-1: `52805177` commits this handoff, `task-OD3P-report.md` (it was uncommitted and git-ignored too) and the six
  suite logs `od3p-evidence/r7-e9bedcc7-*.log`, as they stood at the stop.
- Merge of root `1b64ce45` = `70439eef`: not a no-op (one root commit, sprint-dir docs and logs only), no conflict, no
  file outside the sprint dir changed. Lane files re-run on `70439eef`: 61 passed
  (`od3p-evidence/r8-70439eef-lane-files.log`). Whole `scripts/tests` / stage chain not re-run (no code moved).
- Slip fixed: the report's "Tests" section still read "TO BE FILLED"; filled from the committed logs, and the R8 merge
  added to "Root merges".
- `od3p-absent-contract.md` and `p9-od3-runbook.md` are committed (`git ls-files`). T2 (item 6) still PENDING on B2
  (B2's redo is not in root at `1b64ce45`).

## Fix round 1 (review `91fae138` of `d78d23ce`; dispatch "OD3P fix round 1"; 2026-10-06)

- R-1 (Required) DONE: runbook B6 gives each history book its own fresh `fit.output` `build-equity/od3-fit-<book>`
  (lines 2 and 4 distinct; `p9-od3-runbook.md:126-142`); P7 stops unless `fit` is `pending` with that `--output`
  (`:152-162`); stop list `:227-228`. The cycle also refuses, at plan time, an existing `fit.output` whose `carry.json`
  does not record exactly this carry (`scripts/research_roles.py:380-408`). Reproduced first on the fakes
  (`od3p-evidence/fix1-r1-probe-before.log`), fixed run (`fix1-r1-probe-after.log`).
- S-1 DONE (plan-time `carry_weights.drop_list` / `check_drop`, `research_roles.py:362-378`; variant: an existing
  `fit.output` is accepted only as this carry's own output, so resume still works). S-2 DONE (test
  `test_the_absent_flags_reach_only_the_history_roles_ic_passes`; probe P1 now caught). S-3 DONE
  (`research_roles.py:470-474`). S-4 DONE (`p9-od3-runbook.md:215-217`). S-5 DONE (report "Blind"). S-6 root's.
- Teeth: 8 of 8 caught (`od3p-evidence/teeth-fix1.log`). Commits: fix `365b8c8b`; merge of root `fd084e78` =
  `97a8157f` (plain, no conflict); then the records commit. Suites on `97a8157f`: see the report's "Tests (fix round 1)".
- T2 (item 6) still PENDING on B2 (not in root at `fd084e78`).

## For the next agent

- Next is the re-review (REV-BASE: BASE = `d78d23ce`, the SHA the first review names as reviewed).
- If T2 is dispatched: merge root (which must show B2's redo), do item 6 above, re-run the two lane files, the whole
  `scripts/tests` (seeds 0, 1), the stage chain; append to the report.
- The C++ `absent-members-v1` work is lane ABS's (ABS-1); `od3p-absent-contract.md` sections 2-5 are its input.
- Before the OD-3 read, root resolves `mv-aim-v1` in `scripts/specs/p9/od3/flagged_members.json` and re-pins it
  (runbook P0).
