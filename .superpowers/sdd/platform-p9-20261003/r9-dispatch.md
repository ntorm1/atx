# P9 RESUME 9: per-lane dispatch (PM, 2026-10-07)

ROOT-SDD = `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`. Every agent: Opus; reads the contract
(`resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md` for lanes, `resume7-addendum.md`,
`resume8-addendum.md`, `resume9-addendum.md`), then its section here, then the sections of `r7-dispatch.md` and
`r8-dispatch.md` it names (still the task list), then its handoff in its pool, then greps its ruling ids in
`progress.md`. Reviewers: `r7-review-dispatch.md` (contract), `r8-review-notes.md` (traces), `briefs/review-template.md`.
Hand-back under 200 words per `resume9-addendum.md`. Commit trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
Work ONLY in your pool. Never any git write in pools 8, 24 or 2 (root agents excepted for pool-2).

## B2: finish the two suites (pool-14, `feat/p9-b2-20261003`, head `e86d8b7c`; short)

Task list: `r7-dispatch.md` "B2", `r8-dispatch.md` "B2"; handoff `handoff-B2.md` section "R8 round" (the only
pending step); report `task-B2-report.md` section "R8 round" (two pending cells).
1. `git merge feat/platform-p9-20261003` (root is `685e238b`, docs only above `fd084e78`; expect no conflict).
2. Debug build tag `p9-b2-q`, targets as the lane's receipts list (the eval verb + the gtest targets of `p9-b2-o`).
   Memory gate: HOST-3 (B2 is second in priority; root has no build running).
3. On the merged head: `scripts/tests` under PYTHONHASHSEED 1; `atx-engine/tools/test_stage_chain.py` under seeds 0
   and 1; separate invocations; the command forms of the handoff. 0 failed, named skips only.
4. Fill the two pending cells of the report's R8 table; add the merge SHA and the new verb SHA-256 (SPDLOG-RELINK:
   name the receipt); update the handoff's head line; commit (`git add -f`).
5. Delete `build-equity` / `build-equity-rel` object trees by name after the report commit (keep `mega-*` receipts).
Hand back. The PM then dispatches re-review 2 (B2-SEQ). No code change is expected; if a suite is red, fix the cause
in the lane's own files only, commit, re-run, and say so.

## D3: record the stop, suites, hand back (pool-20, `feat/p9-d2-20261003`, head `aca78a14`; short; CRITICAL PATH)

Task list: `r7-dispatch.md` "D3" (D3-2 scope: the kind files only), `r8-dispatch.md` "D3" (C3-13 under C3-13a);
handoff `handoff-D3.md` (top section); report `task-D3-report.md` (dirty).
1. DIRTY-1: `git add -f .superpowers/sdd/platform-p9-20261003/task-D3-report.md` and commit it exactly as it stands
   (`docs(d3): report as it stood at STOP 8 (DIRTY-1)`).
2. `git merge feat/platform-p9-20261003` (root `685e238b`, docs only; expect no conflict).
3. On the merged head: `scripts/tests/test_wave_kinds.py`; whole `scripts/tests` seeds 0 and 1; the stage chain
   seeds 0 and 1; separate invocations. Check `d3-logs/teeth.log` covers the C3-13 hunks (`0c9f75d9`); if not, run the
   teeth for them (TEETH-RESTORE) and log it. The live-table test runs only if `ATX_EQUITY_BIN` names root's current
   Debug bin (`C:/atx-wt/pool-2/build-equity/bin`, exes of `handoff-root-m1.md`); skip otherwise and say so.
4. Fill the report's suites section and the C3-13 section (commit, tests (a) / (b), teeth); write a short "Resume 9
   close" section at the top of the handoff: head, root SHA merged, suite counts, what root re-proves at merge (the
   C3-5 probes (a) and (b), C3-13; the keep-both with C3b's eight fixture lines and three `test_wave_kinds.py` hunks).
   Commit (`git add -f`).
No build (Python only). Hand back; the PM dispatches the first review.

## D3W-REVIEW: first review (pool-23, `feat/p9-d3w-20261005`, code head `1e6c4336`)

Reviewer. BASE = `git merge-base 1e6c4336 feat/platform-p9-20261003` (REV-BASE). Spec: `r7-dispatch.md` "D3W",
`r8-dispatch.md` "D3W", `r8-review-notes.md` "D3W"; rulings D3-1, D3-2, D3W-EVAL, ABS-1, D3W-PB, ALCOMB-4 .. -7, D2-9,
D2R-3, PRE-35, PRE-38, D3W-STALE. Report `task-D3W-report.md`; handoff `handoff-D3W.md`.
0. DIRTY-1a: first act, commit the dirty `handoff-D3W.md` exactly as it stands (`git add -f`;
   `docs(d3w): handoff as it stood at STOP 8 (DIRTY-1)`); the `@@ROOT3@@` placeholder stays (root did not move in code).
