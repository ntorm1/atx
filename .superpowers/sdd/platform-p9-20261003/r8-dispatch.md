# P9 dispatch R8-1: per-agent text (PM, 2026-10-06)

Every agent: read `resume4-dispatch.md`, `resume6-addendum.md`, (lanes) `r6-wave3-common.md`, `resume7-addendum.md`,
`resume8-addendum.md`, then your section of `r7-dispatch.md` (still your task list), then your section below, then
your handoff. Sprint dir = `.superpowers/sdd/platform-p9-20261003/` (root's copy in `C:/atx-wt/pool-2`; a lane's own
records in its pool's copy, `git add -f`). You resume a stopped lane: never restart it, never redo what its handoff
or the ledger marks done, never rewrite history. Lanes are blind: synthetic and fixture data only.

---

## B2: post-identity redo, follow-up 2, finish (pool-14, `feat/p9-b2-20261003`, head `c3201a60`)

State: `task-B2-report.md` is MODIFIED and uncommitted (+134 lines: the R7 round's record, up to the marker
`<!-- EVIDENCE-R7 -->`); B2-11 (`d8f80c7a`), D2R-2 (b) (`34838462`) and the root merges `b1df1829`, `c3201a60`
are committed. The "Follow-up at the owner stop" section of `handoff-B2.md` is OLDER than that record: the report's
"Post-identity redo" section is the truth.

0. DIRTY-1: `git status --short`; `git add -f` the report and any new `b2-evidence/` files; commit
   `docs(b2): R7 round record at the owner stop`.
1. `git merge feat/platform-p9-20261003` (root `1b64ce45` is docs-only past `ebc918c3`: expect a clean merge; B2-14
   binds any conflict: edit the region, never checkout / restore / copy-over; list every region).
2. Continue the dispatch section (`r7-dispatch.md` "B2") from item 5: builds Debug and Release (next free tag: check
   `build-equity/mega-p9-b2-*-receipt.json`; EXE-CUR); gtests `EvalVerb*`, `*Ledger*`, `TrialLedger*` and the eval /
   ledger suites the report names; `scripts/tests` under PYTHONHASHSEED 0 and 1; `atx-engine/tools/test_stage_chain.py`;
   the tools suites (`atx-impl/tools/test_*.py` the handoff names); `test_no_python_mirror.py`;
   `test_nav_summ_engine.py::test_a_bin_without_the_verb_fails_closed` with the built verb; PRE-15's tests.
3. The refused mutation (teeth of B2-11) stays reported, not retried (PERM-1); the reviewer may try its own.
4. Report: fill the evidence after `<!-- EVIDENCE-R7 -->` (builds, counts, logs by path), "What root re-proves at
   merge" incl. the TRAIN identity procedure and the conflict tables of the three merges (`c37ec671`, `b1df1829`,
   `c3201a60`); update `handoff-B2.md` (a new top section; the old ones stay). Delete your object trees by name.
   Three lanes wait on your merge: hand back when the suites are green; add no scope.

---

## D3: guards and kinds, C3-13 (pool-20, `feat/p9-d2-20261003`, head `d080af09`; CRITICAL PATH)

State: `handoff-D3.md` is MODIFIED and uncommitted: a new top section "Resume 7 (lane D3 after ruling D3-2)" with
placeholders `@@STATE@@` and `@@HEAD@@`. `task-D3-report.md` "Part A: guards and kinds" is committed (`e531257b`);
the merge `d080af09` of root `1b64ce45` is the head; the suites after that merge may not have finished.

0. DIRTY-1: replace `@@STATE@@` with "suites to be re-run on d080af09 (owner stop)" and `@@HEAD@@` with "the commit
   adding this section, on `d080af09`"; `git add -f`; commit `docs(d3): handoff at the owner stop (resume 7)`.
1. `git merge feat/platform-p9-20261003` (expect no-op or clean).
2. Re-run `scripts/tests/test_wave_kinds.py`, then the whole `scripts/tests` under PYTHONHASHSEED 0 and 1, and
   `atx-engine/tools/test_stage_chain.py` (separate invocations) on this tree. Record the counts.
