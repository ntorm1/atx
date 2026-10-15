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

## WAVE REVIEW (GATE-1 step 8; fresh reviewers by area; WAVE-REV-EARLY; dispatched 2026-10-15 ~12:10)

Range: BASE `3c9eeb01` (the wave-1 gate head) .. HEAD `d51d011e` (root after M6: every wave-2 + wave-3 merge except
SQL4). If SQL4 merges at M6b, the IC reviewer (continued) reviews `d51d011e..<M6b head>` as an addendum to its file;
the other areas are unaffected by SQL4 (its files are IC / store only; the store reviewer gets the addendum too).
Five reviewers, one per area, each a fresh Opus agent, read-only in `C:/atx-wt/pool-2` (NO git write there; NO build:
root's gate and SQL4 own the host, HOST-4; Python tests allowed, own `--basetemp` under the session scratchpad; one own
Python mutation per area if cheap, restored, TEETH-CRLF form; C++ claims proved from the lanes' logs and receipts under
ROOT-SDD `*-logs/`, `build-equity/mega-*-receipt.json`, the lane reports and reviews in the pools' sprint dirs). Read
code by SHA (`git show d51d011e:<path>`, `git diff 3c9eeb01..d51d011e -- <path>`), never from the working tree (root
may be merging under you).

Area precedence for a file in two areas (the first listed reviews it; the next cites it):
1. **fields** (`wave-review-fields.md`): `atx-engine/src/research/fields*`, `atx-engine/include/atx/research/fields*`,
   `atx-engine/src/research/holdings*`, `atx-engine/tests/research/*fields*`, `*holdings*`, `atx-engine/tests/fixtures/`,
   the field registry rows under `scripts/specs/` (lanes A4, T2, ABS's field side). Judge PIT / seal safety first.
2. **store / catalog** (`wave-review-store.md`): `atx-core/src/db`, `atx-core/include/atx/db*`, every `*store*` /
   `*catalog*` TU under `atx-engine/` and `atx-impl/`, `scripts/research_store*.py`, `scripts/tests/test_research_store*`,
   `classes.json` and its guard (lanes SQL2, SQL3). Judge: the hook never changes an exit code or a JSON byte; one
   ingest per write; the catalog digest is reproducible; SQL2-CLS classes.
3. **NAV / book C++** (`wave-review-nav.md`): `atx-impl/src/book/`, `atx-impl/src/strategy_nav_v7.cpp`,
   `strategy_mv_aim.cpp`, `strategy_mv_target.cpp`, `strategy_nav_replay.cpp`, `atx-engine/src/book`,
   `atx-engine/include/atx/book*`, `atx-engine/tests/book`, `atx-impl/tests/` for those, `scripts/specs/p9/rules/`
   (lanes C3, AL-CLOCK, COV-MV, B2's NAV side, D3's rules). Judge: flag-absent byte identity claims backed by evidence;
   no hot-path allocation per decision; the registrations promise only figures the exes print.
4. **IC / composition C++** (`wave-review-ic.md`): the rest of `atx-engine/src/research/`,
   `atx-engine/include/atx/research/`, `atx-impl/src/strategy_ic*`, `atx-impl/src/research/`, `eval_report.cpp`,
   `atx-engine/tests/research/` not claimed by fields (lanes D3W, D3W-PB, ABS, SIGN, B2's eval verb, OD3P's C++ side).
   Judge: the C++ fit / composition / screen identities vs their Python predecessors (evidence, not claims); the
   deletion commits removed the Python mirror and its G-P5 row together; `/W4 /WX`.
5. **Python wave / cycle** (`wave-review-python.md`): `scripts/*.py` not claimed above, `scripts/tests/`,
   `atx-impl/tools/`, `atx-impl/strategies/`, `atx-engine/tools/` (lanes B2, C3b, D3, E2 follow-ups, OD3P, SQL3's
   call lines, SIGN's stage call). Judge: no new Python mirror of a C++ rule; the stage chain's contracts; the n37 test's
   1e-12 form (B2-N37); no look-ahead in the cycle (availability clocks).

Every reviewer: `r7-review-dispatch.md` (contract), `briefs/review-template.md` (form), `r8-review-notes.md` (traces),
the standing contract files, the area's lane reports / reviews in the pools (pool map: C3b 15, D3W / D3W-PB 23, ABS 28,
D3 20, OD3P 21, B2 14, SQL3 13, SIGN 29, A4 27, AL-CLOCK 30 (and 8 frozen), COV-MV 31 (and 24 frozen), T2 22, E2 17),
`root-wave3-merge-report.md` (root's checks per merge), the ledger's rulings for the lane ids. Blind: no 2020-2023
statistic in any record or message. Findings in the template's form: Required (I = incorrect, M = missing; each with
file:line, the failing input, the fix) and Suggested; the verdict for the area (APPROVE / CHANGES_REQUIRED). The file
goes into ROOT-SDD as `wave-review-<area>.md`, UNCOMMITTED (the gate root agent commits the five files with
`git add -f`); write it with the editor tool. Required findings are fixed by root as slips or returned to a lane
before any cell (GATE-1); a Required in a merged lane's code that no slip can fix is a gate finding, not a revert.

Hand-back under 200 words: STATUS, verdict, files reviewed (count), Required / Suggested (one line each), tests run
with counts, own mutation result, file path, any permission refusal (exact command), open questions.

## ROOT GATE (GATE-1; one root agent or a chain; the PM's prompt names the gate head and the next free tag)

`root-gate-brief.md` is the brief (steps 0-9); this section adds the facts of RESUME 11. Read also
`handoff-root-m6.md` (and `handoff-root-m6b.md` if SQL4 merged) "What the next root group must know" and "Exes
(EXE-CUR)", `r11-dispatch.md` "WAVE REVIEW", the five `wave-review-<area>.md` files (step 8 ran EARLY, WAVE-REV-EARLY;
you COMMIT them with `git add -f` at step 0 and act on their "What root must do before any cell" lists: a Required =
a root slip or a BLOCKED hand-back before any cell), the ledger from "- RESUME 11" to the end.

- Lanes in the head: waves 2 and 3 as `root-wave3-merge-report.md` lists them through M6 (B2 + fix 2, T2, C3b, D3W,
  D3W-PB, ABS, D3, OD3P + follow-up, AL-CLOCK first APPROVE + fix 1, COV-MV + fix 1, SIGN, A4 pre-flip `6dab6948`,
  SQL2 rows, SQL3 + item 5). NOT in the head (P10 whole; nothing of them in P9): A4's flips and fix-1 tests
  (A4-P10-FLIPS, A4-FIX1-P10: Python stays the producer of record for `ftd_shares_ratio21` and
  `regsho_threshold_days63`; Y-F0's fields manifest `26fee5ce` unchanged); SQL4 unless the prompt says it merged at M6b.
- Step 1 exes: the full set at the gate head, Debug (`equity-dev`) and Release (`equity-rel`, `-Canary`), 0 warnings,
  single-use tags from the prompt's next free tag; receipts per exe. The Debug `p9-3d` NAV exe and `p9-3b` IC exe are
  superseded by these builds. EXE-FREEZE from the re-pin (step 6).
- Step 2 suites: the known-red set is EMPTY (B2 fix 2 merged); the Release
  `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` named skip stays (OD5-2); any other red is attributed by
  merge commit (ROOT-1 rows of `root-wave3-merge-report.md`) and stops the gate for that lane (BLOCKED). Whole
  `scripts/tests` at seeds 0 and 1; `atx-engine/tools/test_stage_chain.py` alone; `atx-impl/tools`;
  `atx-impl/strategies`; each its own invocation; `-p no:cacheprovider`, own `--basetemp`.
- Step 3 identity: the parents' flag-absent identity with EVERY new flag absent (`mv-aim-v1`, `info-clock-rate-v1`,
  C3's grid, the absent-members capability, the sign verb's stage call, SQL3's hook off, SQL4's index flags if merged);
  AL-CLOCK's procedure (`task-AL-CLOCK-report.md` "Flag-absent identity procedure"); D3-Q1's engine-fit identity on the
  current Y-F0 TRAIN fit (`--engine-fit` weights byte-identical to the Python fit's: then P9-F0h is produced with
  `--engine-fit`; else P9-W lapses, PRE-35, and the difference is a finding); the gate greps (D3-Q2, D3-S1);
  COMPOSE-1's parse check of `mv-aim-v1` + `info-clock-rate-v1` together on the tiny world (a refusal lapses P9-MV,
  disclosed before any read); the golden `0x889874a3b9b29c55` at 1 and 4 workers; the canaries.
