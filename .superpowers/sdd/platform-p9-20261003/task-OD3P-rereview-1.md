# Lane OD3P re-review 1 (RESUME 9, dispatch R9-1 "OD3P-REREVIEW")

## Verdict

**APPROVE**: 0 Required, 4 Suggested. R-1 is closed as the first review asked: each history book gets its own fresh
`fit.output`, P7 stops unless the fit phase is `pending` into that dir, and a plan-time guard in
`scripts/research_roles.py` refuses a reused dir. S-1 to S-5 are done. S-2 and S-4 were the two the PM required. S-6
is root's, as ruled. The runbook keeps every required line. The contract changed only by one cycle-side bullet in
section 6, as the report says, and stays consistent with OD3P-1. Every test I ran passed. Both of my re-done teeth
mutations were caught.

## Reviewed SHA

- HEAD `97a8157f` (pool-21, `feat/p9-od3p-20261004`), which is the code reviewed. BASE `d78d23ce` is the SHA the
  first review `91fae138` names as reviewed (REV-BASE).
- DIRTY-1a (my first act, before I read the records as evidence): `60e0b3f9` "docs(od3p): records as they stood at
  STOP 8 (DIRTY-1)". It commits `handoff-OD3P.md` (+16 / -1), `od3p-absent-contract.md` (+4) and
  `task-OD3P-report.md` (+81) exactly as they stood. Their SHA-256 prefixes before `git add`: `94a14cc3`, `739301fd`,
  `2e644297`. I edited none of them.
- Range `d78d23ce..97a8157f`:
  - `91fae138`: the first review (docs).
  - `365b8c8b`: the lane's fix.
  - `97a8157f`: merge of root `fd084e78`. It is PLAIN: its tree `32ab8e74` equals
    `git merge-tree --write-tree 365b8c8b fd084e78`. It changes no lane file. Root's range `1b64ce45..fd084e78` (SQL3
    merge + `classes.json` row) touches none of the lane's files. Root has moved no code since `fd084e78`; root's
    head `685e238b` is docs only.
- Lane hunks (`91fae138..365b8c8b`):
  - `scripts/research_roles.py` (+71 / -3)
  - `scripts/tests/test_research_cycle_od3.py` (+181 / -4)
  - `p9-od3-runbook.md` (+31 / -11)
  - five `od3p-evidence/` files
- Untouched in the range: `carry_weights.py`, `test_od3_tooling.py`, `flagged_members.json` (sha256 still
  `9aa6fb59...f5ff`, `eol=lf`), `scripts/cycle/cli.py`, `classes.json` (by the lane), B2's files, and C++.

## Findings of the first review: closed / not closed

| finding | state | evidence (file:line at HEAD) |
|---|---|---|
| **R-1** (Required) | **CLOSED** | See "R-1 in detail" below the table. |
| S-1 | CLOSED (variant, accepted) | `research_roles.py:193-199` `carry_tool` lazily imports `carry_weights.py`: no copied logic, the same pattern as `wave_readers.py:43`. The call is at `:362`; `check_drop_list` at `:365-378` runs `drop_list` / `check_drop` at plan time (exit 2). Test: `test_research_cycle_od3.py:526-557`. The variant replaces "refuse an existing fit.output" with "refuse one that is not this carry's own". That variant is **required**: runbook R3 (`p9-od3-runbook.md:196-197`) runs the same spec again for summ after R1 has run the carry, so a blanket refusal would refuse R3. This answers the lane's open question 1 (report `:244-245`): the variant is the right reading. |
| S-2 (PM: required) | CLOSED | `test_research_cycle_od3.py:555-589` uses two shapes (E1 history / E2 TRAIN non-anchor / E3 anchor, and two history eras / two TRAIN eras). It checks that the flagged set equals {u, w} x the history roles, and that every other step's argv equals the plain spec's. My teeth F5b re-did the first review's probe P1 against the whole lane cycle file: **CAUGHT**, where the first review saw it pass. |
| S-3 | CLOSED | `research_roles.py:470-474` raises `CycleError` (exit 2) for a history u / w pass that has no runner `--`. Test `:592-607`. |
| S-4 (PM: required) | CLOSED | `p9-od3-runbook.md:215-217` reads "P9-F0h's history S2 net Sharpe = its TRAIN S2 net Sharpe + P9-W's dSR (PRE-39 ...)", which is the ledger's PRE-39 wording (`progress.md:604`). Root's prereg F.1 (`p9-prereg.md:947`) still names P9-F0. Round 2b-i itself lists PRE-39 in F.1 as "not yet applied" (`p9-prereg.md:3`), so that is a 2b-ii item for root / PRE, not this lane's. |
| S-5 | CLOSED | The report's "Blind" block (`task-OD3P-report.md:212-222`) restates the round-1 disclosure. |
| S-6 | root's (not done, correct) | `classes.json` is untouched by the lane (SQL2-CLS ruling, `progress.md:662`). See "What root must check at merge". |