3. C3-13 (ruling C3-13, timing C3-13a): in `scripts/wave_kinds.py` / `scripts/wave_stage_kinds.py` (your files), a
   kind `rule` wave naming a NAV row whose `x-flag` is `--rule <id>` SETS the cell's `nav.rule` to `<id>` (replacing
   the parent's) and appends only the row's per-param `x-flag`s to `nav.flags`; any other row kind (`--rate`,
   `--target-rule`, `--cost-law`) keeps the append route. Tests on fakes: the derived NAV argv carries exactly one
   `--rule` for (a) the parent's own aim rule with other params (`aim-partial-v5`, `trade_fraction` .1: root's probe,
   evidence `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/m2j-logs/item8-probe.txt`, argv and exit codes
   only) and (b) another aim rule; E2's raw-rule-flag guard still accepts the result; the `--rate` / `--target-rule` /
   `--cost-law` fixtures unchanged. Teeth (mutation reverted byte-identically). Note where the parent's `nav.rule` is
   read (`scripts/cycle/phases.py:829` emits it; you do not edit the cycle).
4. Suites again (step 2). Report: a "C3-13" section in `task-D3-report.md` (change -> file:line -> test -> teeth log;
   what root re-proves: the C3-5 probe (a) and (b) on a fresh tiny world at your merge) and the counts of step 2;
   update the handoff top section. Python only: no build. C3b's listed edits in your files (fixture line 18; three
   hunks of `test_wave_kinds.py`) are left alone; root resolves. No cell starts before this lane is merged and
   APPROVE (PRE-35): hand back as soon as it is green; add no scope.

---

## D3W: walk-forward C++, v3 runner, fitter, ALCOMB, D2-9, D2R-3 (pool-23, `feat/p9-d3w-20261005`, head `23e650e6`)

State per `handoff-D3W.md`: items 2 (engine fit + gtests, `200cbf74`) and 3 (runner v3, `994210f4`) committed and
BUILT (`p9-d3w-a`, Debug, source `994210f4`, tree kept) but NO test has run; item 4 (fitter flags, `check_walk_forward`)
is WIP `2209821e`; items 5-8 not started. Rulings: D3-1, D3-2, D3W-EVAL, ABS-1, D3W-PB, ALCOMB-4 .. -7, D2-9, D2R-3,
P19, PRE-35, PRE-38, PRE-39, E2-9, OD3P-1.

1. `git merge feat/platform-p9-20261003` (root `1b64ce45`: `classes.json` + docs since `a15ad887`). Then EXE-CUR on the
   kept `p9-d3w-a` binaries: `git diff --name-only 994210f4..HEAD -- '*.cpp' '*.hpp' '*.h' 'CMakeLists.txt' '*.cmake'`
   must be empty for the targets built, else rebuild first (`p9-d3w-b`).
2. Run the anchored gtests (`WalkForward.*:ResearchComposition*`, then `WeightsV3.*:CompositionWeightRows.*`), then
   each whole exe; fix reds (next tag). Commit.
3. Finish item 4 (`composition_engine_fit.py` from `check_fit`, `fit_composition_weights.py` `main()`, pytest, the
   `producer_fingerprint` equal before / after; names fixed by D3-2); then 5 (ALCOMB-7, then ALCOMB-6: the IC exe
   writes the sleeve's daily series; the eval-verb half per D3W-EVAL as a listed cross-lane append in
   `atx-research-eval`), then 6 (D2-9, D2R-3 S1 / S2); a commit after each. Flag absent = every byte unchanged
   (ALCOMB-4 / -5: mom-volman, theme-tsmom, theme-cov-hedge outputs byte-identical).
4. ABS-1: do NOT implement the absent-members flags; leave `IcComposition::add` easy to extend by a per-member skip
   and name the place (file:line) in the report under "For ABS".
5. Release build once at the end (`equity-rel`; configure first); pytest: the fitter / composition files, then the
   whole `scripts/tests` under seeds 0 and 1 and the stage chain, separate invocations. Then merge root's then-current
   head again and re-run the suites.
