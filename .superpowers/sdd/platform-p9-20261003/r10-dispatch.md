# P9 RESUME 10: per-agent dispatch (PM, 2026-10-08)

ROOT-SDD = `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`. Every agent: Opus; reads the contract
(`resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md` for lanes, `resume7-addendum.md`,
`resume8-addendum.md`, `resume9-addendum.md`, `resume10-addendum.md`), then its section here, then the files its
section names, then greps its ruling ids in `progress.md`. Reviewers: `r7-review-dispatch.md` (contract),
`r8-review-notes.md` (traces), `briefs/review-template.md`. Hand-back under 200 words per `resume10-addendum.md`.
Commit trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Work ONLY in your pool (`cd` there first:
the shell starts in `C:/atx`, where nothing is ever run). Never any git write in pools 8, 24 or 2 (root excepted).

## ROOT M3 (pool-2; one root agent; ROOT-M3-SCOPE)

Read `r7-root-merge.md`, `r9-dispatch.md` "ROOT", `handoff-root-m2.md`, `handoff-root-m3.md` ("Exact next step" is
your list), ledger `progress.md` :740 ("Dispatch root M3") and the RESUME 10 block; then the three reviews' "What root
must check at merge": pool-15 `task-C3b-review.md` (+ `task-C3-report.md` "## Part B (C3b)"), pool-23
`task-D3W-review.md` (+ `task-D3W-report.md`), pool-28 `task-ABS-review.md` (+ `task-ABS-report.md`).
0. Housekeeping docs commit, as the files stand (never edit a ledger line): `progress.md`, `next-goal-prompt.md`;
   `git add -f` `status-10.md`, `handoff-root-m1.md`, `root-wave3-merge-report.md`, `disk-audit.md`,
   `resume10-addendum.md`, `r10-dispatch.md`. NEVER the PNGs under `docs/plans/`, never a cache dir.
1. `git merge --no-ff 76b9f256` (C3b). Keep-both with B2 (`wave_fixture.py`, `test_wave_kinds.py`: dry merges were
   clean). Re-proof (ROOT-1a, TEST-1): `test_wave_kinds.py`, `test_wave_eval_verb.py`, the lane's eight wave pytest
   files + `test_wave_gross_match.py`; ONE Debug build `p9-2u` of `atx-impl-strategy-target-tests` (+ the NAV exe if
   the named filter needs it), named 34 incl. `NavSpec.GridFileRefusesADuplicateKey`; the three C3B-S evidence tools
   listed as broken by design, not rewritten; `sign_pm7_35` G-P5 row still names E2 (SIGN relabels or root at the
   freeze). SQL2-CLS: no new literal expected (check).
2. `git merge --no-ff f0726acb` (D3W). B2-EVAL-KEEP: `eval_report.cpp` keep-both region by region (B2 `:569-605`,
   `:722-737`, `:798-813`; D3W `:603-628`, `:634-660`, `:683`, `:710-716`), never a side checkout (B2-14). ONE Debug
   build (next tag) of `atx-research-eval`, `atx-engine-research-eval-tests`, `atx-engine-research-composition-tests`,
   `atx-impl-strategy-ic-tests`, `atx-research-composition`, `atx-research-admission`, `atx-equity-strategy-ic`; eval
   26 + `EvalVerb.HandRowAtNTotMinusMLeavesOutTheCampaigns`; `WalkForward.*`, `WeightsV3.*`, `MomVolman.*`,
   `ResearchComposition.*`; the fitter / composition pytest files the report names; `d3w-logs/tools/ident.py` (17 / 21
   + 4 fingerprint-only, as the review records). D3W-STALE: `mom-volman-v1.json:50` is NOT edited.
3. `git merge --no-ff 6a3f5043` (ABS). SQL2-CLS rows `atx.od3-flagged-members/v1`, `atx.carry-weights/v1` in
   `classes.json` (one commit; guard before / after; store suite); ONE Debug build (next tag) of
   `atx-equity-strategy-ic,atx-impl-strategy-ic-tests`; `AbsentMembers.*` 11; identity names 53; `--list-rules --json`
   shows capability `absent-members-v1`; the od3 plan on fakes refuses without / plans with the capability.
