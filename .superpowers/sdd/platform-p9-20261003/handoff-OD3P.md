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

## For the next agent

- If T2 is dispatched: merge root (which must show B2's redo), do item 6 above, re-run the two lane files, the whole
  `scripts/tests` (seeds 0, 1), the stage chain; append to the report.
- The C++ `absent-members-v1` work is not this lane's (OD3P-1: D3W's IC runner files); the contract is the input.
- Before the OD-3 read, root resolves `mv-aim-v1` in `scripts/specs/p9/od3/flagged_members.json` and re-pins it
  (runbook P0).
