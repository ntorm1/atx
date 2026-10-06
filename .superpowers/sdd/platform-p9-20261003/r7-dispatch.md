# P9 dispatch R7-1: per-agent text (PM, 2026-10-05)

Every agent: read `resume4-dispatch.md`, `resume6-addendum.md`, (lanes) `r6-wave3-common.md`, `resume7-addendum.md`,
then your section below, then the files it names. Sprint dir = `.superpowers/sdd/platform-p9-20261003/` (root's copy
is in `C:/atx-wt/pool-2`; a lane's own records are in its pool's copy, `git add -f`). You resume a stopped lane: never
restart it, never redo what its handoff or the ledger marks done, never rewrite history (a `WIP(...)` commit stays;
you add commits). Lanes are blind: synthetic and fixture data only, no 2020-2023 statistic.

---

## ROOT: finish M2j (pool-2, `feat/platform-p9-20261003`; one root agent at a time)

Read: `root-wave2-merge-brief.md`, `handoff-root-m2j.md` (your state), the M2j section of
`root-wave2-merge-report.md` (pre-run lists, committed in `9c7aa654`), rulings M2J-1, M2J-2, M2J-3, W3-8a, COV-14,
C3-5, C3-6, C3-9, PRE-7a, EXE-CUR, SEAL-ALLOW-3, ENV-1, PERM-1, M2I-HK, SQL2-CLS.

0. Housekeeping first, one docs commit: the sprint-dir files as they stand (`progress.md`, `status-7.md`,
   `next-goal-prompt.md`, `resume7-addendum.md`, `r7-dispatch.md`). Never the PNGs under `docs/plans/`, never a cache
   dir. `progress.md` is the PM's: commit it as it stands whenever you make a docs commit, never edit its lines.
1. Then, in this order (M2J-1): item 4 (the `--ann-mean-gross` opt-in check), item 5 (Release = Debug NAV on X-5 and
   the wall times), the rest of item 1 (Release whole `atx-impl-strategy-target-tests`; whole `atx-impl-tests` on
   both builds), item 6 (SQL2-CLS: `atx.nav-spec/v1` into `classes.json` as the list says; guard before / after; the
   store suite under seeds 0 and 1), item 9 (ROOT-1: `scripts/tests` and the stage chain, separate invocations),
   item 8 last (the C3-5 probe). A duplicate or conflicting `--rule` on the NAV argv is a stop for the PM.
2. Item 7 is NOT yours any more (W3-8a: lane D3 flips `new-cost-model`); do not edit `scripts/tests/test_wave_kinds.py`.
3. M2I-HK: delete `build-equity/p9-m2i-cov-y1-nav` by its literal path. Keep `p9-m2j-x5-argv` until items 4 and 5 ran.
4. Exes p9-2p / p9-2q are current only if `git diff 327c86b2..HEAD` over C++ / CMake is empty (EXE-CUR); state it.
   Real-data runs through `scripts/run_bounded_research.py` (`--seconds` <= 600, `--max-rss-mib 2048`); compare by
   SHA and JSON path only; read no statistic.
5. No lane merge in this step group. Records: the M2j rows of `integration-log.md` and
   `root-wave2-merge-report.md`, the round-2b facts for the prereg (C3-6 result: Appendix IV item 5), and
   `handoff-root-m2j-fin.md` (code head, docs head, exes with tags and SHA-256, known-red set, next free build tag,
   what the next root agent must know). One docs commit at the end. Lanes are waiting to merge: hand back when done.

---

## B2: post-identity redo, follow-up 2 (pool-14, `feat/p9-b2-20261003`, head `14b881c9`)

Read: `handoff-B2.md` in pool-14 ("Follow-up at the owner stop" wins), `task-B2-report.md` ("Post-identity redo",
"B2-11 / R1 / R2"), `task-B2-rereview-1.md`, rulings B2-10 .. B2-14, PRE-15, D2R-2.

1. `git merge feat/platform-p9-20261003` (root moved: C3 part A is in). B2-14 binds every conflict: resolve each
   region by an ordinary edit of that region; no `git checkout --theirs / --ours`, no `git restore`, no
   `git show <rev>:<path> > <path>`, no copy over a file, no rewrite of a whole file from one side. List every region
   with the side kept and why. If an edit is refused: stop and report.