- Step 4 P9-B0: substitution list first (committed); R-2, Y-F0, Y-1 re-based on the Release exes (Y-1 on the Release
  v1.1 store `build-equity/p9-m2i-dcov-v11`, PRE-29); `--ann-mean-gross` on every reference; `--info-clock-print` on
  Y-F0 and its pair (AL-CLOCK is in); bounded runner; READ NO STATISTIC; the identity-bridge flag line on every print
  (OD5-1).
- Step 6 re-pin: `exes.eval` and every exe's SHA-256; the W1-I2 re-pin reads the registration SHA `7801c198...` of
  `info-clock-rate-v1.json`; COVMV-S2 S-b: iff `scripts/specs/p9/rules/mv-aim-v1.json` is not yet pinned by a ledger
  line, golden or prereg SHA, root edits its `printed_only` sentence in the re-pin commit (else PRE-40 form: left,
  stated); MV-FLAG-P0 is NOT here (it is the OD-3 P0 step).
- Step 7: the store step (M2A-4, SQL3-2: the catalog run writes to a root run dir, never `build-equity/research-store/`
  unless the brief's catalog step says so: then say so), the scoreboard, G-P ticks (G-P1 .. G-P9, G-P11), the G-P10
  print from the merged tree ((d) PARTIAL "three writers hooked, the stage record's side files not", (e) landed for
  `atx.run-store/v1`; the store reviewer's table in `wave-review-store.md`), the CRT record, rho of the P9 pairs (B2-13).
- Step 9 records: `root-gate-report.md`, `integration-log.md`, the round-2b-ii facts by Appendix IV item number (incl.
  item 16's registration SHA `7801c198...`, the A4-Q2 case as A4-P10-FLIPS, ALCLOCK-S2 S-A, COVMV-S2 S-b, SIGN-SUBST,
  D3WPB-Q2, the deferred-suite cells now run), `handoff-root-gate.md` (code head, docs head, the exe table, the
  reference dirs, known-red set, next free tag, what the cells agent must know). The ledger committed as it stands.
- Host: your builds have priority (HOST-4); foreign builds outside `C:\atx-wt\` ignored (HOST-5); ENV-1; the memory
  gate of `research-build.ps1`. If you run low on context: finish the current step to a committed state, write
  `handoff-root-gate-<n>.md` with "Exact next step", report DONE_WITH_CONCERNS; the PM chains a fresh root agent.

### ROOT GATE: rulings from the whole-wave review (RESUME 11, 2026-10-15 ~12:50; binding; full text in the ledger)

- Step 0 slips (one commit each, before step 1's builds; guard before / after; store suite; named tests): STORE-M1
  (`atx-impl/strategies` into the classes guard's `PY_DIRS`; every literal the extended guard finds registered in
  `classes.json` by the table's precedent, class stated; STORE-S S-1's catch-all unit case iff under 20 lines);
  IC-S (the four G-P5-invisible rule pairs into `FROZEN_PAIRS` iff that only pins their bytes and the guard stays
  green; else G-P5 PARTIAL in the report, P10 retirement). Commit the five `wave-review-<area>.md` files (`git add -f`).
- Step 1: GATE-EXES: the exe set includes `atx-research-composition` and `atx-research-admission` (Debug and
  Release, receipts, the re-pin table; EXE-FREEZE covers them).
- Step 2: GATE-COVDIFF: `cov-diff --legacy` on the three container stores `p9-m2i-dcov-v11`, `p9-m2i-dcov-covv1`,
  `p9-m2d-cov-risk-container` (exit 0 each, recorded), `v8-risk-lo3-v10`'s identity cited from the M2d record; plus
  a read-only hand check of each store's manifest `files` SHA-256 against the bytes on disk.
- Step 3: D3-Q1a: the engine-fit identity strips `provenance.engine_fit` from both weights files (canonical JSON, SHA-256
  of the stripped documents) and compares the two admission files whole; the tool and its output committed.
  COMPOSE-1's fake-store parse check of `mv-aim-v1` + `info-clock-rate-v1` together stays required (the NAV reviewer's
  parse probes found no flag conflict; the store-backed run is the gate's).
- Step 4 / cells: PRE-MVX: P9-X's parent is P9-F0h (one `--risk-model`, the v1.1 store, B.7); P9-V pairs with P9-X;
  P9-F = P9-X's book if accepted; stated in the round-2b-ii facts (B.7 "Parent", B.8, E). NAV-2STORE is P10.
- Step 6 re-pin: COVMV-S2 S-b + NAV-S S-2: iff `mv-aim-v1.json` is not yet pinned, root edits its `printed_only` /
  "ex-ante" sentence to name only what the exes print (else PRE-40 form: left, stated).
- Step 7: G-P10 (e) "landed" iff the extended guard is green; (d) PARTIAL as M6 states; the catalog record states the
  `classes.json` SHA by hand (STORE-S S-4).
- Fields area: APPROVE, nothing for root. Python area: as its file says (dispatched 12:10; read it at step 0).