1. Review the code `BASE..1e6c4336`. The lane deleted its object trees: gtests from the lane's logs only
   (`d3w-logs/`, receipts `build-equity/mega-p9-d3w-*`), unless you judge a build necessary (then HOST-3: priority
   below AL-CLOCK / COV-MV; configure `equity-dev`; one tag `p9-d3w-i`; delete the tree by name after).
2. Run yourself: the fitter / composition pytest files the report names and `scripts/tests/test_fit_walk_forward.py`
   (no exe needed); the flag-absent identity with `d3w-logs/tools/ident.py` if it runs without an exe, else state so.
3. Teeth: re-do two of the lane's Python mutations yourself (TEETH-RESTORE).
4. Quality verdict (owner priority): is the walk-forward fit in `atx-engine` modular and reusable (one fit entry, no
   duplicated window logic in the runner), is the v3 weights reader bounded and allocation-free in the hot path, is
   anything a Python mirror of the C++ rule.
Write `task-D3W-review.md` in pool-23's sprint dir (commit `git add -f`): spec verdict item by item (Required /
Suggested), quality verdict, "What root must check at merge", "For ABS" (does the extension point named in the report
exist and suffice for `od3p-absent-contract.md` sections 2-5). Verdict APPROVE / CHANGES_REQUIRED.

## OD3P-REREVIEW: re-review 1 (pool-21, `feat/p9-od3p-20261004`, head `97a8157f`)

