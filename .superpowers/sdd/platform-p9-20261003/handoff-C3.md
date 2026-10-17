# Handoff C3 (lane C3, pool-15, branch `feat/p9-c3-20261004`, base `e1c16051`)

Contract: `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/resume4-dispatch.md`, `briefs/brief-C3.md`,
`wave3-carry.md` (C3 section, PM answers, "Added after drafting"). The report is `task-C3-report.md` (same dir).

## State

- Part A (C++): DONE; review `99b676f7` CHANGES_REQUIRED; **fix round 1 DONE_WITH_CONCERNS** (`9a7c490f` code +
  tests, then the docs commit; the round was stopped after build j and resumed by a fresh agent, no code changed on
  resume): R-1 (duplicate JSON keys refused), R-2 (narrower `leverage_range` refused with `construction_grid`), C3-3
  (`x-flag-aliases ["--cost-v2"]` on `cost-sqrt-v2` only), PRE-7 / PRE-7a (print-only `--ann-mean-gross`, NavSpec
  key `ann_mean_gross`), S-1, S-3 (as a recorded deviation, C3-6), S-4, S-5, S-7, part of S-6. Not done: the rest of
  S-6 (other lanes' comments, "on next touch"). Debug l and Release j: named 33 / 33, whole target-tests 390 / 390,
  canary 15 each; teeth k: six mutations, each caught by its own test. Concern: the explicit before / after exe
  comparison (pre-round vs post-round NAV, flag absent) was NOT run: the worktree switch to `99b676f7` it needed was
  refused by the permission system (report "Permission refusals"). Next: a scoped re-review of the fix round, then
  root's merge (report "Fix round 1 / Root at merge", summary below).
- Part B (C2-11 Python pm6-6 retirement, C2-9, C2-10, W3-6): PENDING, not started: it waits for E2 in root (C3-2)
  (root head `ab189471` = S2R + B2 identity part, merged into this branch at `9dace914`; no newer root merged).
- Commits: `4e958185` wip(c3) functional work; `7e4c9dc4` move-only split (alone); `9dace914` merge root
  `ab189471` (CMake tail conflict: B2's ledger link -> atx-impl-pipeline); then the CMake guard commit
  (`atx-impl-core` INTERFACE takes no sources), then the report commit.
- Build trees: `build-equity` (equity-dev), `build-equity-rel` (equity-rel). Both are regenerable: delete by name
  after the report (keep `build-equity/*receipt*`, `*-build.log`, `*-canary.log`, `c3-*.log`).

## Part B (next round, on a root head that contains E2's merge)

1. `git merge <root head>` (never rebase). Read root's handoff "Concern: C2 item (ii) is C3's"
   (`handoff-root-m2e2.md`) and ruling C2-11 (progress.md).
2. C2-11: retire `scripts/wave_rules.py` `matched_leverage` / `gross_matches` and their users:
   `wave_stage_cell.match` (`gross_match: pm6-6` path and `none`), `FakeCycle.calibrate` (`wave_fixture.py`), the
   fixture default manifest `"gross_match": "pm6-6"` -> `"pm6-6-engine"`, the direct asserts in
   `test_research_wave.py`, replace the engine-vs-Python test by a pinned-fixture test of the engine route, retire the
   G-P5 row in `test_no_python_mirror.py`. Line numbers move with E2: re-locate by symbol.
3. C2-9: a manifest that needs gross matching and does not name `gross_match` is refused at plan, the message naming
   the key and the value `pm6-6-engine`.
4. C2-10: digest re-check of the cell of record immediately before `wave_result.build`.
5. W3-6: list the PM7-35 copy (`wave_rules.py:50-51`) for root; delete only after root's identity vs B1's predicate.
6. Pytest: explicit paths, `scripts/tests` and `atx-engine/tools` in separate invocations, `-p no:cacheprovider`,
   own `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`.

## Build tags used

- p9-c3-a (Debug): target-tests, targets exe. 577 s, 328 TUs (cold tree).
- p9-c3-b (Debug, -Canary): + IC exe, after the split. Canary 15 passed.
- p9-c3-c (Debug, -Canary): merged head; + atx-research-eval, the five research exes, atx-impl. Canary 15 passed.
- p9-c3-d (Release, -Canary): refused by the stale-PCH trap (ccache PCH vs the fresh `deps/equity-rel`).
- p9-c3-e (Release, -Canary, `CCACHE_RECACHE=1` after deleting the six `cmake_pch.cxx.{pch,obj}` by name):
  776 s, 299 TUs; target-tests 387/387; canary 15 passed.
- Fix round 1: p9-c3-f (Debug, fresh tree, 359 TUs, 443 s; canary 15; one red in a new test of mine, fixed),
  p9-c3-g (Debug: named 33/33, whole target-tests 390/390, canary 15), p9-c3-h (Release: the stale-PCH trap again,
  nothing tested), p9-c3-i (refused at memory admission, nothing built, no receipt), p9-c3-j (Release at clean
  `9a7c490f` after deleting the six `cmake_pch.cxx.{pch,obj}` by name, `CCACHE_RECACHE=1`: 274 TUs, 1082 s, canary
  15; named 33 / 33, whole 390 / 390 run on resume), p9-c3-k (Debug, test target only, six teeth mutations: 5 named
  tests fail as intended, 28 pass; reverted), p9-c3-l (Debug at `9a7c490f`, code clean: 12 TUs, canary 15, named
  33 / 33, whole 390 / 390).
- Next free tag: p9-c3-m. Build object trees: see "Disk" (a resumed round configures afresh; expect the stale-PCH
  trap on the first Release build: same remedy).

## Logs

`c3-logs/` here: gtest-a-named (18/18), gtest-a-whole, gtest-b-whole, gtest-c-whole (387/387 each), canary logs.
Fix round 1: `fix1-gtest-f-named.log` (32/33, the red fixed), `fix1-gtest-g-named.log` (33/33),
`fix1-gtest-g-whole.log` (390/390), `fix1-gtest-j-named.log` (33/33), `fix1-gtest-j-whole.log` (390/390, Release),
`fix1-gtest-l-named.log` (33/33), `fix1-gtest-l-whole.log` (390/390, Debug), `fix1-teeth-k-named.log` (5 intended
FAILs), `fix1-nav-identity.py` (tiny-world NAV driver) + `fix1-nav-optin-l.log` (opt-in tree check, PASS), canary
logs `mega-p9-c3-{f,g,j,l}-canary.log`. Receipts and build logs stay in `build-equity/` (`mega-p9-c3-*`).

## Disk

After the docs commit `5911a63e` the lane deleted by name the object trees of `build-equity` (CMakeFiles, atx-core,
atx-engine, atx-impl, atx-tsdb, bin, lib, tests and the tree files) and of `build-equity-rel` (same names), about
4.5 GB; kept: `build-equity/mega-p9-c3-*` receipts / build / cache-stats / canary logs, `c3-build-*.out`,
`c3-configure-*.log`, both `vcpkg-manifest-install.log`, and the two dirs that are not the lane's
(`build-equity/audits/`, `build-equity/v8-interim3-pitch-render-run2/`). Scratch `%TEMP%/c3-navid-l` deleted;
`%TEMP%/c3-navid-dry` (12 MB) is left: its deletion was part of the refused command.

## Root at merge (full list: report "Fix round 1 / Root at merge")

1. Scoped re-review of fix round 1, then merge; both presets, the nine targets of the report's item 1, `-Canary`;
   named filter 33 / 33, whole target-tests 390, `atx-impl-tests` once, canary 15 on the unchanged goldens.
2. X-5 and Y-F0: `nav <argv> --print-spec` (byte-preserving redirect, never PowerShell 5.1 `>`), then the argv run and
   the `nav --spec` run into two new dirs: every file byte-identical.
3. Release = Debug NAV on X-5 (G-P3 NAV half): equal except `summary.json` `producer` (`engine_git_sha`,
   `build_type`).
4. Flag-absent identity against the pre-C3 reference (without `--ann-mean-gross`): equal except
   `producer.engine_git_sha` (check the fields manifests for a `seal` block: without one stdout gains the P13 legacy
   line). The tiny-world before / after the lane could not run: `c3-logs/fix1-nav-identity.py old --bin <pre-merge
   bin>` then `new --bin <merged bin>`, `RESULT PASS`.
5. Wall time: the five research exes' build time on a fresh tag vs the pre-C3 receipt (F-10); X-5 NAV wall time
   before / after the 14-TU split, Debug and Release.
6. SQL2-CLS: `atx.nav-spec/v1` into `atx-engine/schemas/research_store/classes.json` (the only new schema literal of
   C3; this round adds none).
7. W3-8 kind-fixture flip (root post-merge, D3 owns the kind files): `new-cost-model.json` out of
   `SCHEMA_ONLY_FIXTURES`; run the 10 tiny-world kinds against the real `nav --list-rules --json`; D3's E2-8 guard
   reads `x-flag-aliases` (`--cost-v2` = `--cost-law cost-sqrt-v2`, ruling C3-3).
8. The opt-in (PRE-7a): flag `--ann-mean-gross`, NavSpec key `options.ann_mean_gross`; every P9 cell and its
   reference name it alike at the wave-3 P9-B0; with it only `summary.json` changes, by one `ann_mean_gross` key per
   scenario (a grid manifest's `summary_file_sha256` follows). New optional keys: scenario `ann_mean_gross` in
   `atx.dsl-nav-replay-summary/v1`, row `x-flag-aliases` in `atx.nav-rules/v1`.
9. `atx-impl-core` is INTERFACE: retarget any root list / append naming it; W1-I2 re-pin of lineage parents at P9-B0.

## Open questions

- The explicit before / after exe check (flag absent, pre-round vs post-round) is root's at merge (item 4) unless the
  owner allows the refused worktree switch (report "Permission refusals").
- Part B is pending by the dispatch rule, not by a question.

## C3b at the owner stop (2026-10-04)

- **Head `65ea7477`** = merge of root `5c57c1ef` (`feat/platform-p9-20261003`: E2's split, C2's R4 lines as E2 placed
  them, D2R, COVR) into part A's approved head `0dbfa459`. The merge was clean (no conflict). Its commit message is
  git's default (no trailer); not amended (no history rewrite). Then this handoff commit. Tree clean.
- **No code changed, no build, no test run** in C3b. The round stopped in the reading / design phase. Build trees:
  none exist in pool-15 (part A deleted them); only `build-equity/mega-p9-c3-*` receipts and logs are kept.
  Next free build tag: `p9-c3-m`.
- Root at the stop: head `5c57c1ef`; no B2 redo merge and no C3 part A merge yet.

### Per item of the C3b dispatch

| item | state | what the next agent does (design as found) |
|---|---|---|
| 1 C2-11 retire Python `pm6-6` | not started (designed) | see "Design notes" (a)-(e) |
| 2 C2-9 plan-time refusal | not started (designed) | (f) |
| 3 C2-10 digest re-check before `wave_result.build` | not started (designed) | (g) |
| 4 E2-16a `ref.leverage` test | not started (designed) | (h) |
| 5 C3-7 `read_grid` duplicate key + S-12 | not started (designed) | (i) |
| 6 D2R-2 (a) stale text | not started; waits for B2's merge in root | (j) |
| 7 W3-6 PM7-35 Python copy | not started; **design gap found** | (k) |
| optional S-13, C3-10, E2-11 | not started | S-13: one comment line in `atx-impl/src/book/nav_spec.hpp:30-34` |

### Design notes (from the reading; file:line at `65ea7477`)

(a) Scan of every committed file naming the Python mode (`git grep -I pm6-6`, `pm6-6-engine` excluded):
- Ledgered v1 wave manifests, never edited: `scripts/specs/v8/waves/y-2.json:25`, `y-3.json:25`, `y-5.json:25`,
  `y-s.json:23`, `y-s.head.json:11`; their results `.superpowers/sdd/platform-v8-20260929/waves/y-*/wave-result.json`.
  The cells they ledgered (the `-gm` specs) are plain cycle specs with no `gross_match`: `research_cycle run` replays
  them unchanged. `test_wave_kinds.py:203-207` (`test_v1_manifests_keep_their_validation`, D3's file) pins that these
  manifests still VALIDATE, so the refusal belongs at preflight, not at load (plan / status of the old state dirs keep
  working; a run is refused).
- Pinned by the prereg (sha in `p9-prereg.md:685-686`), never edited: `scripts/specs/p9/rules/mom-volman-v1.json:81` and
  `theme-cov-hedge-v1.json:75`: `manifest_note` text says "gross_match pm6-6" (stale text; the prereg itself names
  `pm6-6-engine` for every cell, :200, :237, :253, :266). Report for the PM: a manifest that follows the note literally
  is refused at preflight naming `pm6-6-engine`.
- v2 kind fixtures, unpinned (no sha reference anywhere), not ledgered, D3's files (W3-8): `scripts/specs/p9/kinds/`
  `new-combination-rule.json`, `new-cost-model.json`, `new-field.json`, `new-horizon.json`, `new-signal.json`,
  `new-universe.json`, `replaced-signal.json`, `walk-forward.json` (line 18 each) -> `"pm6-6-engine"`: a one-line
  cross-lane edit per file (list it). D3's pool-20 had not touched them at the stop.
- Root's evidence tools that call the Python mode (not edited, listed for root): `m2e-logs/tools/c2_gm_pair.py:157-159`
  (`WR.gross_matches`), `m2h-logs/m2h2-engine-o1.py:125` (variant "py"): after C3b only the engine variants run.
(b) `scripts/wave_rules.py`: `GROSS_MATCH` (:109) loses `"pm6-6"`; delete `matched_leverage` / `gross_matches`
  (:113-122); add a retired-name map `{"pm6-6": ...}` that `validate_names` (:177) still accepts (v1 validation
  unchanged) and that preflight / run / match refuse with a message naming `pm6-6-engine` (C2-11) and, for a ledgered
  v1 manifest, "replay from a copy that names pm6-6-engine (C2-9)". Module doc :15-18 updated.
(c) `scripts/wave_stage_cell.py`: `match` (:229-269) keeps only mode `"none"` (read `mech-calibration`, return the same
  uncorrected dict as today: byte-identical receipts) and the engine route; a retired mode raises `StageError(..., 3)`
  in `run_stage` (:60) and `match` (a state dir whose preflight receipt predates the retirement); `match_plan`
  (:406-421) and `run_plan` print the refusal for a retired mode; docstring :40-44. Preflight check in
  `scripts/wave_stage_preflight.py:159-201` (`problems.append`, so the E2-14 R3a probe at `test_wave_kinds.py:677`
  that sets `pm6-6` still finds its own needle among the problems).
(d) Tests: `scripts/tests/wave_fixture.py:180` default -> `"pm6-6-engine"`; `FakeCycle.calibrate` (:343) computes the
  fake exe's L' inline (`f"{lev * target / g1:.4f}"`, a test oracle: tests are outside the G-P5 scan). Then run the
  wave suites and update every assertion that encoded the Python route (expected: `test_research_wave.py:77-84`
  (delete), `:179-229` (cell spec becomes `-gm` / `run_spec` `-cal`, runs list `--stop-after w`), `:236-288` (replace by
  a pinned-fixture test of the engine route, no Python comparison), and whatever `test_wave_driver.py`,
  `test_wave_review*.py`, `test_wave_speed.py`, `test_wave_hardening.py`, `test_wave_queue.py`,
  `test_wave_scoreboard.py` assert about the two-NAV route). In D3's `test_wave_kinds.py` (cross-lane, smallest
  edits): `:732` parametrize -> `["pm6-6-engine"]` and drop the dead branch `:759-761`; `:606-614` read
  `record_spec` (`-gm`) instead of `cell_spec`. G-P5: delete the row `scripts/tests/test_no_python_mirror.py:75-76`
  (B2's redo deletes rows :44-55 of the same file: separate hunks).
(e) New tests on REAL v2 manifests (load a `scripts/specs/p9/kinds/*.json` fixture and re-root it as
  `test_wave_kinds.world` does; put them in a C3 file, e.g. `scripts/tests/test_wave_gross_match.py`, not in D3's):
  `pm6-6` refused at preflight naming `pm6-6-engine` (exit 3, no register receipt); a ledgered v1 manifest still
  validates; P9-C's shape (`test_the_match_stage_still_writes_its_own_leverage[pm6-6-engine]`) stays green; PRE-27:
  wave 1 (engine, accepted) -> its `next_parent` (`-gm`, plain at L') named as a v2 wave's parent passes preflight and
  calibrates at L'; naming the `-cal` recipe is refused (R3a). Teeth for each new refusal (mutation reverted).
(f) C2-9: `gross_match` stays in `TOP_REQUIRED` (`scripts/wave_manifest.py:120`); when absent, `validate` emits one
  dedicated message naming the key and `"pm6-6-engine"` (`"none"` for a v2 kind `leverage`, which must name none,
  `wave_kinds.py:198`) instead of the generic missing-key line; at load = at `plan` (exit 2). Outside the kind files.
(g) C2-10: the call site `scripts/wave_stage_record.py:262` is B2's file (B2's hunks are in `verify` / imports,
  not `record`). Option chosen to avoid it: the re-check as the first act of `wave_result.build` (`scripts/
  wave_result.py:35-50`, after `cell` is formed): recompute `w.spec_digest` of `cell.spec` and (engine) `cell.run_spec`
  and refuse (`StageError`, exit 3, lazy import from `wave_stage_util` as `helper_rows` does) on a mismatch; no
  wave-result is written. Test: a FakeCycle subclass whose judge call (`research_cycle run <-cal>` without
  `--stop-after`) commits an edit of `-gm.json` (the S14 window is inside one chain run: on a later run verify's own
  inputs already catch it). If the PM wants the call literally in `record()`, it is a 3-line move after B2 merges.
(h) E2-16a: fixture `new-combination-rule.json` + the template's own `change.set ref` block (re-pinned, as
  `test_wave_kinds.py:824-830` does), gross mismatched, run to verify: `RC.load_spec` of `record_spec` and of the
  `-cal` run recipe both give `ref.leverage == "1.1474"` (pre-calibration) while the record's `nav.leverage == L'`.
(i) C3-7: `atx-impl/src/book/nav_run.cpp:127` parses with `Json::parse(text, nullptr, false)`. Move the callback of
  `parse_spec` (`book/nav_spec.cpp:320-340`) into one public function in `book/nav_spec.hpp` (e.g.
  `parse_unique_keys(text, duplicate&)`), call it from both; `read_grid` refuses `nav grid: duplicate key "K" in PATH`
  after the existing size / read checks (the grid sha is computed from the same bytes). Test (raw text, like
  `NavSpec.RefusesADuplicateKey` :339): a grid file with `"--aim-leverage"` twice in one variant's flags, and `variants`
  twice, refused (exit 2, no output dir); the same file without the repeat runs. S-12: in
  `NavSpec.SpecRunPublishesTheFlagRunBytes` (:404) use a fresh stream and assert it holds
  `"nav spec: atx.nav-spec/v1 " + file + " sha256 " + co::sha256_hex(bytes_of(file))` (`atx/core/sha256.hpp:56`).
  Builds: Debug and Release of `atx-impl-strategy-target-tests,atx-equity-strategy-targets` (`-Canary`), named filter
  of the report + the new test, whole target-tests, canary on both goldens.
(j) D2R-2 (a): the text is now `scripts/cycle/spec.py:240-244` (`argparse_values` doc), `:264-269` (`validate_e27b`
  doc), `:280-283` (refusal message). B2's redo re-places hunks in `scripts/cycle/spec.py` (`effective_exes`,
  `EVAL_EXE`) and adds nav_summ `allow_abbrev=False` (D2R-2 (b)), after which all three statements are stale for both
  tools. Do it only after B2's merge is in root (`git -C C:/atx-wt/pool-2 log --format=%s -20`), text only.
(k) W3-6 design gap (stop condition for this item only): deleting `sign_pm7_35` (`scripts/wave_rules.py:35-57`, G-P5
  row `test_no_python_mirror.py:77-78`) leaves the library wave's screen stage (`wave_stage_library.py:118`
  `screen_decision`) with no sign rule. B1's predicate (`atx-engine/include/atx/engine/research/admission/
  sign_rule.hpp:92` `pm7_35`) is reachable only inside `atx-research-admission screen`'s manifest
  (`admission_cli.cpp:201-213`: `admitted_sign_stands` for admitted strings only, no replacement rule), which needs a
  factor-series dir the wave does not produce. A deletion commit as written would refuse every library wave
  (`sign_rule 'pm7-35' is not one of`). Needs a PM ruling on the route first, e.g. a verb on an exe the wave already
  pins (the NAV exe: `pm7-35 --rows FILE` applying `adm::pm7_35(kPm735SignRule, ...)`, header-only) with its receipt,
  then the deletion. Identity procedure for root, once a route exists: on the screen rows of every ledgered wave
  (Y-2, Y-3, Y-5, Y-S `wave-result.json` `screen.rows`) and on the full decision table of
  `research_admission_sign_test.cpp:46-75`, the C++ route's keep / drop equals `WR.sign_pm7_35` row for row; only then
  root merges the deletion commit.

### Next step for a fresh agent (superseded: see "C3b at the second owner stop" below)

Resume on `65ea7477` (no re-merge needed unless root moved): implement (b)-(f) and (h), run the wave suites and fix the
Python-route assertions, then (g); C++ (i) with a Debug build `p9-c3-m` while the Python suites run, Release after;
`scripts/tests` whole under PYTHONHASHSEED 0 and 1 and `atx-engine/tools/test_stage_chain.py` separately; (j) only if
B2 is in root; report (k) as the open design question. Then merge root's then-current head, re-run, write "## Part B
(C3b)" in `task-C3-report.md`.

## C3b at the second owner stop (2026-10-05)

Stopped at a clean point on the owner's stop (relayed by the PM). Nothing was in flight: the last suite (whole
`scripts/tests`, seed 0) had finished. No process of this lane is running.

- **Branch head:** the docs commit after `278fc2d8` (code). Commits this round: `d1629b01` merge root `4dc831ac`;
  `938812e1` merge root `ebc918c3` (both clean, no conflict; git's default message, not amended); `278fc2d8`
  `feat(c3b)` (all code and tests of items 1-5); then this docs commit (handoff, `c3-logs/c3b-*`).
- **Root merged:** `ebc918c3` (code head, the SQL2-CLS commit). Root has since moved to `1b64ce45` (docs only, per its
  subject; NOT merged). B2's redo is not in root (`git -C C:/atx-wt/pool-2 log --format=%s -30`): item 6 pending.
- **Tree:** clean. Build tree `build-equity` (Debug, configured at `938812e1`, built at `278fc2d8`, 2.7 GB) is KEPT on
  purpose: the next agent's teeth build of item 5 is incremental on it. Regenerable: delete by name when the lane is
  done (keep `build-equity/mega-p9-c3-*` receipts / logs, `c3-*` files, `audits/`, `v8-interim3-pitch-render-run2/`).
  No Release tree exists. Disk freed this round: 0.

### Per item (file:line at `278fc2d8`)

| item | state | change | test | evidence |
|---|---|---|---|---|
| 1 C2-11 retire the Python `pm6-6` | DONE | `scripts/wave_rules.py:109-129` (GROSS_MATCH = none / pm6-6-engine; `RETIRED_GROSS_MATCH`, `retired_gross_match`; `matched_leverage` / `gross_matches` deleted), `:175-187` (`validate_names` accepts the retired name: ledgered v1 manifests validate); `scripts/wave_stage_preflight.py:169-171` (preflight refusal, exit 3); `scripts/wave_stage_cell.py:48-60` (`refuse_retired`, `retired_plan`), `:78` (run), `:252` (match), `:258-273` (`match_none`: the receipt layout of before; fail-closed for a mode without a stage), `:112`, `:411` (plan prints the refusal); G-P5 row deleted (`scripts/tests/test_no_python_mirror.py`, old :75-76; `FROZEN_PAIRS` untouched, so the names stay detected); fixture default `scripts/tests/wave_fixture.py:179`; `FakeCycle.calibrate` computes L' inline (`:343`, a test oracle) | `scripts/tests/test_wave_gross_match.py:63` (v1 manifests validate), `:81` (preflight, a real v2 fixture and a v1 wave; plan / status still exit 0), `:107` (run / match stages of an older state dir), `:127` (fail-closed); the engine-vs-Python test replaced by `test_research_wave.py::test_engine_gross_match_on_the_pinned_fixture` (L' 1.1782 pinned) | teeth P1-P4 `c3-logs/c3b-teeth-py.log` |
| 2 C2-9 no default | DONE | `scripts/wave_manifest.py:260-265`, `:338-347` (`gross_match_missing`: one message naming `"pm6-6-engine"`, or `"none"` for a v2 kind leverage; absent or null; plan exit 2) | `test_wave_gross_match.py:140` | teeth P5 |
| 3 C2-10 / C3-12 digest re-check | DONE | `scripts/wave_result.py:54` (first act once the cell is formed), `:95-117` (`cell_digest_problems`, `refuse_cell_digests`: spec and engine run_spec; StageError exit 3; nothing written) | `test_wave_gross_match.py:191` (control, cell of record, run recipe, gross_match none) | teeth P6 |
| 4 E2-16a test | DONE (test only) | none | `test_wave_gross_match.py:213` (`-gm` and `-cal` resolve `ref.leverage` 1.1474; record `nav.leverage` = L'; verify passes); PRE-27 test `:250` (a v2 wave on wave 1's `-gm` calibrates at L' 1.1782; on its `-cal` refused at preflight, E2-14) | seed-0 run |
| 5 C3-7 `read_grid` duplicate key + S-12 | code + Debug tests DONE; C++ teeth and Release NOT done | `atx-impl/src/book/nav_spec.hpp:90-95` (`parse_unique_keys`), `book/nav_spec.cpp:317-326` (`parse_spec` uses it; messages unchanged), `:344-361` (definition; the trial_epoch callback moved here), `book/nav_run.cpp:5`, `:128-136` (refusal `nav grid: duplicate key "K" in PATH ...`, InvalidArgument) | `atx-impl/tests/strategy_nav_spec_test.cpp:385` `NavSpec.GridFileRefusesADuplicateKey` (a flag twice in a variant, `variants` twice: exit 1, key and file named, no output dir; the unrepeated file runs); S-12 `:446-455` (first console line = `nav spec: atx.nav-spec/v1 <file> sha256 <SHA-256 of its bytes>`) | `c3-logs/c3b-gtest-n-named.log`, `c3b-gtest-n-whole.log` |
| 6 D2R-2 (a) stale text | PENDING (B2 not in root) | text at `scripts/cycle/spec.py:240-244`, `:264-269`, `:280-283` (design (j)) | | |
| 7 W3-6 | DROPPED (C3-11) | `sign_pm7_35` kept; its G-P5 row kept (`test_no_python_mirror.py`, the `("sign_pm7_35",)` row) | | |
| opt S-13 | NOT done | one comment in `book/nav_spec.hpp:30-34`: name `--spo-gross G` (hard cap on planned gross, default `--aim-leverage`, in (0, 1.5 x L]; `strategy_nav_v7.cpp:1025-1026`, `:1297-1303`, verified) beside vol-target-v1 / risk-target-v1 | | |
| opt C3-10 log line | NOT done | not a one-line change: `load_nav_deploy` (`book/nav_deploy.cpp:40`) and `verified_load` have no stream | | |
| opt E2-11 | NOT done | no P9 SPO cell | | |

Exit-code deviation from design (i): a duplicate key in the grid file exits **1**, not 2: `read_grid` refusals are
printed and return 1 at `book/nav_cli.cpp:219-220`, like the schema / duplicate-id refusals of
`ConstructionGrid.RefusesBadGridsBeforeAnyOutput`; the test asserts 1.

### Cross-lane edits (lane D3's files; smallest possible; root resolves at merge)

- `scripts/specs/p9/kinds/{new-combination-rule,new-cost-model,new-field,new-horizon,new-signal,new-universe,
  replaced-signal,walk-forward}.json`: line 18 `"gross_match": "pm6-6"` -> `"pm6-6-engine"` (one line each; D3's own
  walk-forward hunk is line 5, separate).
- `scripts/tests/test_wave_kinds.py`, three hunks: `@@ -609` (`test_leverage_wave_cap_and_table_and_the_matched_l_prime`
  reads `record_spec`, 2 lines); `@@ -729` (parametrize `["pm6-6-engine"]`, 1 line); `@@ -756` (the dead `pm6-6`
  branch deleted, 3 lines). Left for D3 / root (docstrings / comments only, not edited, to keep the hunks small):
  `:543` "pm6-6 as well", `:563` 'with gross_match "none" or pm6-6', `:584` "a v2 horizon cell at pm6-6", `:734` "in
  both modes", `:737` "Python pm6-6: the -gm copy".
- Other wave tests rewritten for the engine route (not D3's): `test_research_wave.py`, `test_wave_driver.py` (W1-I2;
  reader reuse with keep_verdicts: the judge's cycle dir is `cycle-<cell>-gm`; timings), `test_wave_review.py`,
  `test_wave_review_minor.py` (two tests name `gross_match "none"`: their subject is the run stage's NAV being the
  cell's), `test_wave_scoreboard.py` (lineage `-gm`), `test_wave_speed.py` (one NAV, 41.5 s).
- Not edited (PRE-40): `scripts/specs/p9/rules/mom-volman-v1.json:81`, `theme-cov-hedge-v1.json:75`. The two P9
  templates (`scripts/specs/p9/templates/*.json`) carry no `gross_match` (a manifest key): root's P9-M / P9-H manifests
  must name `pm6-6-engine` (C2-9 refuses an absent key, C2-11 refuses `pm6-6`).
- Root's evidence tools that still call the Python mode (not edited; they now fail on the missing functions):
  `m2e-logs/tools/c2_gm_pair.py:156-159` (`WR.matched_leverage`, `WR.gross_matches`), `m2h-logs/m2h2-engine-o1.py:125`
  (variant `"py"`: run only the engine variants).

### Builds and tests actually run

| what | result | log |
|---|---|---|
| tag `p9-c3-m` (Debug, `-Canary`) | refused at the argument check (`-Canary` needs the IC exe and atx-research-eval in the tag): nothing built, no receipt; counted used | `build-equity/c3-build-m.out` |
| tag `p9-c3-n` (Debug, `-Canary`; atx-impl-strategy-target-tests, atx-equity-strategy-targets, atx-equity-strategy-ic, atx-research-eval) at clean `278fc2d8` | build exit 0, 359 TUs, 661 s; canary 14 passed, **1 failed on wall time only** (`first run took 17.5 s (limit 15.0 s)`, ATX_E2E_MAX_SECONDS; the golden-digest assertion just before it passed) on a host running about nine agents | `build-equity/mega-p9-c3-n-*`, `c3-logs/mega-p9-c3-n-canary.log` |
| named filter (part A's 33 + `NavSpec.GridFileRefusesADuplicateKey`) on n | 34 / 34 | `c3-logs/c3b-gtest-n-named.log` |
| whole `atx-impl-strategy-target-tests` on n | 391 / 391, 1 disabled (COVR's `DISABLED_`), ENV-1 hits 0 | `c3-logs/c3b-gtest-n-whole.log` |
| Python teeth P1-P6 (each refusal removed alone, its test run, file restored, SHA-256 checked) | 6 / 6 caught, each by its own test (a FAIL, not an error) | `c3-logs/c3b-teeth-py.{py,log}` |
| wave suites + the new file (before the commit) | 85 passed (the 18 Python-route reds of the first run all rewritten) | scratch run, not logged |
| whole `scripts/tests`, PYTHONHASHSEED 0, head `278fc2d8` | 522 passed, 4 skipped (e2e without ATX_EQUITY_BIN; three RESEARCH_CYCLE_LIVE_ROOT), 0 failed, ENV-1 0 | `c3-logs/c3b-pyt-n-scripts-s0.log` |
| NOT run (stopped) | `scripts/tests` seed 1; `atx-engine/tools/test_stage_chain.py`; the canary re-run on a quiet host; C++ teeth; Release build; final root merge and re-run | |

Next free build tag: `p9-c3-o`.

### Next step for a fresh agent

1. `git merge feat/platform-p9-20261003` (root `1b64ce45` or later; never rebase). If B2's redo is in root
   (`git -C C:/atx-wt/pool-2 log --format=%s -30`), do item 6 (text only, design (j)); else leave it pending.
2. Optional S-13 (the comment above), before the builds.
3. C++ teeth, tag `p9-c3-o` (Debug, `-Targets atx-impl-strategy-target-tests` only, on the kept tree): in
   `book/nav_run.cpp` mutate `if (!doc.is_discarded() && !duplicate.empty())` to `if (false && ...)`; expect
   `NavSpec.GridFileRefusesADuplicateKey` to FAIL (both files run at the last value) and the other 33 named tests to
   pass; restore the file byte for byte; then `p9-c3-p` (Debug, the four canary targets, `-Canary`) at the clean head.
4. Release `p9-c3-q` (configure `powershell scripts\atx-build.ps1 configure -Preset equity-rel`; `-Preset equity-rel`,
   the four targets, `-Canary`; on the stale-PCH trap delete that tree's six `cmake_pch.cxx.{pch,obj}` by name and
   rebuild with `CCACHE_RECACHE=1`): named 34, whole 391, canary on the Release goldens.
5. A canary wall-time miss (`first run took ... (limit 15.0 s)`) with the digest assertion passing is host load:
   re-run the receipt's canary command once on a quieter host and report both runs.
6. Whole `scripts/tests` under PYTHONHASHSEED 0 and 1, and `atx-engine/tools/test_stage_chain.py` (separate
   invocations; `c3-logs/c3b-pyt.sh`), on the merged tree.
7. Write "## Part B (C3b)" in `task-C3-report.md` from this section: items, cross-lane hunks, root's evidence tools,
   and what root re-proves (the wave suites and `test_wave_gross_match.py` on its merged tree; named 34 / whole 391 on
   both builds; the canary on both goldens; D3's merge resolving the three `test_wave_kinds.py` hunks and the eight
   fixture lines). Flag-absent identity: a grid or NavSpec file without a repeated key parses to the same document (the
   callback keeps every value), so no NAV byte moves; gross_match none keeps its receipt layout (its match-plan line now
   reads "the run stage's NAV is the cell's (no correction)"); no new schema literal, flag or JSON key.
8. At the end, delete the `build-equity` object trees by name (keep receipts and logs).

### Open questions

- The PM may want the canary's 15 s first-run budget (ATX_E2E_MAX_SECONDS) relaxed on a loaded host; this lane did not
  change it.
- Scratch of this round (pytest basetemps, synthetic only) is under the session scratchpad `.../scratchpad/c3b/`;
  delete by name.

## R8 round (reconstructed from logs)

Written in RESUME 9 by the C3b agent from `git log 278fc2d8..3129ca14`, `c3-logs/` and the receipts
`build-equity/mega-p9-c3-{o,p,q}-receipt.json` (copies in `c3-logs/`). The R8 agent left no handoff or report text
in the sprint dir; nothing below is counted done without a log or a receipt. Times are local (EDT), 2026-10-06 unless
noted. Found in R9 when the lane's old scratch was deleted: R8 had drafted a handoff addition and a Part B under its
session scratchpad (`.../e45465ef-.../scratchpad/c3b/handoff-add.md`, `partb.md`; never in the sprint dir, so never
committed). Its handoff draft, read before deletion as a draft (not as evidence), agrees with this table: the teeth
mutation was at `book/nav_run.cpp:132` and the file was restored byte for byte (SHA-256 `501cb0c1...`); the Release
canary re-run was not done; seed 1 at `3129ca14` was still running (`@@S1R@@` placeholder). Both drafts were deleted
with that scratch.
DIRTY-1 (R9, first act): the two exit tables `c3-logs/c3b-gt-exits.txt` / `c3b-pyt-exits.txt` committed as they stood
(`7a487eb8`); the other R8 logs below were untracked (git-ignored) and are committed with this section.

| step (handoff "Next step" 1-8) | evidence | result |
|---|---|---|
| 1 merge root | `7c038124` (18:08, parents `e28d7849` + root `1b64ce45`, docs only, clean); `3129ca14` (19:31, parents `f41719c2` + root `fd084e78` = SQL3 merge + `atx.run-store/v1` class row, clean) | done; item 6 not done (B2's redo not in root) |
| 2 optional S-13 | `f41719c2` (18:10): `atx-impl/src/book/nav_spec.hpp` comment, +4 / -3 | done |
| 3a C++ teeth, tag `p9-c3-o` | receipt o: Source `f41719c2`, DirtyEntries 1 (the mutation; the exit tables were still clean at 18:15), equity-dev, target-tests only, 23 TUs (every `book/` TU: S-13 touched `nav_spec.hpp`), 138 s, exe `a144625c...`; `c3-logs/c3b-teeth-o-named.log` (18:16): 34 ran, 33 passed, 1 FAILED = `NavSpec.GridFileRefusesADuplicateKey` (exit 0 where 1 expected, no `nav grid: duplicate key` message, output dir created: both repeated-key files ran) | caught 1 / 1 by its own test. The mutation text was not logged; the failure signature is the handoff's planned `if (false && ...)` in `book/nav_run.cpp`. Restore (before TEETH-RESTORE existed): build p recompiled `book/nav_run.cpp` and nothing else of the lane, p's DirtyEntries 1 is the exit table appended at 18:16, p's named run has the test passing, and at STOP 8 `nav_run.cpp` equals HEAD's blob (SHA-256 checked in R9: `501cb0c1...` both) |
| 3b clean Debug `p9-c3-p` `-Canary` | receipt p: Source `f41719c2`, DirtyEntries 1 (`c3b-gt-exits.txt`), equity-dev, the four canary targets, 1 lane TU + relinks (spdlog rebuilt in the shared cache, SPDLOG-RELINK), 57 s; canary block ExitCode 0, log `mega-p9-c3-p-canary.log` 15 passed; `c3b-gtest-p-named.log` 34 / 34 and `c3b-gtest-p-whole.log` 391 / 391 (1 disabled, COVR's), exe `3bd62115...`, ENV-1 0 | done |
| 4 Release `p9-c3-q` | `build-equity/c3-configure-q-rel.log` (18:59, fresh `build-equity-rel`); receipt q: Source `f41719c2`, DirtyEntries 2 (the two exit tables), equity-rel, the four targets, 359 TUs, 297 s, no stale-PCH trap; canary ExitCode 0 (`mega-p9-c3-q-canary.log` 15 passed); `c3b-gtest-q-rel-named.log` 34 / 34, `c3b-gtest-q-rel-whole.log` 391 / 391 (1 disabled), exe `74ad03cf...`, ENV-1 0 | done |
| canary at the merged head `3129ca14` (same p / q exes) | `c3b-pyt-r-canary-p-debug.log` 15 passed; `c3b-pyt-r-canary-q-release.log` 14 passed, 1 failed: `first run took 15.3 s (limit 15.0 s)`, the golden-digest assertion just before it passed | Debug done; Release = one wall-time miss with matching digests (C3-14): the one re-run on a quieter host NOT done in R8 |
| 6 suites at `f41719c2` (pre-SQL3 merge) | `c3b-pyt-q-scripts-s0.log` 522 passed / 4 skipped / 0 failed; `c3b-pyt-q-chain-s0.log` 9 passed; `c3b-pyt-q-scripts-s1.log` 522 / 4 / 0; ENV-1 0 each | done, superseded by the merged-tree runs |
| 6 suites at the merged head `3129ca14` | `c3b-pyt-r-chain-s0.log` 10 passed; `c3b-pyt-r-scripts-s0.log` 529 passed / 5 skipped / 0 failed (SQL3's tests in; the fifth skip is SQL3's real-exe hook case without an exe), ENV-1 0; `c3b-pyt-r-scripts-s1.log` (started 20:22, ran 21 h 05 min to 2026-10-07 17:28): 187 failed / 342 passed / 5 skipped, 528 hits of exit 3221225794 (164 `git init -q` children, 11 `HARD-STOP [fields]` children; 118 of 122 failure sections carry the code; the other four are host-memory-gate tests of the queue / launcher and one nested pytest) | seed 0 and chain done; seed 1 VOID under ENV-1 (re-run once in R9) |
| 7 report "## Part B (C3b)" | `task-C3-report.md` has no such section | NOT done |
| 8 delete object trees | `build-equity` (2.7 GB) and `build-equity-rel` (1.7 GB) present | NOT done |

EXE-CUR at `3129ca14` (checked in R9): the kept Debug tree (receipt p) and Release tree (receipt q) hold exes whose
SHA-256 equal their receipts (`3bd62115` / `82ffe2f8` / `89c43003` / `f39fba52` Debug; `74ad03cf` / `1ae4d237` /
`7e1ddbc3` / `70e96e5e` Release; targets, targets exe, IC, eval). Their source is `f41719c2`; `git diff f41719c2
3129ca14` over C++ / CMake is `seal_guard.{hpp,cpp}` and `research_catalog_seal_test.cpp` only, all inside
`atx-engine-research-catalog` (EXCLUDE_FROM_ALL; every includer of `seal_guard.hpp` is in that library; none of the
four link lines in `build-equity/build.ninja` names it), plus `classes.json` (run-time data of the store CLI). So both
trees are current for the four targets at `3129ca14`, and at R9's merge `d31b9961` (root `685e238b` adds only
`p9-prereg.md`).

Build tags used in R8: `p9-c3-o`, `p9-c3-p`, `p9-c3-q`. Next free build tag: `p9-c3-r`.

## C3b R9 round (2026-10-07; hand-back for the re-review)

- Commits: `7a487eb8` DIRTY-1 (the two exit tables as they stood); `d31b9961` merge of root `685e238b` (docs only:
  `p9-prereg.md`; clean; git's default message); `51860229` R8 reconstruction + the R8 logs; then the records commit
  (Part B of `task-C3-report.md`, this section, the R9 logs). No code changed in R9; no build (tag `p9-c3-r` unused).
- Ruling TEST-1 (owner) reached the lane mid-round: no Release build, canary, whole target-tests, whole `scripts/tests`
  or stage chain until the gate. Already evidenced in R8 and recorded as done from its receipts and logs: C++ teeth o,
  Debug p (+ canary), Release q (+ canary), named 34 / whole 391 on both. The C3-14 Release canary re-run had already
  launched (bounded quiet-host poller `c3-logs/r9-quiet-run.sh`, 18:33, CPU 56 %, no build process, 3.9 GB free)
  when TEST-1 arrived: 15 passed (`c3b-pyt-r9-canary-q-release-rerun.log`, `r9-host.txt`).
- TEST-1 run: the lane's wave pytest files at seed 0 on `51860229` (code = `d31b9961`): `test_wave_gross_match.py`,
  `test_research_wave.py`, `test_wave_driver.py`, `test_wave_review.py`, `test_wave_review_minor.py`,
  `test_wave_scoreboard.py`, plus the lane's own small `test_wave_speed.py` and `test_no_python_mirror.py`: 104
  passed, 0 failed, 0 skipped, ENV-1 0, 325 s (`c3b-pyt-r9-wave-files-s0.log`).
- Deferred to the gate (TEST-1): the valid seed-1 run of the whole `scripts/tests` (R8's was void under ENV-1); the
  whole suites, Release build and canaries on the gate head.
- Item 6 PENDING: B2's redo is not in root (`git -C C:/atx-wt/pool-2 log --format=%s -30`, R9).
- Object trees: deleted by name after the records commit (the hand-back message gives the freed size).

### Next step

Re-review of C3b (BASE per REV-BASE: the SHA the previous review names as reviewed). Then root's merge (Part B "What
root re-proves"). Item 6 (D2R-2 (a), text only) is a short follow-up once B2's redo is in root.