**R-1 in detail.**

- **Runbook B6** (`p9-od3-runbook.md:126-142`):
  - `fit.output` is REPLACED by a fresh `build-equity/od3-fit-<book>` for each of `p9f0h`, `yf0`, `r2` and `clean`.
  - It is never the TRAIN fit dir and never another line's dir. Lines 2 and 4 get distinct dirs even when both carry
    Y-F0's fit.
  - Before P7, the dir, its `-run1` and its `.pending` must be absent.
- **P7** (`:152-162`): STOP unless the fit phase is `pending`, and the carry argv after the runner's `--` holds
  `--output build-equity/od3-fit-<book>`. On line 4 that argv must end in `--drop-list ... --drop-list-sha256 <pin>`.
- **Stop list** (`:227-228`): carries the same condition.
- **Code** (fail-closed as well):
  - `research_roles.py:363` calls the guard, and `:380-408` is `check_carry_output`.
  - An existing `fit.output` is refused at plan / status / run (exit 2, before any step) unless its
    `atx.carry-weights/v1` `carry.json` records exactly this carry: W, A, the anchor, the eras, and the drop pin or no
    drop.
  - The record keys the guard reads match what the carry tool writes (`carry_weights.py:197-202`).
  - The guard runs only when `fit.carry` is present, so specs without it are untouched (flag-absent identity).
- **Tests:**
  - `test_research_cycle_od3.py:462-486`: the fixed B6 on the fakes. Both lines plan `pending` into their own dirs and
    run through the real carry. Line 4's record carries the list and line 2's does not. Both get the same weights
    bytes. Line 4's E1 w pass ends in the flags. A re-plan shows `done` (resume).
  - `:489-523`: five refusal cases, including the first review's `train_fit_dir` case and the shared-dir cases.
- **My teeth F1** (the guard call removed): **CAUGHT**, 5 failed. Lane probe logs: `fix1-r1-probe-before.log`
  reproduces the original hard-stop, and `fix1-r1-probe-after.log` shows the refusal and the fixed run.

## Spec verdict (r7-dispatch "OD3P" 1-7; first review; dispatch "OD3P fix round 1"; r9 "OD3P-REREVIEW")

