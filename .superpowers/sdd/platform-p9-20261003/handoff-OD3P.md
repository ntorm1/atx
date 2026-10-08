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

## Follow-up round (item 6, OD3P-S; RESUME 9, dispatch "OD3P follow-up"): OWNER STOP 9, 2026-10-07

Head at the stop: `a1348616` WIP code + this handoff commit (records). Root merged first: `feat/platform-p9-20261003`
= `73b4acfd` (contains `fcb0200e`: B2 redo + T2 in root), merge `6f667bc1`, plain, no conflict; root's range changed no
lane file except an append to the `scripts/cycle/cli.py` usage text (not the lane's hunk). Tree clean at the stop; no
teeth ran, nothing mutated. No test has run on this round's code. No permission refusal.

| item | state | where (file:line at `a1348616`) |
|---|---|---|
| 1 merge root | DONE | `6f667bc1` (root `73b4acfd`) |
| 6 T2 (`--pool-one-sided` on the verb) | CODE WIP, UNTESTED | `atx-impl/tools/nav_summ.py:118-121` usage; `:622-628` `paired_verdict` (one verdict rule, now also used by `bundle()`, `:647-649`); `:746-759` `paired_or_note` one-sided branch (`paired_of(..., one_sided=True)` + `verdict`); `:821-824` the one printed line; `:1138-1139` flag; `:1161-1162` refusal (needs `--pool` and `--pool-reference` / `--reference`) |
| S-a (guard eras / admission test with teeth) | NOT DONE (no code change needed; test + teeth only) | guard `scripts/research_roles.py:382-420` |
| S-d (stale `.pending` refused at P7, test with teeth) | CODE WIP, UNTESTED | `atx-impl/tools/carry_weights.py:77` `PENDING_SUFFIX` (`:218` publish uses it); `scripts/research_roles.py:388-395` refusal at plan time (dir or file), doc `:43-45` |
| S-b (contract header: ABS is the consumer) | NOT DONE (checked only) | ABS `task-ABS-report.md` (pool-28) lines 5-6, 48-54, 172-174: flags `--absent-members` / `--absent-members-sha256`, capability `absent-members-v1`, schema `atx.od3-flagged-members/v1`, `summary.json` `absent_members {rule, list_sha256, members}` = the contract's spellings: no spelling differs; edit the header sentence (`od3p-absent-contract.md:3-6`) only |
| S-c (report row) | NOT DONE | `task-OD3P-report.md` "Tests (fix round 1)" seed-1 row: log column -> "no log (partial deleted); deferred to the gate (TEST-1)" |
| runbook | NOT DONE | `p9-od3-runbook.md` P0 T2 bullet (`:80-81`) and R4 (`:200-202`) `[UNKNOWN: T2 ...]` markers; B6 `:139-142` and P7 `:155-156` add "and `<dir>.pending` absent (the cycle refuses it, S-d)"; note in R4 that T2's printed verdict is the bundle's rule (dSR > 0 and p < alpha) while G-B8's pass rule is dSR > 0 with both p printed (prereg F.1) |

### Exact next step (fresh agent, pool-21, from the head above)

1. Tests, `atx-impl/tools/test_od3_tooling.py` (replace the header's "T2 pending" sentence):
   - a `verb` fixture with params `fake` / `real`: `fake` = a dummy exe file passed as `--engine-stats` and
     `NS.EngineStats.run` monkeypatched to a fake built on B2's frozen statistics
     (`atx-engine/tests/fixtures/eval_tie/frozen_statistics.py`: `net_moments`, `paired_stats(..., one_sided=)`,
     `dsr_rows`; the `--paired` CSV path maps back to its series via `engine._csv`; refuse unsupported options; record
     each call's options); `real` = `--engine-stats NS.default_engine_exe()`, skipped with a reason when not built (no
     build in pool-21; root's gate has it). Pass `--engine-stats` explicitly to the old module too (its default exe
     path resolves from its tmp location).
   - T2: the xpre test `f68058e2:atx-impl/tools/test_od3_tooling.py:230-248` on the verb: with the flag only
     `lw.p_one_sided` and `verdict` are added and one line printed; `p_one_sided` equals
     `FS.paired_stats(pooled aligned a, b, 200, NS.DEFAULT_BLOCK, NS.V8_SEED, one_sided=True)` exactly, dSR close; the
     fake records exactly one `--one-sided` call (the pooled pair). Verdict cases on the fake with forced dSR / p
     (PASS, FAIL on p, FAIL on dSR <= 0, FAIL on p null, `--bundle-alpha` honoured). Refusals (no `--pool`; no
     reference): exit 2.
   - T5: xpre `:186-200` on the verb (helpers `test_nav_summ_pool.history_rows` / `write_dir`, `test_nav_summ.write_nav`,
     `SCEN`), plus: the ledger lines with and without `--pool-one-sided` are equal.
   - Flag-absent identity against root's `nav_summ.py` blob `9fd6bd0a6032b4512c93c50fcc25dbc123c8b518` (root `fcb0200e`
     = `73b4acfd`; sha256 `50fa3503...`), loaded by `git cat-file`, its `EngineStats.run` patched the same way: stdout,
     stderr, JSON minus `nav_summ_run`, and (fake) the verb-call sequence equal on: pooled + pool reference (v8), two
     history eras (v7), a listed dir with `--reference`, `--bundle` with `--bundle-json` (the shared `paired_verdict`),
     and `--ledger` lines (`comparable`, as xpre `:276-307`).
2. Tests, `scripts/tests/test_research_cycle_od3.py`: S-a `other_eras` (run the carry into a fresh `out/od3-fit-clean`
   with `stop_after="fit"`, then change `e1/manifest.json` `dates` -> "records another eras"), `other_admission`
   (rewrite `fit/admission.json` -> "records another admission"), `other_anchor` (hand-edit the record's
   `anchor_role_sha256` -> "records another anchor"); each exit 2 at plan, nothing runs. S-d: a `<fit.output>.pending`
   dir (fresh and after a done carry) and a `.pending` file -> refused at plan ("od3-fit-clean.pending exists", "S-d").
3. Run (TEST-1): the two lane files only, `PYTHONHASHSEED=0`, `C:/Program Files/Python312/python.exe`,
   `-p no:cacheprovider`, own `--basetemp` (parent created first), `PYTHONDONTWRITEBYTECODE=1`.
4. Commit the tests (code commit), then teeth with a new tool `od3p-evidence/mutate-fu.py` (TEETH-RESTORE: keep the
   bytes, mutate, run the named test, restore, assert raw SHA-256 = pre-mutation, `git hash-object` = `HEAD:<file>`
   blob (nav_summ is CRLF in the working tree, LF in the blob), `git status --porcelain` empty before and after):
   S-a: the guard ignoring `eras`, `admission`, `anchor` (each alone); S-d: the `.pending` check removed; T2: the
   refusal removed, `one_sided=True` dropped, the verdict comparison inverted.
5. Docs: S-b header, S-c row, the runbook lines above; "Follow-up round (item 6, OD3P-S)" in `task-OD3P-report.md`;
   a head line here; records commit (`git add -f`). Blind: no 2020-2023 statistic.