4. At every merge: tree = `git merge-tree` recompute where no conflict; SQL3-2 (no `build-equity/research-store/`
   in pool-2); trial ledger 133 `27e40f9f` start and end; ENV-1; EXE-CUR by receipt (SPDLOG-RELINK). Whole suites:
   deferred to the gate (TEST-1). A design conflict or an unattributable red stops THAT merge (merge not reverted;
   handoff; BLOCKED) and you go on with the next independent one.
5. Records: `root-wave3-merge-report.md` (exists, 11.9 KB: read, then APPEND sections "C3b", "D3W", "ABS"),
   `integration-log.md` rows, the round-2b-ii facts (Appendix IV items this group touches: 5, 14 if D3W cut the
   sleeve line), `handoff-root-m3b.md` (code head, docs head, exe table with tag / SHA-256 / source commit, known-red
   set, next free tag, kept / deleted outputs, open questions). Code and docs in separate commits; the ledger
   committed as it stands with your docs commits. Root's builds have priority (HOST-4).
Hand back; the PM dispatches M4 (D3 `e197b3b6`, AL-CLOCK, COV-MV, OD3P).

## B2-FIX2: fix round 2 (pool-14, `feat/p9-b2-20261003`, head `73b4acfd`; B2-N37, B2-FIX2-EXE)

Read `handoff-B2.md` (newest section), `task-B2-rereview-2.md`, `handoff-root-m2.md` (the red: item 2, the n37
tools `m2-logs/tools/n37_diag.py`, `n37_diag_any.py`), ledger B2-N37, B2-R1, B2-6, B2-FIX2-EXE.
1. Root is `fcb0200e` in code: nothing to merge now (`git merge feat/platform-p9-20261003` once at the end if root
   moved; it will, during M3). Configure `equity-dev`; ONE Debug build `p9-b2-r` of `atx-research-eval` only
   (HOST-4: second after root; wait if root builds).
2. Reproduce: `ATX_EQUITY_ROOT=C:/atx-wt/pool-2/build-equity`, `ATX_EQUITY_BIN=C:/atx-wt/pool-14/build-equity/bin`,
   `atx-impl/tools/test_nav_summ_v8.py::test_legacy_n37_numbers_reproduced`, `--tb=no --show-capture=no -rN`; diff
   with root's `n37_diag.py` form (copy it into `b2-evidence/`): paths, counts, booleans, max |relative difference|;
   print no value. Read nothing else under pool-2's `build-equity`; write nothing there.
