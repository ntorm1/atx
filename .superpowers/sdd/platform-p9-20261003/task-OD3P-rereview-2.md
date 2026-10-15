# Lane OD3P re-review 2 (RESUME 10; rulings REV-FRESH, FU-REVIEWS; the follow-up round: item 6 / T2 and OD3P-S)

## Verdict

**APPROVE**: 0 Required, 4 Suggested (one of them, S-4, is root's text, not the lane's).

- Item 6 (T2) is done on the engine verb with one verdict rule and the OD3P-T2V label.
- S-a and S-d are done, each with a test that bites. I re-did two of the lane's teeth and two of my own: all four
  were caught, and every restore was byte-exact.
- S-b and S-c are done, and the runbook lines are in.
- Flag-absent identity against root's `nav_summ.py` blob `9fd6bd0a` holds on the fake verb (my run). The real-verb
  cases are the named OD3P-SKIP skips.
- The one red is the environmental OD3P-ENV red and nothing else.
- M3-Q1: **yes**.

## Reviewed SHA and range

- HEAD `d99a12bb` (pool-21, `feat/p9-od3p-20261004`). Code `cc6686f9` (+ WIP `a1348616`). BASE `dc11c977` is
  re-review 1's APPROVE SHA of record (MERGE-SHA, `progress.md:700`).
- Lane commits in range (first parent): `a1348616` (WIP code), `e7d7c23a` (STOP 9 handoff), `cc6686f9` (tests and
  the T2V label), `d99a12bb` (records).
- The four merges of root in range are all PLAIN. For each, the merge tree equals
  `git merge-tree --write-tree <p1> <p2>`, recomputed by me:

  | merge | root head | tree |
  |---|---|---|
  | `6f667bc1` | `73b4acfd` | `28a398f7` |
  | `6e7b236a` | `898bf120` | `45ca147a` |
  | `e75d9d9c` | `0a2ceb2b` | `7ac7c9e6` |
  | `68c2cc6b` | `858a4fca` | `f4d770a9` |

  - All four root heads are ancestors of `feat/platform-p9-20261003`.
  - Only `6f667bc1` changed a lane-scope file: it brought B2's `nav_summ.py`, root's blob `9fd6bd0a`. The other three
    changed none of `nav_summ.py`, `research_roles.py`, `carry_weights.py`, the two lane test files,
    `scripts/specs/p9/od3/`, the contract or the runbook.
- The lane's own hunks are `git diff 858a4fca d99a12bb` (root's head at the last merge), 19 files:
  - `atx-impl/tools/nav_summ.py` (+40 / -3 against blob `9fd6bd0a`)
  - `atx-impl/tools/carry_weights.py` (+2 / -1)
  - `scripts/research_roles.py` (+12 / -3)
  - `atx-impl/tools/test_od3_tooling.py` (+389 / -4)
  - `scripts/tests/test_research_cycle_od3.py` (+72 / -1)
  - records: report, handoff, contract, runbook, `od3p-evidence/` (including fix round 1's five logs, committed as
    they stood, a disclosed slip)
  - No C++, CMake, golden, pinned value or spec file. No `classes.json` edit by the lane.