2. B2-11 with re-review R1 and R2, at the places the handoff names; rewrite `scripts/tests/test_wave_eval_verb.py`
   on a real v2 manifest; teeth by mutation (reverted).
3. Re-test on the merged tree: the G-P5 deletion (`test_no_python_mirror.py`,
   `test_nav_summ_engine.py::test_a_bin_without_the_verb_fails_closed`), PRE-15's tests.
4. D2R-2 (b) last (scan for abbreviated `nav_summ` flags first, then `allow_abbrev=False` and the e27b cases).
5. Builds Debug and Release (tags from `p9-b2-i`; EXE-CUR); gtests eval / ledger / `TrialLedger*`; `scripts/tests`
   under PYTHONHASHSEED 0 and 1; the stage chain; the tools suites; the mirror guard.
6. Report: finish "Post-identity redo" in `task-B2-report.md` (conflict tables of both merges, B2-R3's hunks and where
   each sits now, what root re-proves at merge incl. the TRAIN identity procedure); update `handoff-B2.md`.
   Three lanes wait on your merge (C3b, SQL3, OD3P): hand back when the suites are green; add no scope.

---

## D3: guards and kinds (pool-20, `feat/p9-d2-20261003`, head `f6726dd6`; CRITICAL PATH)

Read: `handoff-D3.md` in pool-20, `briefs/brief-D3.md`, `wave3-carry.md` (D3 section, W3-7, W3-8, "Added after
drafting"), rulings D3-1, D3-2, W3-8a, PRE-35, C3-3, E2-17, E2-8, P14, KINDS-TW.

Your scope under D3-2 is the kind files ONLY: `scripts/wave_kinds.py`, `scripts/wave_stage_kinds.py`,
`scripts/tests/test_wave_kinds.py`, `scripts/specs/p9/kinds/**`. The walk-forward C++, the v3 runner, the fitter,
ALCOMB-6 / -7, D2-9 and D2R-3 belong to lane D3W (pool-23): do not touch the fitter, `composition_*.py`,
`research/composition/**` or the IC runner files.

1. `git merge feat/platform-p9-20261003`.
2. Finish and test the WIP of `0389e868` (handoff rows 2, 3a, 3b): the C3-3 alias guard, the E2-17 `inputs.*` guard,
   the walk-forward kind out of SCHEMA_ONLY. The kind derives the fitter's flags under exactly these names (D3-2):
   `--walk-forward expanding --walk-forward-lag N --walk-forward-step N [--walk-forward-min-decisions N]`; registered
   values lag 2, step 21, min_decisions 21 (D3-1). Check the WIP's `WALK_FORWARD_FLAGS` against them. Tests as the
   handoff lists them, on fakes; teeth for every refusal (a working-tree mutation you revert).
3. W3-8a: flip `new-cost-model.json` out of `SCHEMA_ONLY_FIXTURES` too (C3 part A's cost-law rows are in your
   tree); the suite then states "10 runnable, 0 schema only" or what is true.
4. Handoff row 3c: the live-table check as a test, skipped unless `ATX_EQUITY_BIN` names a bin with both exes.
5. Optional: KINDS-TW (a real-exe kinds canary as a NEW fixture; `scripts/specs/tiny.json` is never edited).
6. Suites: `scripts/tests/test_wave_kinds.py`, then the whole `scripts/tests` under PYTHONHASHSEED 0 and 1, and
   `atx-engine/tools/test_stage_chain.py`, separate invocations. Python only: no build is expected.
7. Lane C3b (pool-15) makes small listed edits in your files (one `"pm6-6-engine"` line per kind fixture; three
   hunks in `test_wave_kinds.py`): leave room, do not pre-empt them; root resolves at merge.
8. Report `task-D3-report.md` section "Part A: guards and kinds" (each guard -> file:line -> test -> teeth log; the
   routes each guard closes; what root re-proves at merge incl. the live-table test on real exes; "For PRE": the
   walk-forward manifest shape and the template's path and SHA-256); update `handoff-D3.md`.
   No cell starts before this lane is merged and APPROVE (PRE-35): hand back as soon as it is green; add no scope.

---

## D3W: walk-forward C++, v3 runner, fitter (pool-23, NEW branch `feat/p9-d3w-20261005` at `899b5811`)

`899b5811` is lane D3's head before its WIP commit (root `9c7aa654` with C3 part A is merged in it). The lease's
one-time `dev` configure failed (known owner item): ignore `build/`; use `equity-dev` / `equity-rel`. Your pool has
no sprint-dir records yet: create `task-D3W-report.md` and `handoff-D3W.md` there (`git add -f`).

Read: `C:/atx-wt/pool-20/.superpowers/sdd/platform-p9-20261003/handoff-D3.md` ("Design decided": the fingerprint
constraint, item 1 engine / runner / fitter, item 4; table rows 1, 4, 5), `briefs/brief-D3.md`, `wave3-carry.md`
(brief-D3 superseded, D3 section, "Added after drafting"), rulings D3-1, D3-2, P19, ALCOMB-4 .. ALCOMB-7, D2-9,
D2R-3, D2-1, D2-7, PRE-35, PRE-39, E2-9.

Scope under D3-2 (files: the fitter, `composition_*.py`, `research/composition/**`, `strategy_ic_runner.cpp`,
`strategy_ic_composition.*`, `strategy_ic_admission.cpp`, `strategy_ic_detail.hpp`, `strategy_ic_rules.*`,
`strategy_ic_theme_tsmom.*`, `strategy_ic_mom_volman.cpp` for the one appended line; their tests). NOT yours: the
kind files (lane D3, pool-20), part B (the `ew-theme-aim-v2` C++ fit, resid diagnostics, the D2-3 deletion: a later
round), the absent-members flag of OD3P-1 (a later round; leave `IcComposition::add` easy to extend by a per-member
skip).

Order, with a commit after each: (1) `git merge feat/platform-p9-20261003`; (2) the C++ engine walk-forward fit and
its gtests; (3) the runner reading `atx.dsl-composition-weights/v3`, `WeightsV3.SingleRowEqualsV1Bytes`, the refusal
tests; (4) the fitter flags and post-step (names fixed by D3-2:
`--walk-forward expanding --walk-forward-lag N --walk-forward-step N [--walk-forward-min-decisions N]`; no edit
inside `fit` / `fit_prior` or their closure: show the fitter's `producer_fingerprint` equal before and after);
(5) ALCOMB-7, then ALCOMB-6; (6) D2-9 and D2R-3 (S1, S2). Registered constants: lag 2, step 21, min_decisions 21;
flat before the first row (D3-1). Flag absent = every byte unchanged; mom-volman, theme-tsmom and theme-cov-hedge
outputs byte-identical with walk-forward absent (ALCOMB-4 / -5).

Builds: `equity-dev` first (tags `p9-d3w-a`, ...), `equity-rel` once at the end; name the targets you build and the
anchored gtest filters. Pytest: the fitter / composition files, then the whole `scripts/tests` under seeds 0 and 1
and the stage chain, separate invocations. Report: items -> file:line -> test -> log; flag-absent identity procedure
for root; new literals / flags / keys; cross-lane edits; "For PRE": the P9-W block (fit mode, the per-date weights
producer, `composition_fit.json` `walk_forward` shape, what P9-M's sleeve line reads: keep or cut, ALCOMB-6).
This is the longest lane: if context runs low, WIP commit + handoff + DONE_WITH_CONCERNS.

---

## AL-CLOCK (pool-8, `feat/p9-alclock-20261004`, head `ebaf1e3f`)

Read: `handoff-AL-CLOCK.md` in pool-8 (the design), `briefs/brief-AL-CLOCK.md`, `wave3-carry.md` (brief-AL-CLOCK
superseded, AL-CLOCK section, W3-9, W3-16), plan section 2.4 lines 601-605, rulings ALCLOCK-1, ALCLOCK-2, DEC-16,
C3-1, C3-3, C3-4, C3-5, C3-9, PRE-7a, PRE-14, PRE-19, PRE-31, W1-I1, ALCOMB-1.

1. `git merge feat/platform-p9-20261003`.
2. Items 1-5 of the handoff as designed: the trade-rate row `info-clock-rate-v1` and one parser entry (append-only,
   W3-9); `two_speed_fast_theta(cadence)` (W3-16); the refusals; the per-book stats.
3. ALCLOCK-1: the `info_clock` block prints only under `--info-clock-print` (NavSpec `options.info_clock_print`);
   print-only, not a rule flag; with it only `summary.json` changes, by that block; it works on a book that does not
   run the rule (the reference). Tests: flag absent = bytes unchanged; flag on a non-rule book = only the block
   added; rule on without the flag = no block; `--spec` equals argv with the flag.
4. Tests of handoff item 3, incl. the 1-vs-4 `--book-workers` byte test with the rule on, and the E2 `rule_flags`
   preflight on fakes. State which token your row puts on the NAV argv (`--rate info-clock-rate-v1`) and how a wave
   of kind `rule` naming it resolves (root repeats the C3-5 probe at your merge).
5. Registration `scripts/specs/p9/rules/info-clock-rate-v1.json` (`source_sample_end` 2019; the `ea_days_since`
   mapping stated as a fixed choice; C3-4 line; DEC-16 argument) and template
   `scripts/specs/p9/templates/info-clock.json` in AL-COMB's shape (no `rules` key, no `change.flags`;
   `gross_match: "pm6-6-engine"`; id <= 18 chars).
6. Builds Debug then Release (`p9-alclock-a`, ...); named filters; whole target-tests; canary on both goldens.
7. Report `task-AL-CLOCK-report.md` with "For PRE": the P9-C block (rule, row, registration and template SHA-256,
   parent conditions, producers of each printed figure, the flag pair cell / reference). You merge BEFORE COV-MV.

---

## COV-MV (pool-24, `feat/p9-cov-20261003`, head `b8f714c9`)

Read: `handoff-COV-MV.md` in pool-24 (the findings and the chosen singular-safe form), `briefs/brief-COV-MV.md`,
`wave3-carry.md` (COV-MV section, W3-9, W3-10, W3-17, COV-11), `cov-design.md` section 5.2, rulings COV-4, COV-5,
COV-7, COV-9, COV-11, COV-13 (c), COV-14, PRE-9, PRE-10, PRE-14, PRE-24a, PRE-29, C3-1, C3-4, E2-8, ALCOMB-1, W1-I1.

1. `git merge feat/platform-p9-20261003`.
2. Items 1-8 of the handoff as designed: kernel `mv_target` (never invert F by itself; the rank-deficient-F test
   shows the target finite and equal to the dense solve), the `target` row `mv-aim-v1` with `risk_model` /
   `risk_model_sha256` params (append-only), the parser entry, the hook, the refusals (`incompatible[]`), the
   print-only `mv_aim` summary (priced count, the specific-vol ratio of PRE-24a, the eigen 00-02 share of
   COV-13 (c), the special-tier negative-weight count), COV-11's a = 2 control and the per-row group check.
3. Synthetic containers only. The lane never opens `build-equity/p9-m2i-dcov-covv1`; the registration names its
   manifest pin `ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3` (W3-10, PRE-29).
4. Registration `scripts/specs/p9/rules/mv-aim-v1.json` (K-P9-11 keys; `source_sample_end` from the primary source
   you verify, else "unverified", which is flagged under PRE-19) and template in AL-COMB's shape
   (`gross_match: "pm6-6-engine"`; id <= 18 chars).
5. AL-CLOCK (pool-8) appends a row and a parser entry in the same registry files and merges before you (W3-9).
   Before your report: if `git -C C:/atx-wt/pool-2 log --format=%s -30` shows AL-CLOCK merged, your root merge
   brings it (keep both rows); if not, say so and root resolves the keep-both append at your merge.
6. Builds Debug then Release (`p9-covmv-a`, ...); named filters; whole target-tests; `Cov*` suites; canary both goldens.
7. Report `task-COV-MV-report.md` with "For PRE": the P9-MV block (rule, row, SHA-256s, `source_sample_end`, the
   realised-gross statement of C3-4, the printed figures and their producers) and what the reviewer traces for COV-4.

---

## C3b (pool-15, `feat/p9-c3-20261004`, head `081e2090`)

Read: `handoff-C3.md` in pool-15 ("C3b at the owner stop": items and design notes (a)-(k)), `task-C3-report.md`,
rulings C2-9, C2-10, C2-11, C3-7, C3-10, C3-11, C3-12, E2-16a, D2R-2, PRE-27, PRE-40, W3-8a.

1. `git merge feat/platform-p9-20261003`.
2. Items 1-5 as designed: (b)-(e) retire the Python `pm6-6` route (refused at preflight / run / match naming
   `pm6-6-engine`; ledgered v1 manifests still validate; the ledgered files of (a) are never edited); (f) C2-9;
   (g) C2-10 as the first act of `wave_result.build` (C3-12); (h) the E2-16a test; (i) C3-7 `read_grid` duplicate
   keys and S-12. Teeth for every new refusal.
3. Item 7 is dropped (C3-11): do NOT delete `sign_pm7_35`; its G-P5 row stays; say so in the report.
4. PRE-40: do not edit `scripts/specs/p9/rules/mom-volman-v1.json` / `theme-cov-hedge-v1.json`.
5. Your edits in lane D3's files (the `"pm6-6-engine"` line of each kind fixture; the three hunks of
   `scripts/tests/test_wave_kinds.py`) are the smallest possible and listed by file and hunk: D3 (pool-20) is
   editing that test file now; root resolves at merge.
6. Item 6 (D2R-2 (a), text in `scripts/cycle/spec.py`) LAST and only if B2's redo is merged in root; else pending.
7. Optional: S-13, the C3-10 log line, E2-11.
8. Builds Debug then Release (`p9-c3-m`, ...) of `atx-impl-strategy-target-tests,atx-equity-strategy-targets`
   (`-Canary`); the report's named filter plus the new tests; whole target-tests; canary both goldens; the wave
   suites; whole `scripts/tests` under seeds 0 and 1; the stage chain.
9. Report: "## Part B (C3b)" in `task-C3-report.md` (root's evidence tools that still call the Python mode, listed
   for root; what root re-proves); update `handoff-C3.md`. C3b merges before any cell (E2-16).

---

## SQL3 (pool-13, `feat/p9-sql3-20261004`, head `2e0b79f6`)

Read: `handoff-SQL3.md` in pool-13 (writers, the `store` block, W3-4 / W3-5 designs, the test plan),
`briefs/brief-SQL3.md`, `wave3-carry.md` ("brief-SQL3.md" superseded, SQL3 section, W3-2 .. W3-5), `sql-design.md`,
rulings SQL-2, SQL-6, SQL-11, SQL-12, SQL2-CLS, SQL3-1, M2A-4, B2-12.

1. `git merge feat/platform-p9-20261003`.
2. Items 1-4 as designed (hooks outside B2's files, `scripts/research_wave.py`, the `store` block, W3-4
   `seal_guard.cpp` + gtest, W3-5 `book_monitor.py`). The hook never changes an exit code or a JSON byte; catalog
   off = no subprocess, bytes identical.
3. SQL3-1: list `atx.run-store/v1` and the `classes.json` rows you propose; do not edit `classes.json`; the classes
   guard is then an expected red in your tree (name it).
4. Item 5 (the three call lines in B2's files) LAST and only if B2's redo is merged in root; else pending.
5. Build `p9-sql3-a` (equity-dev; the targets the handoff names), gtests `ResearchCatalog*:SealGuard*:StoreCli*`;
   pytest as the handoff's plan lists, each group in its own invocation.
6. Report `task-SQL3-report.md` with "For root" (the tiny-world identity procedure; the real-tree catalog run is
   root's at the gate, M2A-4; G-P10 (a)-(e) each landed / partial / P10).

---

## OD3P (pool-21, `feat/p9-od3p-20261004`, head `80484f0b`)

Read: `handoff-OD3P.md` in pool-21, the prereg section F (`p9-prereg.md`, root's copy), rulings PRE-10, PRE-17,
PRE-18, PRE-20, PRE-21, PRE-31, OD3P-1, ALCLOCK-2, OD5-1, ALD-9.

1. `git merge feat/platform-p9-20261003`; re-run your two test files on the merged tree.
2. OD3P-1 (replaces the pinned-0 design of item 2): the carry tool no longer zeroes a dropped member; the carried
   weights are TRAIN's bytes; `fit.carry.drop {path, sha256}` makes the cycle pass
   `--absent-members <file> --absent-members-sha256 <hex>` to the IC exe on history roles only, and refuse (fail
   closed, naming capability `absent-members-v1`) when the IC exe does not offer it; theme-erc-v1 and ic-shrink bases
   are no longer refused for that reason. Keep the other refusals (unknown or repeated id, a base that applies a
   listed rule, no `--era`). Write the contract for the C++ lane as `od3p-absent-contract.md` in your sprint dir:
   list-file schema and keys, flag spelling, capability name, the defining equivalence (a listed member with data =
   the same run with that member's data removed), where the cycle puts the flags. Tests on fakes; teeth.
3. ALCLOCK-2: move `info-clock-rate-v1` from `rules_pending` to `rules` in `flagged_members.json`; `mv-aim-v1`
   stays pending (root resolves it from COV-MV's registration before the read).
4. Item 6: the runbook `p9-od3-runbook.md` (four history lines: P9-F0h, Y-F0, R-2, the clean-source book; G-B8 on
   E1+E2; the era fields argv rule; `holdout_gate.py` never; the identity-bridge flag on every print; history NAVs
   run through the cycle only, never a hand `--spec`; the clean-source line prints "not read: no producer" if the
   C++ capability is not merged; every value unknown until the cells have run is marked so).
5. Item 7: the whole `scripts/tests` under seeds 0 and 1, the stage chain, `test_no_python_mirror.py`.
6. Item 3 (T2: the one-sided pooled p on the verb, in `nav_summ`) LAST and only if B2's redo is merged in root.
7. Report `task-OD3P-report.md` ("For PRE": Appendix IV item 32; "For root": the identity procedure) and the handoff.

---

## A4 (stretch; pool-27, NEW branch `feat/p9-a4-20261005` at root `1b64ce45`; ruling A4-1)

Read: `briefs/brief-A4.md` ("Rules for every P9 lane" is superseded by `resume4-dispatch.md` + `r6-wave3-common.md`
where they differ: you DO build your own targets; pytest flags per the contract), `wave3-carry.md` ("brief-A4.md"
under "Brief text superseded", the A4 section, W3-13), `task-A3-report.md`, `task-A3-rereview-1.md`,
`C:/atx-wt/pool-12/.superpowers/sdd/platform-p9-20261003/handoff-A3.md` item 4 (A1-SHIM; read-only),
`task-AL-SIG-report.md` (the 13F reader, P18), plan `docs/plans/2026-10-02-platform-core-migration.md` section 4
slice 5 and `docs/plans/2026-10-02-platform-core-audit.md` section 2 rows for `research_fields_sec.py`,
`research_fields_holdings.py`, `research_fields_v9.py`; rulings A4-1, A3-SHIM, A3-FIX1, A3-3, P18, DEC-5, OD-A3-6F,
S-R2, W1-I1.

1. `git merge dfa4218d` (T2's APPROVE head: the re-dated SEC / holdings fixtures) first; then root's head whenever it
   moves. Identity runs on T2's merged fixtures.
2. Deliver per the brief and the carry: the clock primitives (check `asof_series.hpp` before adding a "Latest"
   clock; `fields/clock.hpp` is another meaning), the FTD / RegSHO stage readers in the AL-SIG 13F reader's style on
   A3's source interface (no second 13F decoder), builder kinds for the moved fields with registry rows, a
   planted-leak probe with teeth per builder, fixture byte identity vs the Python builders; A1-SHIM; S-R2; A3 review
   S-2 (reuse key); A3-3 (per-field re-hash under `--reuse`); S-6 boundary (`coskew_60m`, `xrd0_ttm` stay Python).
   Optional items of the carry as time allows.
3. Each `kind: engine` flip is its OWN commit at the END of your branch, after the pre-flip head is complete and
   tested (A3 precedent): root merges the pre-flip head, runs the TRAIN identity per moved field (payload SHA-256 =
   the current manifest's; seal counts from the engine receipts), then merges the flips. Name the pre-flip head and
   the flip commits in the report.
4. Builds Debug (`p9-a4-a`, ...) then Release; the fields test targets; `atx-engine-research-fields-tests`; the
   fixture pytests; whole `scripts/tests` under seeds 0 and 1 and the stage chain if you changed anything a Python
   test executes.
5. Report `task-A4-report.md` and `handoff-A4.md` in pool-27's sprint dir (create it; `git add -f`): per field the
   builder, the registry row, the probe, the fixture identity; the TRAIN identity procedure for root (argv per field).
   A4 is a stretch lane: it merges only if APPROVE at the wave-3 gate, else it is P10 whole; hand off cleanly at
   any point (WIP commit + handoff) rather than hold the gate.