3. Cause (a): the engine verb's number FORMATTING differs from Python's repr -> fix the verb's printing path
   (`eval_report.cpp` / `nav_summ.py`'s printing path); if the fix is in C++, a SECOND Debug build `p9-b2-s` of
   `atx-research-eval` is allowed for this round (the first was the reproduction), stated in the report; then
   re-run the test. Cause (b): arithmetic
   (summation order, fma) -> the golden stays UNCHANGED; the test's comparison becomes numeric at 1e-12 relative
   (B2-R1 (b) form), with a docstring line naming ruling B2-N37 and the B2-6 disclosure; the report states the max
   observed difference. Never edit the golden file. Never edit an expected hash.
4. Tests: the test file; the eval verb gtests from your build if (a) (named filter `EvalVerb*`); B2's own pytest
   files that touch the changed code only (TEST-1).
5. Report: "Fix round 2 (B2-N37)" in `task-B2-report.md` (cause, hunks file:line, max difference, evidence logs in
   `b2-evidence/r10-*`); handoff head line; commit (`git add -f`); delete the object tree by name (keep receipts).
Hand back; the PM dispatches re-review 3 (BASE `46ffe3dc`).

## SQL3-5: item 5 finish (pool-13, `feat/p9-sql3-20261004`, head `489d9a7e`; Python only)

Read `handoff-SQL3.md` ("Next step for a fresh agent" 2-4), `task-SQL3-report.md` item 5, the WIP commit
`489d9a7e` (`git show --stat`, the diff), ledger SQL3-1, SQL3-2, SQL3-TREE, "Dispatch SQL3 item 5" (:742).
1. Verify the three call lines as committed against the handoff's step 2 (resolver root attribute; `import
   research_store_hook` in each file); fix slips in place.
2. Tests (step 3): extend `test_one_ingest_per_write_in_a_fake_wave` (+1 ingest: the wave result) and a fake-store
   unit case per writer in `scripts/tests/test_research_store_hook.py`; the three writers' own test files
   (`test_cycle_*`, `test_wave_stage_record*` or wherever they live).
3. Run (TEST-1): `test_research_store_hook.py` (fake-store cases; the real-exe case iff
   `C:/atx-wt/pool-2/build-equity/bin/atx-research-store.exe` exists: read-only use) and the three files' own tests,
   seed 0. Teeth: one Python mutation per call line (the call removed -> the ingest-count test red), TEETH-RESTORE.
4. Report "Item 5 for the follow-up" in `task-SQL3-report.md`; handoff head line; commit (`git add -f`). SQL3-TREE:
   delete pool-13's kept object tree `build-equity` by name AFTER the report commit (keep `audits/`, the tracked
   dirs, `mega-*` receipts and logs). `git merge feat/platform-p9-20261003` before the report if root moved in code.
Hand back; the PM dispatches review 2 (BASE `69b8c162`).

## OD3P-FU: follow-up finish (pool-21, `feat/p9-od3p-20261004`, head `e7d7c23a`; Python only)

Read `handoff-OD3P.md` (STOP 9 section: the item table and "Exact next step" 1-5 are your list), the WIP
`a1348616`, `task-OD3P-rereview-1.md` (S-a .. S-d), `od3p-absent-contract.md`, pool-28 `task-ABS-report.md` (S-b
check), ledger OD3P-S, OD3P-T2V, OD3P-SKIP, OD3P-1, ALCLOCK-2.
1. Steps 1-5 of the handoff's "Exact next step" exactly: the `verb` fixture (fake / real; real skipped by a NAMED
   reason taking the exe from the same env variable root's nav_summ suites use, OD3P-SKIP); T2 on the verb; T5;
   flag-absent identity vs root's `nav_summ.py` blob `9fd6bd0a`; S-a (`other_eras` / `other_admission` /
   `other_anchor`) and S-d (`.pending` dir and file) in `test_research_cycle_od3.py`; teeth tool
   `od3p-evidence/mutate-fu.py` (TEETH-RESTORE); S-b header, S-c row, the runbook lines (OD3P-T2V label: "bundle
   rule, not G-B8"); "Follow-up round (item 6, OD3P-S)" in `task-OD3P-report.md`.
2. Run (TEST-1): the two lane test files only, seed 0. Blind: no 2020-2023 statistic.
3. `git merge feat/platform-p9-20261003` before the report if root moved in code; commit (`git add -f`).
Hand back; the PM dispatches re-review 2 (BASE `dc11c977`).

## ALCLOCK-FIX1: fix round 1 (pool-30, `feat/p9-alclock2-20261007`, head `a662d1c2`; C++)

Read `handoff-AL-CLOCK.md` "Fix round 1 (ALCLOCK-S)" (the design table and "Exact next step" are your list),
`task-AL-CLOCK-review.md` (S-1 .. S-4), ledger ALCLOCK-S, ALCLOCK-S1a, ALCLOCK-S3a, ALCLOCK-4, ALCLOCK-1 / -3,
TEETH-RESTORE, HOST-4; `.agents/cpp/agent.md`.
1. Configure `equity-dev`; write S-3 (the calibrate gtest as designed; ALCLOCK-S3a's shape), S-1 (help / refusal
   text in the three places, ALCLOCK-S1a; the capacity declaration conditionally, flag absent byte for byte), S-2
   (the registration sentence: ALCLOCK-4's two verbatim clauses; keep "C3-4"; new SHA re-quoted in "For PRE"), S-4
   (one-row fixture); shape with `atx-build.ps1 -Preset equity-dev check <file>`.
2. Commit in slices (S-3 test; S-1 / S-2 / S-4). ONE Debug build `p9-alclock-k` of `atx-impl-strategy-target-tests`
   (HOST-4: third); `InfoClock*` + the new test; `scripts/tests/test_info_clock_rule.py` seeds 0 / 1.
3. Teeth `p9-alclock-l` (the `nav_session.cpp` `if (clock_rule)` mutation, TEETH-RESTORE tool, expect only the new
   test red) ONLY if no other build is waiting; else record "teeth deferred (HOST-4)" and the reviewer may do it.
4. Append "Fix round 1 (ALCLOCK-S)" to `task-AL-CLOCK-report.md`; head line in the handoff; `git merge
   feat/platform-p9-20261003` if root moved in code (re-run the named filter only if `nav_*` / `strategy_nav_v7`
   changed in the merge); commit (`git add -f`); delete the object tree by name (keep receipts / logs).
Hand back; the PM dispatches re-review 1 (BASE `e9b3a323`). Root M4 merges the fix head if APPROVE by its slot.

## COVMV-FIX1: fix round 1 finish (pool-31, `feat/p9-covmv2-20261007`, head `12f5c094`; C++)

Read `handoff-COV-MV.md` "Fix round 1 (COVMV-S...)" (the item table and "Next step for a fresh agent" 1-6 are your
list), `task-COV-MV-review.md` (S-1 .. S-7), ledger COVMV-S, COVMV-WIP, COVMV-S6a, COV-13 (c), COV-14, TEETH-RESTORE,
HOST-4; `.agents/cpp/agent.md`.
1. Steps 1-3 of the handoff: re-run the single-TU checks that did not finish; ONE Debug build `p9-covmv-i` of
   `atx-impl-strategy-target-tests,atx-engine-book-tests` (HOST-4: fourth; `covmv-logs/tools/wait-build.ps1`); log the
   exe SHA-256s beside the runs (S-7); `MvTarget.*` (12), `MvAimRule.*` (16), the named filter (99).
2. Step 4 teeth `p9-covmv-j` ONLY if no other build is waiting; else "teeth deferred (HOST-4)" (the reviewer's one
   build may do it).
3. Step 5: "Fix round 1 (COVMV-S)" in `task-COV-MV-report.md` (hunks by S item, file:line, evidence); handoff line;
   `git merge feat/platform-p9-20261003` if root moved in code (AL-CLOCK is not yet in root: the CMakeLists tail
   keep-both row stays root's); commit (`git add -f`); delete `build-equity`'s object tree by name (keep `audits/`,
   `v8-interim3-pitch-render-run2/`, receipts, logs).
Hand back; the PM dispatches re-review 1 (BASE `e9e794b3`). Root M4 merges the fix head if APPROVE by its slot.

## D3WPB-FIX1: fix round 1 (pool-23, `feat/p9-d3w-20261005`, head `a0c7427c`; Python only; D3WPB-SEQ)

Read `task-D3W-PB-review.md` (R-1 and "What root must check at merge"), ledger D3WPB-R1, D3WPB-Q1.
1. One commit on `a0c7427c`: `scripts/tests/test_no_python_mirror.py:60, :64, :66, :69` `retired_by` -> `"P10"`; the
   comments at `:56-57` and `:61-62` cite ruling D3WPB-Q1 (the D2-5 precedent for `horizon_stats.py`); nothing else.
   Run `test_no_python_mirror.py` in its own invocation (expect 7 passed; state the function / row counts it prints).
2. Append "Fix round 1 (D3WPB-R1)" (five lines) to `task-D3W-PB-report.md` (or the lane's report file name as it
   exists), a head line in the lane's handoff; commit (`git add -f`). No build, no merge of root (the reviewer and
   root do the rest). Tree clean.
Hand back in under 100 words; the PM then resumes the review (a fresh reviewer, REV-FRESH).

## D3WPB-REVIEW-RESUME (pool-23; after D3WPB-FIX1 hands back; fresh reviewer)

Read `task-D3W-PB-review.md` whole (your predecessor's PARTIAL; "Next step (for the resuming reviewer)" 1-5 is
your list), `briefs/brief-D3W-PB.md`, ledger D3W-S, D3WPB-Q1 / Q2 / R1, D3WPB-SEQ, HOST-4, TEETH-RESTORE. HEAD = the
fix commit (the lane's hand-back names it); BASE `f0726acb`; pre-deletion `c044f5f6`; deletion `460f5ba2`.
1. Verify R-1 closed at HEAD (the four labels, the two comments; `test_no_python_mirror.py` own invocation).
2. "Next step" 1-5: configure `equity-dev`; the ONE teeth build `p9-d3wpb-c` with a TEETH-RESTORE tool (keep bytes,
   mutate `fit.cpp:340` as written, restore in a `finally` BEFORE any test, assert raw SHA-256 and blob = HEAD and
   porcelain empty), then the two named filters on the built binaries (expected reds / greens as written); the five
   fitter pytest files with those exes (only the `RealExecutables` aim-v2 subtest red: Python-side evidence only); HOST-4:
   fifth (wait while any build runs). Delete the object tree by name.
3. Verdict into `task-D3W-PB-review.md` replacing the PARTIAL header (APPROVE / CHANGES_REQUIRED); keep the file's
   "What root must check at merge" (order: D3W first, then `c044f5f6` with identity, then `460f5ba2`, then the fix
   commit; EXE-FREEZE S-f); commit ONLY that file (`git add -f`).

## A4-REVIEW-RESUME (pool-27, `feat/p9-a4-20261005`, head `9e80a7d5`; fresh reviewer; lowest build priority)

Read `task-A4-review.md` whole ("Not yet checked" 1-8 and "Exact next step" are your list), `briefs/brief-A4.md`,
`r7-dispatch.md` "A4", `wave3-carry.md` A4 items, `r8-review-notes.md` "A4, ABS, SIGN, D3W-PB", `task-A4-report.md`,
`handoff-A4.md`; ledger A4-1 .. A4-3, A4-Q1 .. Q3, A4-REV, DIRTY-2, HOST-4, TEETH-RESTORE. HEAD `7c9140c4`, pre-flip
`6dab6948`, BASE `73b4acfd`.
1. Items 1-7 as written (code from `holdings_regsho.cpp:200`; Python; the flips; the tests' identity form; the R8
   reconstruction's honesty against `a4-evidence/`; A4-Q2 read-only in pool-2: Y-F0's `--fields` manifest named in the
   P9-B0 references, grep for the two field names; house style).
2. Item 8: ONE Debug build `p9-a4-f` of `atx-engine-research-fields-tests,atx-research-fields` for your single
   leak-probe mutation (TEETH-RESTORE tool), the A4 filter on the mutated exe (probe red, rest green), restore, then
   the lane's pytests on the restored tree; the clean 25 / 25 from the lane's `p9-a4-e` log with the exe SHA checked
   against its receipt (A4-REV). HOST-4: last; wait while any build runs. Delete the object tree by name.
3. Verdict into `task-A4-review.md` replacing the PARTIAL header; "What root must check at merge" (pre-flip head
   first with the fixture identity and A4-Q2's case, then the flips; SQL2-CLS; registry rows); commit ONLY that file.
Stretch: APPROVE or P10 whole (a Required the lane cannot close in one round = P10, not a weakened verdict).

## SIGN: resume from the WIP commits (pool-29, `feat/p9-sign-20261006`, head `422e9fad`; stretch; SIGN-RESUME)

Read `briefs/brief-SIGN.md` (the spec), the three commit bodies (`git log --format=%B -3`), `git diff --stat
9c360cde..422e9fad`, `sign-logs/*.py` (the lane's tools), pool-15 `task-C3-report.md` "## Part B (C3b)" (the
`pm6-6` retirement precedent), ledger C3-11, C3B-S (`sign_pm7_35` row relabel), SIGN-RESUME, HOST-4, TEETH-RESTORE,
EXE-CUR, G-P5; `.agents/cpp/agent.md`.
1. FIRST: `handoff-SIGN.md` (what exists, file:line, per commit; what is untested; what is missing against the
   brief), committed (`git add -f`).
2. `git merge feat/platform-p9-20261003` (root's then-current head; C3b is already merged in the lane at
   `749d2c32`; if root has merged C3b by then the merge is plain).
3. Single-TU checks of the new TUs; ONE Debug build `p9-sign-a` of `atx-equity-strategy-ic,atx-impl-strategy-ic-tests`
   (HOST-4: sixth; wait while any build runs); `SignVerb*` (or the filter the gtests use); fix slips in place.
4. Pytest (TEST-1): `scripts/tests/test_wave_sign_verb.py` (the real-exe cases now run with your exe via the env
   variable the stage uses), the wave pytest files the brief names, `test_no_python_mirror.py`, the identity pytest
   (24 combinations vs the Python rule). Teeth: `sign-logs/sign-teeth-py.py` (Python) and, iff no build is waiting,
   `sign-logs/sign-teeth-cpp.py` with ONE teeth build `p9-sign-b` (TEETH-RESTORE; else "deferred").
5. `task-SIGN-report.md` (the exe and key chosen and why; verb -> file:line -> tests -> teeth; the identity procedure
   for root: argv and the ledgered wave results' screen rows BY PATH, which you do not open; what root re-proves);
   commit records; THEN the deletion of `sign_pm7_35` + its G-P5 row as the LAST commit (`sign-logs/sign-deletion.py`
   if it does that), naming the pre-deletion head in the report. Delete the object tree by name.
Hand back; the PM dispatches the first review. Merges only if APPROVE at the gate (C3-11), else P10 whole.
