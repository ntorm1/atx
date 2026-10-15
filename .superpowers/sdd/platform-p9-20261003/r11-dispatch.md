# P9 RESUME 11: per-agent dispatch (PM, 2026-10-15)

ROOT-SDD = `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`. Every agent: Opus; reads the contract
(`resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md` for lanes, `resume7-addendum.md`,
`resume8-addendum.md`, `resume9-addendum.md`, `resume10-addendum.md`, `resume11-addendum.md`), then its section
here, then the files its section names, then greps its ruling ids in `progress.md`. Reviewers:
`r7-review-dispatch.md` (contract), `r8-review-notes.md` (traces), `briefs/review-template.md`. Hand-back under
200 words per `resume11-addendum.md`. Commit trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
Work ONLY in your pool (`cd` there first: the shell starts in `C:/atx`, where nothing is ever run). Never any git
write in pools 8, 24 or 2 (root excepted).

## ROOT M5 (pool-2; one root agent; the whole "Dispatch root M5" list)

Read `r7-root-merge.md`, `r10-dispatch.md` "ROOT M4" (the form of a root group), `handoff-root-m4.md` (whole; its
"What the next root group (M5) must know" and "Exes (EXE-CUR)"), `handoff-root-m5.md` (the STOP 10 stub: nothing
landed), ledger `progress.md` :835 ("Dispatch root M5": YOUR LIST, order and details verbatim) and the RESUME 10 /
RESUME 11 blocks; then each lane's "What root must check at merge": pool-14 `task-B2-rereview-3.md`, pool-21
`task-OD3P-rereview-2.md`, pool-23 `task-D3W-PB-review.md` + `task-D3W-PB-report.md` (TRAIN identity procedure,
`d3w-logs/tools/pb-ident.py`, `cmp.py`), pool-29 `task-SIGN-review.md` + `task-SIGN-report.md` (the identity
procedure by path), pool-27 `task-A4-review.md` + `task-A4-report.md`.
0. Housekeeping docs commit, as the files stand (never edit a ledger line): `progress.md`, `next-goal-prompt.md`;
   `git add -f` `status-11.md`, `resume11-addendum.md`, `r11-dispatch.md`. NEVER the PNGs under `docs/plans/`,
   never a cache dir.
1. B2 fix 2 at `101f695a` (test only). EXE-CUR first: is `atx-research-eval` of `p9-2y` current by source for the
   n37 test? (the ledger line says root's eval verb is current; `handoff-root-m4.md` "Exes" decides; rebuild
   `atx-research-eval` as `p9-2z` only if stale). Re-run `test_legacy_n37_numbers_reproduced` with
   `ATX_EQUITY_ROOT=C:/atx-wt/pool-2/build-equity`, `ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin`, blind flags
   `--tb=no --show-capture=no -rN`: must pass; the known-red set becomes EMPTY. B2-S3 S-1 docstring slip. If it
   does not pass: stop that chain (merge stays; BLOCKED for that lane; go on).
2. OD3P follow-up at `4a7372e2` (Python). The two lane files with the verb: 96 passed / 0 skipped (OD3P-SKIP: the
   real-verb case must RUN; rebuild `atx-research-eval` first if stale by source); `nav_summ.py` flag-absent identity
   vs blob `9fd6bd0a` on the listed cases; `--pool-one-sided` only in runbook R4; OD3P-S2 S-4 `classes.json`
   one-string slip iff guard + store suite stay green.
3. D3W-PB chain: `c044f5f6` (ONE Debug build of `atx-engine-research-composition-tests,atx-impl-strategy-ic-tests,
   atx-research-composition,atx-research-admission`; the TRAIN identity procedure of the lane's report: `pb-ident.py`
   on BASE `f0726acb`'s exes = root's `p9-2v` and on the merged tree's new exes with the SAME work dir, `cmp.py`:
   expected 21 / 24 identical, the 3 `engine-*` weights files differing only at
   `provenance.engine_fit.producer_fingerprint`, `OUTPUT_PRODUCERS` `d28a3089`; the two named filters; the five
   fitter pytest files 180 / 0), then `460f5ba2` (`test_composition_resid.py`, `test_no_python_mirror.py`), then
   `44a331e6` (S-g keep-both in `test_no_python_mirror.py`: root's B2 comment line, then the fix's two D3WPB-Q1
   lines; 7 passed; with `P9_FREEZE` only E2's `sign_pm7_35` row remains, until SIGN's deletion lands in step 4).