- Merge preview (read-only, `git merge-tree --write-tree 5c811b56 d99a12bb`; root's head now, with COV-MV merged):
  clean. The five lane code files keep the lane's blobs. Root's `nav_summ.py` is still `9fd6bd0a` at `5c811b56`. The
  other M5 follow-ups (pool-13 `489d9a7e`, pool-14 `101f695a`) touch none of the five files.

## Spec verdict (r10 "OD3P-FU" 1-3, handoff "Exact next step" 1-5, ledger OD3P-S / OD3P-T2V / OD3P-SKIP)

| item | verdict | evidence (file:line at HEAD) |
|---|---|---|
| 6 / T2: `--pool-one-sided` calls the verb's paired path with `one_sided=True` on the pooled pair, plus the verdict dict | done | `nav_summ.py:753-762`. The listed dirs keep `analyse`'s plain `paired_of` (`:706`). Test `test_od3_tooling.py:546`: only `lw.p_one_sided` and `verdict` are added, and one line is printed. `p_one_sided` equals the frozen Python on the pooled pair exactly. On the fake, the call sequence is the plain run's, with exactly one `--one-sided` call: the pooled pair against the pooled reference. |
| ONE verdict rule `paired_verdict`, shared with `--bundle` | done | `nav_summ.py:624-630`. `bundle()` calls it at `:652-654`: a verbatim extraction of the old inline dict, the same expression, the same alpha and the same rule text (read in the diff). No other verdict code in the diff. |
| OD3P-T2V label "bundle rule, not G-B8" | done | Printed at `:825-827`. Also in the verdict's `rule` (`:758-761`) and the usage text (`:118-123`). Asserted at `test_od3_tooling.py:546`, `:600` and `:620`. |
| refusals without `--pool` or without a reference: exit 2 | done | `nav_summ.py:1164-1165` (`ap.error`). Test `:620`: both argv exit 2, and the fake verb saw no call. `--reference` alone is accepted. |
| flag absent = root's blob `9fd6bd0a` (stdout, stderr, JSON minus `nav_summ_run`, verb-call sequence) on the listed cases | done (fake). The real case is root's | Test `:689`. It loads the blob by `git cat-file` (sha256 `50fa3503...` asserted) and patches the same fake into it. Cases: pooled v8 with a pooled reference, two history eras v7, a listed dir with `--reference`, `--bundle` with `--bundle-json`, and a pooled `--ledger` read (ledger lines minus `run` / `recorded_by` / `prev_sha256`; ledger path substituted in stdout only). Passed in my run. The flag-absent code path is unchanged: `getattr(args, "pool_one_sided", False)` false keeps the old line, `print_pool` prints only when a `verdict` is present, and the new `ap.error` fires only with the flag. |
| T5 on the verb | done (fake) | `test_od3_tooling.py:640`: the history read adds 0 trials, V and N are unchanged, and the ledger bytes with `--pool-one-sided` equal those without it. |
| `verb` fixture fake / real (OD3P-SKIP) | done | `test_od3_tooling.py:395-497`. `real` takes `NS.default_engine_exe()`, the same expression root's `test_nav_summ_engine.py:34` uses (`$ATX_EQUITY_BIN/atx-research-eval.exe` when set, else `build-equity/bin`). Its skip is NAMED: the reason names the verb, its path and OD3P-SKIP. In pool-21 the three `real` params skip (no verb). The fake is test-only, so it is out of the G-P5 scan, and it answers from B2's frozen statistics in the verb's JSON shape. |
| S-a (required): `other_eras`, `other_admission`, `other_anchor` each exit 2 at plan, nothing runs | done, with teeth | `test_research_cycle_od3.py:612-646`. The carry runs into a fresh dir and a re-plan shows `done`. Then one component changes. `run` raises `CycleError` with `EXIT_USAGE` (= 2, `cycle/spec.py:25`) and a message naming that component ALONE (`records another <c>)` anchored). `plan` on the CLI exits 2 with the same text, and `TR.calls` is unchanged. Lane teeth A1, A1b (re-review 1's X1, now caught), A2 and A3. My re-do of A2 on the whole cycle file: caught. |
| S-d (required): a `<fit.output>.pending` dir or file refused at plan | done, with teeth | Code: `research_roles.py:389-395`. It sits in `check_carry_output`, reached only through `check_carry` when `fit.carry` is present (`:332-333`), so specs without `fit.carry` are untouched. It runs before the done check, so a stale `.pending` beside a done carry is refused too. Test `:649-678` covers `dir_fresh`, `dir_after_done` and `file`. Each exits 2 at run and at CLI plan with nothing run; once the `.pending` is removed, the spec plans `pending` or `done`. The refusal set matches the tool's own: `carry_weights.publish` raises on `pending.mkdir()` `FileExistsError` for a dir or a file (`carry_weights.py:217-222`). Lane teeth D1; my own probe R-1 (below). |
| S-b: the contract header names ABS; no spelling differs from ABS's report | done | Header `od3p-absent-contract.md:3-10`. Against pool-28 `task-ABS-report.md:5-6`, `:48-54` and `:172-174`, every spelling is identical: flags `--absent-members` / `--absent-members-sha256`, capability `absent-members-v1`, schema `atx.od3-flagged-members/v1`, and `summary.json` `absent_members {rule, list_sha256, members}`. The tallies over the whole contract show no variant spelling. The C++ as merged agrees: `strategy_ic_absent.cpp` `absent_members_record` writes `rule` / `list_sha256` / `members`, and the runner's usage names both flags. Sections 1-6 are unchanged. |
| S-c: report row corrected | done | `task-OD3P-report.md:209`: "no log (partial deleted); deferred to the gate (TEST-1) (re-review 1 S-c)". |
| runbook lines | done | P0 T2 bullet `p9-od3-runbook.md:80-83`. B6 `:144-146` and P7 `:160-162`: `.pending` absent, S-d. R4 `:204-213`: T2's line and the `verdict` dict are the bundle's rule, labelled "bundle rule, not G-B8"; G-B8's read is the prereg's own, made by the PM from the printed p values; no second rule is coded. The stop list `:237`. The `[UNKNOWN: ...]` markers name root's merge. |
| run (TEST-1): the two lane files, seed 0 | done | 92 passed / 1 failed (OD3P-ENV) / 3 skipped (OD3P-SKIP), on `cc6686f9`, `e75d9d9c` and `68c2cc6b` (lane logs). I reproduced it on `d99a12bb`. |
| merge root before the report; commit | done | Three plain merges in the follow-up round (above). The tree is clean. |

## M3-Q1 (the class `od3-drop-list` for `atx.od3-flagged-members/v1`, root `094bcd3b`): **yes**

The contract (section 2) describes the list in five points, and the row matches each one:

| contract (section 2) | `classes.json:19` row |
|---|---|
| The list is the drop list itself: `fit.carry.drop.path`, the human-written `scripts/specs/p9/od3/flagged_members.json`, `eol=lf`. | `writer` names that file, human. |
| No derived file is written. | `ingest` `spec_doc`, `spec_kind` `other`: an input document. |
| It lists members treated as absent on every date (OD3P-1). It is not a cycle spec. | Its own class, `spec_kind` `other`, not `cycle-spec`. |
| It is read by the IC exe (`--absent-members`) and by `carry_weights.py`. | `writer` names both readers. |
| It is pinned by its SHA-256. | `pinned_by` names git, `--absent-members-sha256` and `summary.json absent_members.list_sha256`. |

The row has the same shape as `diagnostic-spec`, the alternative re-review 1 named. The classes guard stays green:
`atx-engine/tools/test_research_store_classes.py` gave 5 passed and 97 subtests passed. I ran that one file because
M3-Q1 asks for it.

One wording slip, root's (S-4): `pinned_by` says "the carry record's fit.carry.drop.sha256". `carry.json` records
`drop.list_sha256` (`carry_weights.py:201`), and `fit.carry.drop.sha256` is the cycle spec's key. This is not a
reason for a rename.

## OD3P-ENV (the verb-less red)

My run shows exactly one failure: `scripts/tests/test_research_cycle_od3.py::test_carry_replaces_the_pooled_fit_and_feeds_every_era`.

- **What fails.** The traceback ends in `cycle/cli.py:356` `HARD-STOP [summ]: exit 1`. The only captured stderr is
  `nav_summ: no engine statistics executable C:\atx-wt\pool-21\build-equity\bin\atx-research-eval.exe: build
  atx-research-eval ... or name one with --engine-stats EXE` (the `EngineStats.__init__` refusal). The summ argv
  carries no `--pool-one-sided`.
- **From the diff.** The test's body is byte-identical between `dc11c977` and HEAD (34 lines compared). The refusal
  text is absent from `dc11c977`'s `nav_summ.py` and present in root's B2 blob at `73b4acfd`, which arrived with merge
  `6f667bc1`. The lane changed nothing on the summ path: its `research_roles.py` hunk is the plan-time S-d check and
  docstrings, and `nav_summ`'s flag-absent path is the blob's. pool-21 has no `build-equity/bin/atx-research-eval.exe`,
  and `ATX_EQUITY_BIN` was unset.
- **Conclusion.** The red is B2's G-P5 refusal and nothing else. It is environmental, not a finding. Root's run with
  the verb is the run of record.

## Quality verdict

Good. The change is small and reuses what exists.

- **No Python statistic.** T2 asks the verb for `--one-sided` on the call it already makes (the same resamples: the
  T2 test shows the flag run's calls equal the plain run's apart from the flag). `paired_verdict` compares the verb's
  numbers. It is the bundle's pre-existing rule, extracted, not a new rule and not a copy of a C++ rule (the verb
  computes no verdict; `eval_report.cpp:257` is a comment). No second verdict rule is coded.
- **Flag-absent identity.** No default changed. No output key is added without the flag. No iteration order changed.
  The new refusal fires only with the flag.
- **S-d is fail-closed and early.** It refuses at plan what the carry tool would refuse only after the history u
  passes. The fixtures are synthetic.
- **No look-ahead or seal exposure.** No data and no exe were used. The fixture eras are dated 2015, 2017 and 2020,
  all synthetic.
- **Determinism.** The fake records sorted options. Nothing iterates a set into output.
- **Imports.** `test_od3_tooling.py` puts `scripts/` first on `sys.path`. No module basename is shared between
  `scripts/` and `atx-impl/tools/` (checked), so nothing is shadowed.
- **Blind.** The new hunks of `nav_summ.py`, the report, handoff, contract and runbook carry no 2020-2023 statistic.
  A decimal and percent scan hit only pytest progress bars, wall times ("39.6 min") and rule numbers. The fixture
  ledger's Sharpe constants are the fixture's own, and the report does not print them.
- **Weaknesses.** See S-1 and S-2: a None path, and an identity case that exercises one verdict branch.

## Findings

| path:line | severity | problem | fix |
|---|---|---|---|
| `atx-impl/tools/nav_summ.py:757-758` | Suggested (S-1) | The one-sided branch indexes `p["verdict"]` without the None check that `paired_of`'s contract allows (`:250`, "None below 3 common sessions"; `:255-256`). The `a.size < 3` note in front makes None unreachable as far as I can see, since the verb and `align` see the same sessions. A null block would still raise a `TypeError` traceback instead of a clean `paired: null`. Flag present only. | `if p is not None:` around the verdict line. The flag-absent identity is untouched. |
| `atx-impl/tools/test_od3_tooling.py:701` | Suggested (S-2) | The identity test's `--bundle` case does not exercise the p comparison of the shared `paired_verdict`. My re-do of the lane's T3 (the comparison inverted, which also changes `bundle()`) was caught only by the forced-verdict test (5 failed), not by this case. The fixture's bundle verdict therefore does not depend on p. The extraction is verbatim (read), so the risk is low, but identity of the shared rule is shown on one branch only. | Add a forced bundle case on the fake (`force` already applies to every `--one-sided` call, which `bundle()` makes): a PASS and a FAIL-on-p bundle compared new vs blob. |
| `.superpowers/sdd/platform-p9-20261003/task-OD3P-report.md:305` | Suggested (S-3) | The red is cited as `test_research_cycle_od3.py:92`. At HEAD it is `:94`: the module docstring grew by 2 lines in `cc6686f9`, and `:92` was re-review 1's line. | At the next touch of the report. A reviewer does not edit lane records. |
| `atx-engine/schemas/research_store/classes.json:19` (root's `094bcd3b`, not the lane's) | Suggested (S-4), root's, P10 | `pinned_by` says "the carry record's fit.carry.drop.sha256". The carry record holds `drop.list_sha256`, and `fit.carry.drop.sha256` is the cycle spec's pin. The class itself is right (M3-Q1: yes). | Reword at P10 to "the cycle spec's fit.carry.drop.sha256; carry.json drop.list_sha256". No rename. |

## Evidence

**My own runs.** All in pool-21 at `d99a12bb`, with `C:/Program Files/Python312/python.exe`,
`PYTHONDONTWRITEBYTECODE=1`, `PYTHONHASHSEED=0`, `-p no:cacheprovider` and my own `--basetemp` (parent created first).
`ATX_EQUITY_BIN` was unset. No ENV-1 exit and no permission refusal. `git status --porcelain` was empty after every
run.

```
$ pytest -rs scripts/tests/test_research_cycle_od3.py atx-impl/tools/test_od3_tooling.py
1 failed, 92 passed, 3 skipped in 44.26s   (exit 1)
  FAILED test_carry_replaces_the_pooled_fit_and_feeds_every_era: HARD-STOP [summ]: exit 1;
         stderr: nav_summ: no engine statistics executable ...\pool-21\build-equity\bin\atx-research-eval.exe  (OD3P-ENV)
  SKIPPED x3 test_od3_tooling.py:546 / :640 / :689: "the engine verb atx-research-eval is not built at ... Ruling
         OD3P-SKIP, root's run with the verb built executes this case"
$ pytest atx-engine/tools/test_research_store_classes.py        (M3-Q1 only)
5 passed, 97 subtests passed in 0.94s      (exit 0)
```

**Teeth (TEETH-RESTORE)**, with my own tool (session scratch, not committed). It keeps the pre-mutation bytes in
memory and mutates one CODE site. It runs the selection, then writes the bytes back in a `finally`. It asserts three
things: the raw SHA-256 equals the pre-mutation bytes; `git hash-object` equals the `HEAD:<file>` blob (both files are
CRLF in the working tree and LF in the blob); and porcelain is empty. Every selection passed unmutated first. The cycle
selections deselect the OD3P-ENV test (`-k "not carry_replaces_the_pooled"`).

```
HEAD d99a12bb; porcelain empty before the first mutation
B0 cycle file -k "not carry_replaces_the_pooled": 49 passed, 1 deselected | tool file: 43 passed, 3 skipped
B0 -k stale_pending: 3 passed | tool -k t2_: 11 passed, 1 skipped
L-A2 (lane's A2) S-a: guard ignores the admission [whole cycle file]   CAUGHT (1 failed, 48 passed)
     FAILED ...[other_admission-records another admission]; restored sha256 f2a554bf2467, hash-object 270c2f8d = HEAD
L-T3 (lane's T3) T2: verdict comparison inverted [whole tool file]       CAUGHT (5 failed, 38 passed, 3 skipped)
     FAILED t2_verdict...[pass, fail_p, fail_p_at_alpha, bundle_alpha_fail, bundle_alpha_pass];
     restored sha256 7b5c5e242e78, hash-object 3af286f6 = HEAD
R-1 (own) S-d: .pending check sees dirs only (file arm removed)          CAUGHT (1 failed [file], 2 passed)
R-2 (own) T2: verdict ignores dSR <= 0 (pass = p < alpha)                CAUGHT (3 failed [dsr_zero, dsr_negative, dsr_nan])
porcelain after the last restore: empty                                  (tool exit 0)
```

**From the lane's logs only** (`od3p-evidence/`):

- Teeth 9 / 9 (`teeth-fu.log`, tool `mutate-fu.py`). I read the tool: it keeps and restores bytes and asserts SHA,
  blob and porcelain, as TEETH-RESTORE requires.
- The 92 / 1 / 3 runs on `cc6686f9`, `e75d9d9c` and `68c2cc6b`.
- Fix round 1's five logs, newly committed.
- Not run by anyone in the lane or by me: every `real`-verb case, the mirror guard, the whole `scripts/tests` and the
  stage chain. These are root's / the gate's (TEST-1).

## What root must check at merge (M5; MERGE-SHA = this review's commit above `d99a12bb`)

1. **The two lane files, green with the verb built**, including the environmental test:
   - Command: `scripts/tests/test_research_cycle_od3.py` and `atx-impl/tools/test_od3_tooling.py`, seed 0, the verb
     at `NS.default_engine_exe()`.
   - Expected: **96 passed, 0 failed, 0 skipped**.
   - A skip means the verb was not found. It is not a pass: root's nav_summ suites never skip, while these `real`
     cases skip by OD3P-SKIP. A skip of `test_flag_absent_nav_summ_equals_roots_blob` for "git history unavailable"
     is not a pass either.
   - `test_carry_replaces_the_pooled_fit_and_feeds_every_era` must pass (OD3P-ENV, ROOT-1a).
2. **The flag-absent identity on root's blob.** Root's `atx-impl/tools/nav_summ.py` must still be blob `9fd6bd0a`
   when the follow-up merges (true at `5c811b56`).
   - If it changed, the lane re-derives the pin. Root never edits it.
   - The `[real]` param of the identity test is root's half of the proof.
3. **`--pool-one-sided` in P9 argv of record.** It must be absent from every P9 argv of record (specs, templates,
   kinds, the cycle's summ step) except where the runbook names it: R4 G-B8, `p9-od3-runbook.md:204-213`.
   - At HEAD no tracked spec, template or code passes it.
   - The R4 verdict line is the bundle's rule, not G-B8's (OD3P-T2V). G-B8 is read by the PM from the printed p
     values.
4. **SQL2-CLS: no new literal expected.** The lane's code hunks add no `atx.<name>/v<n>` literal. The guard is 5
   passed at HEAD with root's `094bcd3b` rows.
5. **Merge preview against root `5c811b56`: clean.** The lane files keep the lane's blobs. The other M5 follow-ups
   (B2 fix 2, SQL3 item 5) do not touch the five lane code files.
6. **Unchanged from re-review 1, root's:**
   - the `mv-aim-v1` resolution and the re-pin of `flagged_members.json` before the read (runbook P0; COV-MV is now in
     root);
   - PRE-39 F.1 wording (2b-ii);
   - seeds 0 / 1 and the stage chain at the gate (GATE-1).

## Checked

- [x] Previous reviews read first (REV-FRESH). BASE = `dc11c977`. Only `dc11c977..d99a12bb` reviewed. The merges are
  plain, and their lane-file effects are listed above.
- [x] Python only. No build, no exe, no data, no 2020-2023 output, nothing dated 2024-01-01 or later.
  `holdout_gate.py` not run. No lease touched. Root history read as one-line subjects only. The other pools (2, 13,
  14, 28) were read-only.
- [x] No tracked file changed by me except this review. No `git checkout` / `restore` / `stash`. Porcelain was empty
  before the commit.