| item | verdict | note |
|---|---|---|
| r9 0 DIRTY-1a | done | `60e0b3f9`, by me. |
| r9 1 R-1, S-1..S-5 | done | See the table above. S-6 is root's. |
| r7 2 OD3P-1 (carry zeroes nothing; flags on history u / w; capability refusal; contract) | done, unchanged | `carry_weights.py` is unchanged in the range. The cycle's flag placement is unchanged apart from S-3's fail-closed branch. |
| r7 3 ALCLOCK-2 | done, unchanged | `rules` = theme-cov-hedge-v1 2017, mom-volman-v1 2016, info-clock-rate-v1 2019. `rules_pending` = [mv-aim-v1]. |
| r7 4 runbook | done | All required lines are intact. Four history lines (section 1 table, `:44-49`). `holdout_gate.py` never runs (rule 0.2, `:19-20`). The identity-bridge flag is on every print (rule 0.3 `:21-27`, section 4 heading `:206`, stop list `:230`). The "not read: no producer" fallback is at `:57`, `:79`, `:161-162`, `:188`, `:209`, `:231-232`. Every value that is not yet known is marked `[UNKNOWN: ...]`. The fix round edited only B6, P7, prediction 3 and the stop list. |
| r7 5 suites | done for R7. Fix round: seed 1 not run | The lane ran these on `97a8157f`: lane files 74, `scripts/tests` seed 0 563 / 5 skipped, stage chain 10, mirror 7, classes guard 1 expected red. The seed-1 run was stopped at the OWNER STOP (report `:209`). I re-ran seed 0 before TEST-1 reached me. Seed 1 is deferred to the gate (TEST-1 / GATE-1). |
| r7 6 T2 | pending by rule | B2's redo is not in root: `git -C C:/atx-wt/pool-2 log --format=%s -30` shows no B2 merge. Not a finding. |
| r7 7 report, handoff | done | The fix-round sections are at `task-OD3P-report.md:167-246` and `handoff-OD3P.md:49-61`. |
| contract vs OD3P-1 | consistent | Range diff: section 6 gained one bullet (`od3p-absent-contract.md:122-125`) listing the three plan-time refusals, all cycle-side. Sections 2-5 (lane ABS's spec) are byte-unchanged. The schema, keys, flag spelling, capability, the defining equivalence (`:73-82`), and where the cycle puts the flags all still match `research_roles.py:453-477` and OD3P-1 (`progress.md:624`). |
| blind | clean | I grepped the runbook, report, contract and handoff for decimal / percent / statistic patterns. The only hits are rule numbers (0.3, 0.5), the prediction's formula text ("> 0", "= TRAIN + dSR", no values), and wall times (2374.5 s, 39.6 min). None is a 2020-2023 statistic. I read the prereg PRE-39 lines with digits masked. |

## Quality verdict

Good. The fix is small and reuses existing code: the guard reads the carry tool's own record schema and runs its own
list checks through one lazy import, without copying logic. A fresh run is never blocked, and resume still works.
Each new refusal happens at plan time, before any step, and names the runbook section it enforces.

- **Flag-absent identity.** Without `fit.carry` there is no new code path. These identity tests pass unchanged: blob
  `313b23b3` (root's `research_roles.py`, still root's blob at `fd084e78`), round-1 module `8081a6d0`, and branch
  tool `186655d9`.
- **Imports.** The `sys.path` insert of `atx-impl/tools` shadows nothing: no module basename is shared between
  `atx-impl/tools` and `scripts/` or `atx-engine/tools`.
- **Determinism.** The `why` text iterates a fixed dict order, and nothing iterates a set into output.
- **Data and C++.** No data, no seal exposure, no golden or pinned value edited, no C++, no new Python mirror of a C++
  rule.
- **Cost.** Two small JSON reads at plan time.
- **Weakness.** The guard's `eras` / `admission` / `anchor` components are untested (N-1).

## Findings (new)

| path:line | severity | problem | fix |
|---|---|---|---|
| `scripts/tests/test_research_cycle_od3.py:489-496` (guard `research_roles.py:393-405`) | Suggested | Test gap: no test exercises the guard's `eras` component (nor `admission` or `anchor`). My own probe X1 made the comparison ignore `eras`, and it was NOT CAUGHT (`-k "drop or absent or fit_output or carry"`: 32 passed). This is defensive only. A dir carried for other eras with the same W / A / drop would otherwise plan `done`. The IC runner's weights-to-`--train` binding would then refuse at `w:E1`, after the history u passes: R-1's failure mode, but only by hand misuse. | Add an `other_eras` case (the same W, A and list, one era role with another SHA) and an `other_admission` case to the refusal test. |
| `.superpowers/sdd/platform-p9-20261003/od3p-absent-contract.md:3-6` | Suggested | The header still says the C++ lane is "D3W's files; the PM assigns the work at D3W's hand-back". ABS-1 (`progress.md:643`) gives it to lane ABS, branched from D3W's APPROVE head. The source citations are "at `4dc831ac`" and may have moved on D3W's head. The handoff was updated (`handoff-OD3P.md:68`) but the contract was not. Sections 2-5 are unaffected. | At the next touch (or in ABS's brief), say "lane ABS (ABS-1)" and have ABS re-locate the cited lines on its base. A reviewer does not edit lane records (DIRTY-1a). |
| `.superpowers/sdd/platform-p9-20261003/task-OD3P-report.md:209` | Suggested | The seed-1 row names log `fix1-97a8157f-scripts-tests-seed1.log`, which does not exist (the partial was deleted, as the row says). Seed 1 never ran on the fix code. Hash-seed sensitivity is unlikely: the change sorts or compares lists and dicts and iterates no set into output. | Root runs seed 1 at the gate (TEST-1 / GATE-1). The row should read "no log". |
| `.superpowers/sdd/platform-p9-20261003/p9-od3-runbook.md:139-140`, `:155-157` | Suggested | B6 lists `build-equity/od3-fit-<book>.pending` as must-be-absent before P7. P7's stop rule and the plan-time guard do not test it. A stale `.pending` (possible only from a hand-run carry; a crashed cycle carry leaves `-run1`, which P7 sees as `failed`) plans `pending`. The carry tool would then refuse at the fit step after the u passes. | Have P7 state "and `<dir>.pending` absent", or extend `check_carry_output` to refuse an existing `<fit.output>.pending`. Low priority. |

## Evidence

**My own runs**, in POOL with `C:/Program Files/Python312/python.exe`, `PYTHONDONTWRITEBYTECODE=1`,
`PYTHONHASHSEED=0`, `-p no:cacheprovider` and my own `--basetemp` (its parent created first). The tree was
`60e0b3f9`: HEAD `97a8157f` plus the DIRTY-1a docs commit, so the code is identical. No ENV-1 exit. No permission
refusal. `git status --porcelain` was empty after every run.

```
$ python -m pytest scripts/tests/test_research_cycle_od3.py atx-impl/tools/test_od3_tooling.py ...
74 passed in 31.17s                                   (exit 0)
$ python -m pytest scripts/tests ...                    (whole, PYTHONHASHSEED=0)
563 passed, 5 skipped in 1166.81s (0:19:26)            (exit 0; = the lane's seed-0 count on 97a8157f)
$ python -m pytest atx-engine/tools/test_research_store_classes.py ...      (extra, read-only)
1 failed, 4 passed, 96 subtests passed  -- the expected SQL2-CLS red, exactly:
  {'atx.carry-weights/v1': ['atx-impl/tools/carry_weights.py'],
   'atx.od3-flagged-members/v1': ['atx-impl/tools/carry_weights.py']}
```

**TEST-1 (`progress.md:688`, the owner's ruling, sent to me by PM message while I worked).** The PM's TEST-1 message
reached me after both runs below had already finished:

- the whole `scripts/tests` seed-0 run, started under the r9 dispatch's "run yourself" line;
- the one-file classes-guard run.

Neither can be undone, and neither changes how this review counts its evidence. The verdict rests on the lane files,
the two teeth and reading the code. The whole-suite figure is extra evidence only: it is not part of the evidence this
review needs.

After TEST-1 arrived I ran nothing more. Under TEST-1 the whole `scripts/tests` (seeds 0 and 1) and the stage chain
are deferred to the gate (GATE-1; see "What root must check", item 2).

**Teeth** (TEETH-RESTORE). My tool keeps the original bytes in memory, mutates `scripts/research_roles.py`, runs the
named tests, and writes the bytes back. It then asserts that the file's SHA-256 equals HEAD's blob and that
`git status --porcelain` is empty, and it asserted both before the mutation too. No git discard command was used.

```
F1  (R-1 guard call removed; one of the lane's eight)             CAUGHT  (exit 1; 5 failed, 1 passed)
    restore OK sha256 1a70e29cadf5 == HEAD blob; porcelain 0 lines
F5b (S-2 probe P1 on the whole lane cycle file; one of the eight) CAUGHT  (exit 1; 2 failed, 15 passed)
    restore OK sha256 1a70e29cadf5 == HEAD blob; porcelain 0 lines
X1  (own probe, not counted: the guard ignores eras)              NOT CAUGHT (exit 0; 32 passed) -> N-1
    restore OK sha256 1a70e29cadf5 == HEAD blob; porcelain 0 lines
```

**From the lane's logs only** (`od3p-evidence/`):

- Teeth F1-F7 (8 of 8 caught, `teeth-fix1.log`).
- Stage chain 10 and mirror guard 7 on `97a8157f`.
- The R-1 before / after probes.

## What root must check at merge

1. **SQL2-CLS rows in `atx-engine/schemas/research_store/classes.json`.** At HEAD neither literal is in the file, and
   the guard is red on exactly these two (verified above).
   - `atx.carry-weights/v1` goes into class `fit-output` `schemas`. `carry.json` is published in `fit.output`, and the
     class's ingest is `artifact-only`. Extend its `writer` with `atx-impl/tools/carry_weights.py`.
   - `atx.od3-flagged-members/v1`: root picks the class whose ingest treats the list as a pinned input, using
     `cycle-spec` (ingest `spec_doc`, `spec_kind` `spec`) only if no better class exists, and states the choice
     (S-6, `progress.md:662`). The alternative is its own `spec_kind: other` row before `other`, like
     `diagnostic-spec` (`**/diagnostics/*.json`).
   - Afterwards the classes guard must be 5 passed.
2. **Identity procedure** (report `:117-134`, `:224-233`).
   - At the merge (TEST-1), re-run the two lane files (expected: 74 passed). There is no build and no exe.
   - Deferred to the gate (GATE-1, root, once, on the gate head): the whole `scripts/tests` under seeds 0 and 1, and
     `test_stage_chain.py` separately. **Seed 1 has not run on the fix code anywhere.**
   - The identity tests pin root's `research_roles.py` blob `313b23b3`. If another lane changes root's
     `research_roles.py` before this merge, the lane re-derives that pin; root never edits it.
3. **`mv-aim-v1` pin (P0, root's).** Resolve `mv-aim-v1` in `scripts/specs/p9/od3/flagged_members.json` from
   COV-MV's registration: into `rules` with its year if that year is 2015 or later, else removed. `rules_pending` must
   then be empty. Re-pin the file (current sha256 `9aa6fb596a4edd0b2a8028a337678161efdfa50535d61dbe9c18fdff9f56f5ff`).
   Until then, line 4's plan is refused at plan time (S-1), as the runbook's P7 expects.
4. **T2 (r7 item 6)** is pending on B2's redo. OD-3 waits for it (PRE-21). After B2 merges, the lane does it and the
   suites are re-run.
5. **R-1 at the read.** The four `scripts/specs/p9/od3/od3-<book>.json` specs each name their own
   `build-equity/od3-fit-<book>`, and P7 shows `fit` as `pending` into it before R1.
6. **After ABS merges.** `<frozen ic exe> --list-rules --json` lists `absent-members-v1`. Line 4's plan shows the
   E1 / E2 u / w argv ending in the two flags, and E3 without them. Without ABS, line 4 prints "not read: no
   producer". The contract header names D3W, not ABS (N-2); ABS reads sections 2-5.
7. **Prereg (2b-ii, root / PRE).** F.1's prediction (`p9-prereg.md:947`) still names P9-F0. PRE-39 names P9-F0h, as
   the runbook already says (lane open question 2).

## Checked

- [x] DIRTY-1a commit made first and the records not edited.
- [x] Diff stays inside the lane's scope. The merge is plain and touches no lane file.
- [x] The evidence in the report matches its claims: line citations spot-checked (`research_roles.py:193-199`,
  `:362-408`, `:470-474`; runbook `:126-142`, `:152-162`, `:215-217`, `:227-228`; test `:555-589`). The cited resume
  test `test_carry_replaces_the_pooled_fit_and_feeds_every_era` exists (`:92`).
- [x] Python only. No build, no data, no 2020-2023 output, nothing dated 2024 or later. `holdout_gate.py` not run.
  No lease touched.