4. SIGN chain: `c2010ed1` first (ONE Debug build of `atx-equity-strategy-ic,atx-impl-strategy-ic-tests`;
   `SignVerb*` 6; root's identity on the ledgered wave results' screen rows BY PATH as `task-SIGN-report.md` lists
   them: the verb's decisions row for row = the Python rule's, compared as decisions and reasons only, NO statistic
   read; the wave pytest files + `test_wave_sign_verb.py` with the exe; `test_wave_driver.py` 28), then `50a43599`
   (the deletion: `test_no_python_mirror.py` 7, the `sign_pm7_35` row gone, `FROZEN_PAIRS`), then `e2844926`;
   SQL2-CLS row `atx.sign-receipt/v1` class `ic-output` in `classes.json` (SIGN-CLS-2; guard before / after; store
   suite).
5. A4 chain: `6dab6948` first (ONE Debug build of `atx-engine-research-fields-tests,atx-research-fields`; the A4
   filter 25; the fixture identity; A4-Q2: BOTH `ftd_shares_ratio21` and `regsho_threshold_days63` are in Y-F0's
   manifest `train-2020-2023-lo3-fields-v15` `26fee5ce`: run the engine builder on the real TRAIN inputs for both
   fields through the bounded runner (`--seconds` <= 600, `--max-rss-mib 2048`, SEAL-ALLOW-3 by source check) and
   compare its payload bytes to the Python builder's files in that fields dir by SHA-256 only; equal = identity of
   record; a miss STOPS A4's merge: BLOCKED, the flips not merged, hand back), then the flips `69141218`,
   `59d807ea`, then the APPROVE head of record: the newest `Task A4: APPROVE at <sha>` line of the ledger at that
   moment (`fc3e414e` unless a fix-1 APPROVE line exists; the lane's fix-1 work above `fc3e414e` is NOT merged in M5;
   registry rows vs code; SQL2-CLS none).
6. At every merge: tree = `git merge-tree` recompute where no conflict; MERGE-SHA (the APPROVE SHA, never a branch
   tip); SQL3-2 (no `build-equity/research-store/` in pool-2); trial ledger 133 `27e40f9f` start and end; ENV-1;
   EXE-CUR by receipt; build tags from `p9-2z` upward, single-use. Whole suites: deferred to the gate (TEST-1). A
   design conflict or an unattributable red stops THAT chain at a lane boundary (the merge not reverted; BLOCKED for
   that lane; go on with the next independent one). Root's builds have priority (HOST-4); HOST-5 for foreign builds.
7. Records: `root-wave3-merge-report.md` (APPEND sections "B2 fix 2", "OD3P follow-up", "D3W-PB", "SIGN", "A4"),
   `integration-log.md` rows, the round-2b-ii facts (Appendix IV items this group touches), `handoff-root-m5b.md`
   (code head, docs head, exe table with tag / SHA-256 / source commit, known-red set, next free tag, kept /
   deleted outputs, open questions; `handoff-root-m5.md` stays as the STOP 10 record). Code and docs in separate
   commits; the ledger committed as it stands with your docs commits. Delete your own object trees by name after the
   records commit (keep receipts and the exes the gate needs: state which in the handoff).
Hand back; the PM dispatches M6 (AL-CLOCK fix head iff APPROVE, SQL3 item 5 after review 2, the A4 fix head iff
APPROVE, SQL4 iff APPROVE: LAST) and then the gate (`root-gate-brief.md`).

## ALCLOCK-REREVIEW-1 (pool-30, `feat/p9-alclock2-20261007`; fresh reviewer; ALCLOCK-REV-RESUME)

Read `task-AL-CLOCK-rereview-1.md` whole (your predecessor's PARTIAL; its "Exact next step" 9 steps are your list;
its read table says what is read), `task-AL-CLOCK-review.md` (S-1 .. S-4), `task-AL-CLOCK-report.md` "Fix round 1
(ALCLOCK-S)" and "For PRE", `handoff-AL-CLOCK.md`, the lane's `alclock-logs/` (builds `p9-alclock-k / -l / -m`:
ALCLOCK-M); `r7-review-dispatch.md`, `r8-review-notes.md` "AL-CLOCK", `briefs/review-template.md`,
`r10-dispatch.md` "ALCLOCK-FIX1" and the AL-CLOCK bullet of "Scoped re-reviews"; ledger ALCLOCK-S, ALCLOCK-S1a,
ALCLOCK-S3a, ALCLOCK-4, ALCLOCK-M, ALCLOCK-REV-RESUME, REV-FRESH, TEST-1, HOST-4, TEETH-RESTORE, EXE-CUR.
HEAD `f892eab1`; BASE `e9b3a323`; the range holds root merges (`65ec16af`, `1d603d23`, `38967882`): check their
plainness as the PARTIAL says.
1. The 9 steps. The lane's own teeth `p9-alclock-l` CAUGHT (the lane's account): verify from its log and the
   receipt SHA-256 (EXE-CUR); your ONE build (`p9-alclock-n`) only if an item cannot be proved from logs + receipts
   (HOST-4: third; wait while root or A4 builds). If pool-30 holds no object tree, say so and rely on the receipts.