6. Report `task-D3W-report.md` (create; `git add -f`): items -> file:line -> test -> log; the flag-absent identity
   procedure for root; new literals / flags / keys; cross-lane edits (incl. `strategy_ic_mom_volman.cpp:147`,
   `test_research_spec.py`, the eval verb); "For PRE": the P9-W block (fit mode, the per-date weights producer,
   `composition_fit.json` `walk_forward` shape, what P9-M's sleeve line reads, ALCOMB-6); "For ABS"; "For part B"
   (what D3W-PB starts from). Update `handoff-D3W.md`; delete `build-equity*` object trees by name (keep receipts).
   Longest lane: on low context, WIP commit + handoff + DONE_WITH_CONCERNS.

---

## C3b: finish (pool-15, `feat/p9-c3-20261004`, head `e28d7849`)

State per `handoff-C3.md` "C3b at the second owner stop": items 1-5 DONE (code `278fc2d8`), Debug `p9-c3-n` built and
kept (2.7 GB), named 34 / 34, whole 391 / 391, canary 14 / 1 wall-time miss with matching digests, Python teeth 6 / 6,
`scripts/tests` seed 0 522 / 4 / 0. Not run: seed 1, stage chain, C++ teeth, Release, the last root merge.

Do "Next step for a fresh agent" 1-8 of that handoff as written (root `1b64ce45`; item 6 iff B2's redo is in root at
that moment, else pending; optional S-13 before the builds; C++ teeth `p9-c3-o`; clean `p9-c3-p` `-Canary`; Release
`p9-c3-q` with the stale-PCH recipe; the canary wall-time rule is C3-14: one re-run on a quieter host, the limit is
never edited; seeds 0 and 1 and the stage chain on the merged tree). Report "## Part B (C3b)" in `task-C3-report.md`
(items, cross-lane hunks in D3's files by file and hunk, root's two evidence tools that still call the Python mode,
what root re-proves), update the handoff, delete the object trees by name at the end. C3b merges before any cell.

---

## AL-CLOCK: compile, test, register (pool-8, `feat/p9-alclock-20261004`, head `120f6a25`)

State per `handoff-AL-CLOCK.md`: 16 C++ files (+352 / -32) WRITTEN BUT NEVER COMPILED (`WIP(al-clock)` on the merge
`8a6ebe5b` of root `4dc831ac`); `build-equity` configured (`equity-dev`), nothing built, `p9-alclock-a` free; no test,
no pytest, no registration, no template. Rulings: ALCLOCK-1, ALCLOCK-2, ALCLOCK-3 (the loosened refusal:
`--capacity-curve` accepted with `info-clock-rate-v1`; the registration states it; a test shows the loosening touches
no other rate), W3-9, W3-16, C3-5, PRE-7a.

Do "Next step for a fresh agent" 1-4 of that handoff: merge root (`1b64ce45`+; C3 part A is already in your tree; the
merge brings `classes.json` and docs), compile `p9-alclock-a` and fix slips, the C++ test file + CMake tail, the
`rule_flags` pytest, the registration and the template (as "Decided, not yet written" lists them), the tests of
`r7-dispatch.md` items 3-4 (flag absent = bytes unchanged; the flag on a non-rule book adds only the block; rule on
without the flag = no block; `--spec` equals argv with the flag; 1-vs-4 `--book-workers`; E2 `rule_flags` preflight on
fakes), whole target-tests, teeth, Release, canary on both goldens, `scripts/tests` seeds 0 / 1 + the stage chain if
Python changed. Report `task-AL-CLOCK-report.md` with "For PRE" (the P9-C block incl. registration / template SHA-256,
`source_sample_end` 2019 flagged, the `ea_days_since` mapping, the NAV argv token `--rate info-clock-rate-v1` and how a
kind `rule` wave naming it resolves). You merge BEFORE COV-MV (W3-9). Delete object trees by name at the end.

---

## COV-MV: implement the fixed design (pool-24, `feat/p9-cov-20261003`, head `190eebe2`)

State per `handoff-COV-MV.md` "Session R7": NO code; the design for items 1-8 is fixed by file:line (kernel
`mv_target`, impl rule `strategy_mv_aim.{hpp,cpp}`, glue, hook, refusals, summary block, workspace term, gtests,
teeth plan, registration / template shape). Rulings: COV-14, COVMV-1 (the two out-of-list edits are allowed: the new
impl file with its line at the END of `atx-impl/CMakeLists.txt`; ONE added term in `nav_workspace_reserve_bytes`
with a flag-absent test), COV-4, COV-5, COV-7, COV-9, COV-11, COV-13 (c), PRE-9, PRE-10, PRE-14, PRE-19, PRE-24a,
PRE-29, C3-1, C3-4, W3-9, W3-10, W3-17.

1. `git merge feat/platform-p9-20261003`. Fresh configure (`equity-dev`). Implement items 1-8 as designed, a commit
   per item group; tags `p9-covmv-a`, ...; Release once at the end. Synthetic containers only; the lane never opens
   `build-equity/p9-m2i-dcov-covv1`; the registration names its manifest pin
   `ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3`.
2. Registration `scripts/specs/p9/rules/mv-aim-v1.json` (K-P9-11 keys; `source_sample_end` from a primary source you
   verify from cov-design section 8's citations, else "unverified" (flagged under PRE-19 either way if 2015+)) and
   template `scripts/specs/p9/templates/mv-aim.json` in AL-COMB's shape (`gross_match: "pm6-6-engine"`; id <= 18 chars).
3. AL-CLOCK (pool-8) appends a row and a parser entry in the same registry files and merges before you (W3-9):
   before your report, if `git -C C:/atx-wt/pool-2 log --format=%s -30` shows AL-CLOCK merged, merge root (keep both
   rows); if not, say so and root resolves the keep-both append.
4. Tests: `MvTarget.*`, the impl rule gtests incl. refusals with teeth (one extra build with every new refusal
   disabled, revert), `Cov*` suites, whole target-tests, canary both goldens, the flag-absent identity (bytes
   unchanged without the rule; the workspace reserve unchanged without the flag), `scripts/tests` seeds 0 / 1 and the
   stage chain if Python changed.
5. Report `task-COV-MV-report.md` with "For PRE" (the P9-MV block: rule, row, registration / template SHA-256,
   `source_sample_end`, C3-4 realised-gross statement, the printed figures and their producers) and what the reviewer
   traces for COV-4. Delete object trees by name at the end. Long lane: on low context, WIP commit + handoff.

---

## SQL3: close the round (pool-13, `feat/p9-sql3-20261004`, head `784d2abd`; short)

State: items 1-4 DONE and committed; `handoff-SQL3.md` is MODIFIED and uncommitted (rewritten for the R7 round; its
seed-1 line reads "SEE-SEED-1"); `task-SQL3-report.md` committed; item 5 pending on B2; object tree deleted.

0. DIRTY-1: `git add -f` the handoff; commit `docs(sql3): handoff at the owner stop (R7 round)`.
1. `git merge feat/platform-p9-20261003` (expect no-op). SQL3-SEED: run the whole `scripts/tests` under
   PYTHONHASHSEED 1 (and the store suite under seed 1 if the report lacks it); replace "SEE-SEED-1" in the handoff
   (and any such placeholder in the report) with the counts; the classes guard red naming only `atx.run-store/v1`
   is expected (SQL3-1). No build. Commit; hand back. Item 5 stays pending until B2 is in root (a later short round).

---

## OD3P: close the round (pool-21, `feat/p9-od3p-20261004`, head `e9bedcc7`; short)

State: items 1-5, 7 DONE and committed (`9be598cf`, `05e2a373`, suites on `e9bedcc7`); `handoff-OD3P.md` is MODIFIED
and uncommitted (rewritten for the R7 round); T2 (item 6) pending on B2.

0. DIRTY-1: `git status --short`; `git add -f` the handoff and any uncommitted `od3p-evidence/` log; commit
   `docs(od3p): handoff at the owner stop (R7 round)`.
1. `git merge feat/platform-p9-20261003` (expect no-op); if it is not a no-op, re-run your two lane test files.
   Confirm `od3p-absent-contract.md` and `p9-od3-runbook.md` are committed. Hand back. T2 stays pending.

---

## PRE-2b-i: the rulings into the prereg text (doc agent; pool-2 sprint dir; no pool; commits ONLY `p9-prereg.md`)

Ruling PRE-2B-SPLIT. You are blind: no 2020-2023 return / IC / Sharpe / turnover / NAV statistic is read; the only
numbers you copy are the v8 ledger's ACCEPTED PUBLIC lines the prereg itself cites for G-B7 (items 27-30 of
Appendix IV: R-2, X-2, X-3, X-5 step values; find the lines by the prereg's own citations, section E and A.2). Read
`p9-prereg.md` whole, `p9-prereg-flags.md` "P9 rules" and "Open for the PM", and by grep in `progress.md` the rulings
PRE-31 (item 10), PRE-32 .. PRE-40, COV-14, PRE-40, ALCLOCK-1, ALCLOCK-2, ALCLOCK-3, OD3P-1, D3-1, D3-2, GATE-1, C3-11,
C3-12, C3-13, SQL3-1, A4-1, W3-8a, OD5-1 .. OD5-7, EXE-FREEZE, PRE-29, PRE-30 (never the whole ledger).

1. Apply each ruling at the place the prereg's "Open for the PM (round 2a)" items and Appendix IV point to (B.6, B.7,
   B.8, C.11, E, F.1, A.4, D): PRE-32 (P9-X is no v2 `leverage` wave), PRE-33 (P9-V in a degenerate pair: PRE-12
   governs; the printed line), PRE-34 (what prints for P9-X and G-B2), PRE-35 as read by D3-2 (cells wait for lane
   D3's guards; P9-W lapses iff D3W's walk-forward or D3's kind wiring are not merged and APPROVE then), PRE-36 (G-B7
   steps), PRE-37 (PRE-27 on P9-X / P9-V), PRE-38 (producer of the realised vol ratio = the NAV exe's `ann_vol`; the
   sleeve line keeps the eval verb), PRE-39 (P9-W's parent is P9-F0h), COV-14 (P9-V's predictions: all-ranks pooled
   row + per-rank table; "dominated by ranks 00-01; 39 of 51 ranks nearer 1"), PRE-40 (the stale `manifest_note`
   statement in B.2 / B.3), ALCLOCK-1 / -2 / -3 and OD3P-1 (section F and B.4 wording), PRE-31 item 10 (a flagged rule
   is treated like a flagged member in the "without flagged members" line), GATE-1 (one gate; C / G wording), D3-1
   (walk-forward constants lag 2, step 21, min_decisions 21; flat before the first row; P9-W's dSR includes it).
2. Replace the section "Open for the PM (round 2a)" by "Rulings applied in round 2b-i" (item -> ruling -> where in
   the text), and extend section H.
3. Relabel every remaining placeholder "ROUND 2b-ii: <who delivers it>" (Appendix IV's own index tells you who);
   change the header line "ROUND 2a - not ruled" to "ROUND 2b-i (rulings applied; values pending 2b-ii)".
4. Edit nothing else; never touch a registered SHA-256; do not touch any other file. Commit:
   `git -C C:/atx-wt/pool-2 add -f .superpowers/sdd/platform-p9-20261003/p9-prereg.md` and commit ONLY that path
   (`docs(p9): prereg round 2b-i: rulings PRE-32..40, COV-14 and the RESUME 7 rulings applied`), trailer as the
   contract. Final message: the list of placeholders still open with their owner.

---

## DATA-1: classify `C:\atx\data` (read-only agent; no pool; writes one file in pool-2's sprint dir; no commit)

Owner instruction (ledger line "OWNER INSTRUCTION ... C:\atx\data"): delete what atx-engine and atx-impl do not
currently use. YOU DELETE NOTHING. You classify the 187 entries of `C:/atx/data` (14 GB, git-ignored) and write
`C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/data-classification.md`. Hard rules: nothing in `C:/atx` is
built, switched, committed, written or deleted; `C:/atx/atx-db` is never opened (grep the pool-2 copy
`C:/atx-wt/pool-2/atx-db` read-only instead for references to `atx/data` or entry names); open no data payload; read
receipt / README / manifest / `*.json` metadata files only in entries whose names carry no date at or after
2024-01-01 (an entry named `..._2026...` as a DATA range, e.g. `tickerhistory_20260501_20260615_20260919`, is never
opened; a trailing `_2026MMDD` build stamp on an entry whose data range is 2013-2019 is a build date, not a data
date: metadata may be read). Never run `atx-impl/tools/holdout_gate.py`.

Method: `Get-ChildItem C:/atx/data` with size (du) and mtime per entry; for each entry name (and its stem without the
build stamp) grep the tracked tree of `C:/atx-wt/pool-2` (`git -C C:/atx-wt/pool-2 grep -n -- <name>` over
`atx-engine`, `atx-impl`, `atx-core`, `atx-tsdb`, `scripts`, `docs`, `README*`, `CMake*`, `*.json` specs) and the
pool-2 `atx-db` copy; note whether a hit is a live input (a spec path, a default constant, a test fixture path read
at run time, a tool argument), a doc / README mention, a review receipt / hash pin, or a bench. Start from the
ledger line's partial findings (keep: `finra_short_interest`, `finra_short_volume`; unclassified: the
`tickerhistory_training*` / `tickerhistory_native*` families and `equity_universe_pit_2013_2019_20260920`; likely
unused: `equity_scorecard16..21_*`, `l7_riskmodel_*`, `equity_mine_l9*`, `equity_fund_*`, `asof_smoke_ctx_*`,
`equity_*_training_2013_*`, `equity_identity19_*`, `equity_profile17dbg_*`, `equity_source_reconciliation_*`,
`tickerhistory_replay*`). Also check whether the OD-3 era roles (2015-2019 history) or any `scripts/specs/**` path
names an entry, and whether any receipt under `atx-engine/reviews/` pins an entry by hash (a pinned entry is
"reference", class B).

Classes, one table row per entry (name, size, mtime, class, evidence as file:line or "no hit", for class D: what
regenerates it (tool + receipt) or which later entry supersedes it): (A) used by current atx-engine / atx-impl code,
tests, specs or tools; (B) vendor / source / history-block / reference data (pinned by a receipt or a hash);
(C) possibly read by an atx-db job (any hit in the atx-db copy, or an atx-db-shaped name); (D) unused AND
(regenerable by a named tool with a receipt in the entry or in the repo, OR superseded by a later entry of the same
stem that class A uses); (E) the 2026-dated entry (never opened); (U) unclear. Totals per class in GB. The PM decides;
a later agent deletes class D only. Final message: counts per class, GB per class, the five largest D entries, any
permission refusal (exact command).

---

## ROOT (pool-2; one root agent at a time; dispatched by the PM at the first APPROVE)

Per `r7-root-merge.md`, starting from `handoff-root-m2j-fin.md`. Step 0 of the first root agent of this resume: one
housekeeping docs commit of the sprint-dir files as they stand: `progress.md`, `next-goal-prompt.md`,
`r7-dispatch.md` (modified), and `git add -f` for the untracked `status-8.md`, `r7-review-dispatch.md`,
`r7-root-merge.md`, `resume8-addendum.md`, `r8-dispatch.md`, and `data-classification.md` if present; NEVER the PNGs
under `docs/plans/`, never a cache dir; if `p9-prereg.md` is modified and uncommitted it is the PRE-2b-i agent's
(do not add it; it commits its own). Then the merges the PM's prompt names, in `r7-root-merge.md`'s order, with the
C3-5 probe (a) and (b) at D3's merge (C3-13) and with `--rate info-clock-rate-v1` at AL-CLOCK's merge, SQL2-CLS for
every new literal (`atx.run-store/v1` from SQL3; `atx.carry-weights/v1`, `atx.od3-flagged-members/v1` from OD3P;
whatever B2, AL-CLOCK, COV-MV, D3W list), and ROOT-1 at every merge. Records: `root-wave3-merge-report.md` (created
at the first wave-3 merge; B2 and T2 go in `root-wave2-merge-report.md`), `integration-log.md`, the round-2b-ii facts
for the prereg, `handoff-root-<group>.md`.