Reviewer. BASE `d78d23ce` (the SHA review `91fae138` names; REV-BASE). Spec: `r7-dispatch.md` "OD3P", the first
review `task-OD3P-review.md` (R-1 Required; S-1 .. S-6), the dispatch "OD3P fix round 1" in the ledger (S-2 and S-4
required by the PM; S-6 is root's), `r8-review-notes.md` "OD3P"; rulings OD3P-1, ALCLOCK-2, PRE-31, ABS-1.
0. DIRTY-1a: first act, commit the dirty `handoff-OD3P.md`, `od3p-absent-contract.md`, `task-OD3P-report.md`
   exactly as they stand (`git add -f`; `docs(od3p): records as they stood at STOP 8 (DIRTY-1)`).
1. Review `d78d23ce..97a8157f`: R-1 closed as the review asked (each history book its own fresh `fit.output`; P7 stops
   unless the fit is pending); S-1 .. S-5; the runbook lines; the contract unchanged or changed only where the report
   says. Run the two lane test files and the whole `scripts/tests` seed 0 yourself (Python only); re-do two of the
   eight teeth mutations (TEETH-RESTORE). Blind check (no 2020-2023 statistic in runbook / report / contract).
Write `task-OD3P-rereview-1.md` (commit `git add -f`). Verdict APPROVE / CHANGES_REQUIRED; "What root must check at
merge" (incl. SQL2-CLS literals `atx.carry-weights/v1`, `atx.od3-flagged-members/v1`; the `mv-aim-v1` pin in
`flagged_members.json` is root's at P0).

## C3b: record the stop, finish steps 3-8 (pool-15, `feat/p9-c3-20261004`, head `3129ca14`)

Task list: `r7-dispatch.md` "C3b", `r8-dispatch.md` "C3b"; handoff `handoff-C3.md` section "C3b at the second owner
stop" (its "Next step for a fresh agent" 1-8 is the list); rulings C3-11, C3-12, C3-13, C3-14, PRE-40, E2-16.
1. DIRTY-1: commit `c3-logs/c3b-gt-exits.txt` and `c3b-pyt-exits.txt` as they stand (`git add -f`;
   `docs(c3b): logs as they stood at STOP 8 (DIRTY-1)`).
2. Reconstruct the R8 round from `git log 278fc2d8..3129ca14` (S-13 `f41719c2`; merges `7c038124`, `3129ca14`),
   `c3-logs/` and `build-equity/mega-p9-c3-*` receipts: which of steps 2-6 ran and with what result; write it into the
   handoff as "R8 round (reconstructed from logs)" with the next free tag; nothing is counted done without a log or a
   receipt. Is the kept Debug tree still there and current (EXE-CUR vs `3129ca14`)?
3. Finish steps 3-8 as written (C++ teeth under TEETH-RESTORE, never `git checkout`; Debug canary; Release build +
   named 34 + whole 391 + canary; C3-14 for a wall-time miss; `scripts/tests` seeds 0 and 1 and the stage chain on the
   merged tree; "## Part B (C3b)" in `task-C3-report.md`). `git merge feat/platform-p9-20261003` first (root
   `685e238b`). Item 6 only if B2's redo is in root (check `git -C C:/atx-wt/pool-2 log --format=%s -30`), else pending.
   HOST-3: C3b is third in build priority.
4. Commit records (`git add -f`); delete the object trees by name after the report commit (keep receipts / logs).
Hand back; the PM dispatches the re-review.

## AL-CLOCK: continue in the fresh pool (pool-30, `feat/p9-alclock2-20261007`, head `1cbe5233`; TEETH-POOL)

Task list: `r7-dispatch.md` "AL-CLOCK", `r8-dispatch.md` "AL-CLOCK"; rulings ALCLOCK-1 / -2 / -3, W3-9, W3-16, C3-5,
C3-13, PRE-7a, TEETH-POOL, TEETH-RESTORE, HOST-3. Pool-8 is FROZEN: read it, never write it, never run git there
except `git -C C:/atx-wt/pool-8 status --short` / `diff` / `log` for reading.
1. Compare `C:/atx-wt/pool-8/.superpowers/sdd/platform-p9-20261003/` with your pool's: copy (plain `cp`) every
   AL-CLOCK record that is missing or newer in pool-8 (`handoff-AL-CLOCK.md` (dirty there), `task-AL-CLOCK-report.md`,
   `alclock-logs/**` incl. `tools/`, `teeth-set1.diff`) into your pool's sprint dir; fill `@@HEAD@@` in the handoff
   with `1cbe5233`; add a line "continued in pool-30 on `feat/p9-alclock2-20261007` (TEETH-POOL)"; commit
   (`git add -f`; `docs(al-clock): records copied from frozen pool-8 at STOP 8 (DIRTY-1 / TEETH-POOL)`). Fix the
   pool path inside `alclock-logs/tools/*.sh` / `teeth.py` if they hard-code pool-8 (lane-local tools, allowed).
2. `git status --short` must be empty apart from ignored files. Configure: `powershell scripts\atx-build.ps1 configure
   -Preset equity-dev` from pool-30.
3. Teeth set 2 (handoff step 2) ONLY under TEETH-RESTORE (the tool restores the bytes itself and asserts the SHA);
   if `teeth.py` cannot restore, skip set 2 and say so (set 1 M10 stands).
4. Handoff steps 3-7: merge root (`685e238b`), Debug `p9-alclock-h` (canary, named, whole), Release configure +
   `p9-alclock-i` (named, whole, canary), suites if root moved in code (it did not: `fd084e78` is merged), fill the
   report's `@@...@@` lines, commit, delete the object trees by name (keep receipts / logs in `alclock-logs/`).
   HOST-3: fourth in build priority (wait if B2 or C3b is building).
Hand back; the PM dispatches the first review. Root merges `feat/p9-alclock2-20261007`.

## COV-MV: continue in the fresh pool (pool-31, `feat/p9-covmv2-20261007`, head `b6e304ee`; TEETH-POOL)

Task list: `r7-dispatch.md` "COV-MV", `r8-dispatch.md` "COV-MV"; handoff `handoff-COV-MV.md` "Session R8" (its "Next
step" 2-5); rulings COVMV-1, COV-11, COV-13, COV-14, COV-4, W3-9, TEETH-POOL, TEETH-RESTORE, HOST-3. Pool-24 is FROZEN
(read-only). Its records (`handoff-COV-MV.md`, `task-COV-MV-report.md`, `covmv-logs/`) are committed at `b6e304ee`:
check `ls` of pool-24's sprint dir against yours and copy anything newer (expected: nothing).
1. Add to the handoff: "continued in pool-31 on `feat/p9-covmv2-20261007` (TEETH-POOL); the two `// TEETH` lines
   stay uncommitted in pool-24 for the owner"; commit (`git add -f`).
2. Configure `equity-dev`; handoff step 2: Debug canary `p9-covmv-f` (the four targets), `MvAimRule.*`, `MvTarget.*`,
   the named filter and the whole target-tests on the merged head (`db33c6ca` is in your history: this is the Debug
   re-run the lane owed). Step 3: Release configure + `p9-covmv-g` (named, whole, canary). Step 4: `git merge
   feat/platform-p9-20261003` (root `685e238b`; AL-CLOCK not yet in root: note the keep-both row for root). Update
   the report (evidence, "Pending"), commit. Step 5: delete the object trees by name (keep receipts / logs).
   Any new teeth: TEETH-RESTORE only. HOST-3: fifth in build priority.
Hand back; the PM dispatches the first review. Root merges `feat/p9-covmv2-20261007`, after AL-CLOCK.

## A4: commit the stop, reconstruct, continue (pool-27, `feat/p9-a4-20261005`, head `1e4904b7`; stretch)

Task list: `r7-dispatch.md` "A4" (:249-279), `briefs/brief-A4.md`, `wave3-carry.md` A4 items (and the "Brief text
superseded" entry for `brief-A4.md`, W3-13, Q13, M-4), `r8-dispatch.md` if it names A4; rulings A4-1, A4-2, A4-3,
DIRTY-2, HOST-3, TEETH-RESTORE. Handoff `handoff-A4.md` is STALE (2026-10-05): the R8 agent did a large slice of work
and never committed or handed back.
1. DIRTY-2: first act, `git add -A -- atx-engine` (every path `git status --short` lists; they are all under
   `atx-engine/`; never a cache or object tree) and commit `WIP(a4): work uncommitted at STOP 8 (DIRTY-2; untested as
   committed)`.
2. Reconstruct: `git diff --stat 1b64ce45..HEAD`, read every new file and hunk; write a new top section of
   `handoff-A4.md` ("R8 slice, reconstructed"): what exists (file:line), what compiles / is tested = unknown until you
   build, what is missing against the dispatch steps 2-5; commit (`git add -f`).
3. `git merge feat/platform-p9-20261003` (root `685e238b`; T2 `dfa4218d` is already merged at `ebfb333e`).
4. Continue steps 2-5 of `r7-dispatch.md` "A4": clock primitives, FTD / RegSHO stage readers, builder kinds + registry
   rows, planted-leak probes with teeth, fixture byte identity vs the Python builders, A1-SHIM / S-R2 / A3 S-2 / A3-3 /
   S-6, the `kind: engine` flips as their own commits at the end, Debug / Release builds, fields gtests, pytests,
   `task-A4-report.md`. Owner's bar: modular, reusable, performant C++ in `atx-engine` (one reader interface for the
   short-side sources, bounded memory, no per-row allocation in the hot loop; `.agents/cpp/agent.md`). Implement
   first, then the tests the brief names (no TDD). HOST-3: lowest build priority; do all reading / coding before the
   first build; configure `equity-dev` (an `a4-configure-dev.log` exists; re-run the configure).
5. Commit in slices (code, then tests, then the flips); records `git add -f`; delete object trees by name at the end.
Hand back (DONE / DONE_WITH_CONCERNS with the exact list of what is not done); the PM dispatches the first review.
A4 merges only if APPROVE at the gate (A4-1); else P10 whole, nothing of it in the gate head.

## ROOT (pool-2; one root agent at a time; dispatched by the PM at the first APPROVE)

Per `r7-root-merge.md`, starting from `handoff-root-m1.md`. Step 0 of the first root agent of this resume: one
housekeeping docs commit of the sprint-dir files as they stand: `progress.md`, `integration-log.md`, and `git add -f`
for the untracked `resume9-addendum.md`, `r9-dispatch.md`; NEVER the PNGs under `docs/plans/`, never a cache dir.
Then the merges the PM's prompt names, in `r7-root-merge.md`'s order (B2, T2, C3b, D3, AL-CLOCK from
`feat/p9-alclock2-20261007`, COV-MV from `feat/p9-covmv2-20261007`, D3W, OD3P; then ABS, D3W-PB, SQL4, SIGN, A4 if
APPROVE), with the C3-5 probes (a) and (b) at D3's merge (C3-13) and with `--rate info-clock-rate-v1` at AL-CLOCK's
merge, SQL2-CLS for every new literal, B2-EVAL-KEEP at the second of B2 / D3W, SQL3-2 (no catalog at the default
path) before any P9-B0 run, and ROOT-1 at every merge. Records: `root-wave3-merge-report.md` (B2 and T2 go in
`root-wave2-merge-report.md`), `integration-log.md`, the round-2b-ii facts for the prereg, `handoff-root-<group>.md`.
Build tags from `p9-2t`. Root's builds have priority (HOST-2 / HOST-3).