2. Verdict into `task-AL-CLOCK-rereview-1.md` replacing the PARTIAL header (APPROVE / CHANGES_REQUIRED; Required /
   Suggested); "What root must check at merge" (M6: the second AL-CLOCK merge at the fix head; the named filter
   `InfoClock*` + the calibrate gtest; `test_info_clock_rule.py` seeds 0 / 1; the registration sentence ALCLOCK-4;
   flag absent byte for byte); commit ONLY that file (`git add -f`). Delete your object tree by name if you built.
Hand back. Until a `Task AL-CLOCK: APPROVE at <sha>` ledger line names the fix head, `e9b3a323` stays of record.

## SQL3-5-FINISH (pool-13, `feat/p9-sql3-20261004`, head `d43b3a8b`; Python only; no build)

Read `handoff-SQL3.md` (newest section, OWNER STOP 10: done / not done / the TEETH-CRLF fix / next step),
`task-SQL3-report.md` item 5, the code commit `b55c0d11` (`git show --stat`, the diff), `r10-dispatch.md` "SQL3-5",
ledger SQL3-1, SQL3-2, SQL3-TREE, SQL3-WQ, TEETH-CRLF, "Dispatch SQL3 item 5" (:742), the STOP 10 hand-back line
for SQL3 (RESUME 10 block, near the end).
1. SQL3-WQ: `scripts/wave_queue.py:146` (item 1's file) carries the same relative-root pattern: one hunk (pass the
   root-relative name as the three item-5 call lines now do) plus one test case under a relative `--root` in the
   wave-queue test file; listed in the report.
2. Teeth with the TEETH-CRLF form: one Python mutation per item-5 call line (the call removed -> the ingest-count
   test red), restore, assert `git hash-object <path>` = `git rev-parse HEAD:<path>` and the raw SHA-256 of the
   working bytes = your saved copy, porcelain empty (TEETH-RESTORE). Evidence under `sql3-evidence/r11-*`.
3. Run (TEST-1, seed 0, explicit paths, own `--basetemp`): `scripts/tests/test_research_store_hook.py`,
   `test_research_store_identity.py`, the seven writers' test files, the wave-queue test file; exes read-only from
   root: `ATX_EQUITY_ROOT=C:/atx-wt/pool-2/build-equity`, `ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin` (the
   `p9-2y` set; EXE-CUR: log the SHA-256 of each exe you use beside the run). Read nothing else under pool-2's
   `build-equity`; write nothing there.
4. `git merge feat/platform-p9-20261003` once before the report if root moved in code (root M5 is merging now; a
   plain merge is expected; conflicts only in your own files, region by region). "Item 5 for the follow-up" section
   in `task-SQL3-report.md` (+ the SQL3-WQ paragraph: hunk file:line, the test case, teeth results); handoff head
   line; commit (`git add -f`).
5. SQL3-TREE: delete pool-13's kept `build-equity` object tree by name AFTER the report commit (keep `audits/`, the
   tracked dirs, `mega-*` receipts and logs); state the bytes freed.
Hand back; the PM dispatches review 2 (BASE `69b8c162`, file `task-SQL3-review-2.md`).

## SQL4-FINISH (pool-32, `feat/p9-sql4-20261008`, head `73a871fb`; C++; stretch; SQL4-GO; SQL-12)

Read `handoff-SQL4.md` whole (the STOP 10 state: tasks 1-4 coded uncompiled; the CMake lines for the new TU and
the store-library link; pending items), `briefs/brief-SQL4.md` (the spec), `sql-design.md` (the sections the brief
names), `r10-dispatch.md` "SQL4" (its steps 2-5 remain yours), ledger SQL4-GO, SQL4-Q1 (P10: the marginal verb reads
signals only from sidecars; stated in the report and handoff), SQL4-Q2 (the lane's design accepted provisionally;
the reviewer judges on the owner's bar: modular, no duplicated plumbing, a null index = today's path byte for
byte), SQL-1 .. SQL-9, SQL-11, SQL-12, EXE-FREEZE, EXE-CUR, HOST-4, HOST-5, TEST-1, TEETH-RESTORE, TEETH-CRLF, G-P10;
`.agents/cpp/agent.md` before any C++. The lease's `dev` configure failed (known): ignore `build/`, configure
`equity-dev` yourself.
1. `git merge feat/platform-p9-20261003` (root's current head; it moves during M5: merge again before the report).
   Never rebase. Conflicts in your hunks only, region by region.
2. CMake: the new TU into the strategy library's source list and the store library linked into
   `atx-impl-strategy` as the handoff gives the lines; single-TU `check` of every new / changed TU
   (`powershell scripts\atx-build.ps1 -Preset equity-dev check <file>`); fix slips in place; commit the compile fix
   as its own slice.
3. Tasks in order, committed in slices: 1 remainder (the marginal `--pair-cache-index` flag), 5 (pair cache), 6
   (ledger index head; `verify --ledger`), 7 (tests; CMake tail lines). Forbidden list and "Out of scope" as the
   brief says; no Python file; no pinned source list or SHA; flag absent = every output byte unchanged.
4. TEST-1: ONE Debug build per slice at most (tags `p9-sql4-a`, `-b`, ...) of
   `atx-equity-strategy-ic,atx-impl-strategy-ic-tests` and once of
   `atx-research-store,atx-engine-research-catalog-tests` through `scripts/research-build.ps1` (HOST-4: fourth; wait
   while any of OUR builds runs, 60 s quiet, bound 180 min; HOST-5 for foreign ones); named filters
   `IcCacheIndex.*:IcResultIndex.*:MarginalPairIndex.*:StrategyIcRunner.*:MarginalIc.*` and `ResearchCatalog.*`; the
   three source tripwires unchanged; ONE teeth build (every refusal disabled at once; a TEETH-RESTORE tool in the
   TEETH-CRLF form). Release, whole targets, pytest suites: the gate's.
5. Flag-absent identity for root: on the tiny-world fakes in the lane (with and without an `index.sqlite` present,
   no flag: bytes identical; with the flag: identical outside the listed blocks); the real X-5 u / w identity and
   the marginal shard identity are ROOT's procedure at merge (write the argv and the expected-identical file list in
   the report).
6. `task-SQL4-report.md` + `handoff-SQL4.md` (`git add -f`; the SQL4-Q2 deviation and the SQL4-Q1 P10 item stated);
   `git merge feat/platform-p9-20261003` again before the report; delete the object trees by name (keep receipts
   and logs). If you run low on context: `WIP(sql4): ...`, handoff, DONE_WITH_CONCERNS.
Hand back; the PM dispatches the first review. Merges only if APPROVE and in root before the gate's P9-B0 re-pin
(SQL-12); the PM may stop the lane at the gate: then P10 whole, nothing of it in P9.

## A4-FIX1 (pool-27, `feat/p9-a4-20261005`, head `e982afef`; tests only; A4-S)

Read `handoff-A4.md` "RESUME 10 fix round 1 (A4-S)" (the design: S-1 seal rows, S-2 one Reg SHO probe per clock;
fixture rows planted through `make_holdings_fixture.py` with T2's writers unchanged; the trap: `pq.read_table` under
`year=2023/` adds a hive column, use `pq.ParquetFile(path).read()`), `task-A4-review.md` (S-1, S-2, "What root must
check at merge"), `task-A4-report.md`, `r10-dispatch.md` "A4-REVIEW-RESUME" (the build form), ledger A4-S, A4-BUILD,
A4-Q2, A4-REV, HOST-4, TEST-1, TEETH-RESTORE, TEETH-CRLF; `.agents/cpp/agent.md`.
1. The fixture rows (seal row(s) and late-list rows) via `make_holdings_fixture.py`; four seal gtests + three
   per-clock probe gtests in the lane's fields test file(s); shape with single-TU `check`. TESTS ONLY: no production
   source edited (a production hunk needed = stop, state why in the handoff, hand back DONE_WITH_CONCERNS).
2. Re-configure `equity-dev` first (A4-BUILD), then ONE Debug build `p9-a4-h` of
   `atx-engine-research-fields-tests,atx-research-fields` (HOST-4: second; wait while root builds); the A4 filter
   (the 25 + the 7 new); the lane's pytests that touch the fixture; the fixture identity the review names.
3. Teeth `p9-a4-i` (the probe mutation; TEETH-RESTORE tool in the TEETH-CRLF form; expected: the probe tests red,
   the rest green) ONLY if no other of our builds is waiting; else record "teeth deferred (HOST-4)" (the reviewer's one
   build may do it).
4. "Fix round 1 (A4-S)" section in `task-A4-report.md` (hunks file:line, counts, evidence under `a4-evidence/r11-*`);
   handoff head line; `git merge feat/platform-p9-20261003` if root moved in code (root merges `fc3e414e` at M5:
   plain); commit (`git add -f`); delete the object tree by name (keep receipts and logs).
Hand back; the PM dispatches the scoped re-review (BASE `fc3e414e`, file `task-A4-rereview-1.md`). A4-S: this
round never holds the gate; root merges `fc3e414e` at M5 regardless; the fix head goes in at M6 iff APPROVE.

## Scoped reviews of this wave (fresh reviewers; REV-FRESH; FU-REVIEWS; dispatched on hand-back)

Each: read `r7-review-dispatch.md`, `resume9-addendum.md` (TEST-1), `resume10-addendum.md`,
`resume11-addendum.md`, the previous review file(s) in the pool, the round's section of the lane report / handoff,
the ruling that ordered the round; BASE = the previous APPROVE SHA; HEAD = the lane's hand-back head; review
BASE..HEAD only; re-run the lane's named filter (if the lane left a built tree or its logs name the receipt:
EXE-CUR) and the lane's pytest files; one own mutation if cheap (TEETH-RESTORE, TEETH-CRLF); verdict APPROVE /
CHANGES_REQUIRED; "What root must check at merge"; commit only the review file (`git add -f`).
- SQL3 review 2: pool-13, BASE `69b8c162`, file `task-SQL3-review-2.md`; `r8-review-notes.md` "SQL3": the hook
  never changes an exit code or a JSON byte; one ingest per write; catalog off = no subprocess; SQL3-WQ's hunk
  and test; the TEETH-CRLF teeth evidence; Python only.
- SQL4 first review: pool-32, BASE = merge-base with root, file `task-SQL4-review.md`; the brief's "Root verifies"
  and "Flag-absent identity"; the three tripwires; no Python; no pinned list moved; lock order documented; SQL4-Q2
  judged on the owner's bar; SQL4-Q1 stated as P10. Stretch: APPROVE or P10 whole.
- A4 re-review 1 (scoped): pool-27, BASE `fc3e414e`, file `task-A4-rereview-1.md`; A4-S S-1 (four seal gtests),
  S-2 (three per-clock probes), tests only (the diff touches no production source), the fixture rows; teeth if the
  lane deferred it (the reviewer's one build, HOST-4 last).

## ROOT M6 (pool-2; after M5 hands back; the PM's prompt names the heads)

Per `r7-root-merge.md` and `handoff-root-m5b.md`. Merges, in order, each at its APPROVE SHA (MERGE-SHA), each with
the review's "What root must check at merge" + ROOT-1a targeted checks, one Debug build per C++ lane at most:
AL-CLOCK fix head (iff `Task AL-CLOCK: APPROVE at <sha>`), SQL3 item 5 (iff review 2 APPROVE; Python), the A4 fix
head (iff APPROVE; tests only), SQL4 (iff APPROVE; LAST; the real X-5 u / w identity and the marginal shard identity
per its report). Records as in M5; `handoff-root-m6.md`. Then the gate (`root-gate-brief.md`, GATE-1) as the PM's
next prompt.
