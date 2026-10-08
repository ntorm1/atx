# Lane C3b review (part B of lane C3; P9 RESUME 9)

Reviewer: fresh adversarial reviewer (Opus 5.5). Pool-15, branch `feat/p9-c3-20261004`, HEAD `b2517fe4` (tree clean).
BASE = `git merge-base b2517fe4 feat/platform-p9-20261003` = `685e238b` (REV-BASE; part A is already in root).
Range reviewed: `685e238b..b2517fe4`: code `278fc2d8` (items 1-5), `f41719c2` (S-13), merges `7c038124`, `3129ca14`,
`d31b9961` (root docs / SQL3 code; none conflicts with a lane file), records `e28d7849`, `7a487eb8`, `51860229`,
`98bba064`, `b2517fe4`. Code diff: 27 files (4 C++ under `atx-impl/`, 23 Python / JSON under `scripts/`); records diff:
34 files, all the lane's own (`c3-logs/`, `handoff-C3.md`, `task-C3-report.md`).

Read: `r7-review-dispatch.md`, `resume8-addendum.md`, `resume9-addendum.md` (TEST-1, TEETH-RESTORE), `r8-review-notes.md`
"C3b", `briefs/review-template.md`, `r7-dispatch.md` / `r8-dispatch.md` / `r9-dispatch.md` "C3b", `briefs/brief-C3.md`,
`task-C3-review.md` / `task-C3-rereview-1.md` (context), `task-C3-report.md` "## Part B (C3b)", `handoff-C3.md` ("C3b at
the owner stop" design notes (a)-(k), "C3b at the second owner stop", "R8 round (reconstructed from logs)", "C3b R9
round"), `c3-logs/`, `.agents/cpp/agent.md` sections 0-10; ledger by grep: C2-9, C2-10, C2-11, C3-7, C3-10, C3-11,
C3-12, C3-13, C3-14, C3B-6, PRE-27, PRE-40, E2-16, E2-16a, EXE-CUR, PERM-1, REV-BASE, TEST-1, TEETH-RESTORE, W3-6.
Blind: root history read as one-line subjects; no 2020-2023 output or statistic opened (the only numbers seen are
synthetic fixture values and the tiny-world canary's synthetic lines). No build. No permission refusal.

## Verdict: APPROVE

Every item of the C3b task list is done, pending by rule (item 6, now root's under C3B-6) or dropped by ruling (item 7,
C3-11). The Python `pm6-6` route is refused at preflight, run and match (exit 3), plan / status of an old state dir still
work, and the ledgered v1 manifests still validate; `gross_match` has no default (C2-9); the digest re-check sits in
`wave_result.build` before anything is built or written (C2-10 / C3-12); E2-16a and PRE-27 are pinned by tests;
`read_grid` refuses a repeated key before any output with exit 1 like every other grid-file refusal, and an unrepeated
file parses to the same document (the callback keeps every value). No Python copy of the gross-match rule survives
outside tests; the G-P5 row is retired and the names stay detected. The C++ change is one function moved out of an
anonymous namespace plus one 8-line call site. My own runs agree with the lane's counts and my one mutation was caught.
Nothing Required; five Suggested, none blocking.

## Required

None.

## Spec verdict (item by item; file:line at `b2517fe4`)

| item | verdict | evidence |
|---|---|---|
| 1 merge root | done | `7c038124` (root `1b64ce45`), `3129ca14` (root `fd084e78`), `d31b9961` (root `685e238b`); `git diff --stat` of each merge's second side touches no lane file; no conflict |
| 2 (b)-(e) C2-11 retire the Python `pm6-6` | done | `scripts/wave_rules.py:113-129` (`GROSS_MATCH` = none / pm6-6-engine; `RETIRED_GROSS_MATCH`, `retired_gross_match`; `matched_leverage` / `gross_matches` deleted); refused at preflight `scripts/wave_stage_preflight.py:169-171` (a problem, so `preflight refused: ...` exit 3 at `:206`, before register), at run `scripts/wave_stage_cell.py:78` and match `:252` (`refuse_retired` `:48-53`, `StageError(..., EXIT_PIN)`); plan prints `#   REFUSED (exit 3): ...` for run and match (`:112-114`, `:411-413`); `validate_names` accepts the retired name `scripts/wave_rules.py:185`, so `scripts/specs/v8/waves/y-{2,3,5,s}.json` and `y-s.head.json` validate as before (`scripts/tests/test_wave_gross_match.py:63`); the message names `"pm6-6-engine"` and the C2-9 replay route |
| 2 ledgered files of design note (a) never edited | done | `git diff 685e238b b2517fe4 -- scripts/specs/v8 .superpowers/sdd/platform-v8-20260929 scripts/specs/p9/rules` is empty; no commit of the range touches them |
| 2 (f) C2-9 no default | done | `scripts/wave_manifest.py:260` drops `gross_match` from the generic missing list, `:264-265` + `:338-347` one dedicated line naming `"pm6-6-engine"` (or `"none"` for a v2 kind `leverage`) for an absent or null key; plan exits 2 (`test_wave_gross_match.py:140`); the old `engine()` default (absent = Python match) is gone: no `gross_match` default remains anywhere in `scripts/` (grep) |
| 2 (g) C2-10 / C3-12 digest re-check | done | `scripts/wave_result.py:54` (`refuse_cell_digests`, `:95-118`): runs once the cell dict is formed from the in-memory receipts and before anything else in `build`; `build` writes nothing and its only caller `scripts/wave_stage_record.py:262` writes only after it returns, so a mismatch leaves no wave-result, log section, ledger head or sprint copy (asserted at `test_wave_gross_match.py:208-209`); checks the cell of record and, engine route, the run recipe; `gross_match none` falls back to the spec stage's digest (`wave_result.py:47`) and is covered (`none-cell` case) |
| 2 (h) E2-16a test | done (test only, as ruled) | `test_wave_gross_match.py:213`: the `-cal` run recipe and the `-gm` cell of record both resolve `ref.leverage` 1.1474 while the record's `nav.leverage` is L'; the inherited-ref route stays covered by C2's `test_research_wave.py::test_engine_run_recipe_pins_the_reference_to_the_uncalibrated_construction` |
| 2 PRE-27 | done | `test_wave_gross_match.py:250`: a v2 wave on wave 1's `-gm` passes preflight and calibrates at L' 1.1782; on its `-cal` it is refused at preflight (E2-14 R3a), exit 3, no register |
| 2 (i) C3-7 `read_grid` duplicate key | done | `atx-impl/src/book/nav_run.cpp:130-136` after the size / read / sha checks, before the schema check; message `nav grid: duplicate key "K" in PATH (...)`; returned as an error, printed and `return 1` at `atx-impl/src/book/nav_cli.cpp:219-220` like every grid refusal (design (i) said 2; the deviation is recorded in the report and consistent with `ConstructionGrid.RefusesBadGridsBeforeAnyOutput`); refused before any output (`atx-impl/tests/strategy_nav_spec_test.cpp:413` asserts no output dir) |
| 2 flag-absent identity of C3-7 | holds by construction | `atx-impl/src/book/nav_spec.cpp:347-362`: the callback returns `true` for every event, so the DOM is the one `Json::parse(text, nullptr, false)` builds (same `allow_exceptions=false`, same comment handling); `parse_spec` (`:319-326`) keeps its messages; the grid sha is of the same bytes |
| 2 (i) S-12 | done | `strategy_nav_spec_test.cpp:447-455`: the first console line equals `nav spec: atx.nav-spec/v1 <file> sha256 <SHA-256 of its bytes>` |
| 2 teeth for every new refusal | done | Python P1-P6 (`c3-logs/c3b-teeth-py.log`: 6 / 6, each caught by its own test, SHA-256 restore checked by the tool); C++ `p9-c3-o` (`c3b-teeth-o-named.log`: 34 ran, 33 passed, 1 failed = `NavSpec.GridFileRefusesADuplicateKey`, exit 0 where 1 expected, no message, output dir created); my own mutation below |
| 3 item 7 dropped (C3-11) | done | `sign_pm7_35` still at `scripts/wave_rules.py:39`; its ALLOWLIST row kept `scripts/tests/test_no_python_mirror.py:75`; stated in the report |
| 4 PRE-40 | done | `scripts/specs/p9/rules/mom-volman-v1.json` SHA-256 `8a7222fa...` and `theme-cov-hedge-v1.json` `55f25f2f...` equal the prereg pins (`p9-prereg.md:1083-1084`); their last commits are AL-COMB's (`efb7ab2e`, `97a2d1c3`, `36e1434c`), none in the range |
| 5 cross-lane hunks in D3's files | done, minimal, listed | the eight kinds fixtures: one line each, line 18 (`@@ -15,7 +15,7 @@`), `"pm6-6"` -> `"pm6-6-engine"`; `scripts/tests/test_wave_kinds.py` three hunks `@@ -609,8` (2 lines: `cell_spec` -> `record_spec`), `@@ -729,7` (1 line: parametrize `["pm6-6-engine"]`), `@@ -756,9` (3 lines of the dead branch deleted); stale docstrings left and listed (`:543`, `:563`, `:584`, `:734`, `:737`). Dry merge with D3's head `b9b288e5` (`git merge-tree`, old mode, writes nothing): no conflict in either file set (D3's `walk-forward.json` hunk is lines 26-31, separate) |
| 5 wave tests rewritten for the engine route test the same subjects | done | `test_research_wave.py` (library wave to its result, dropped string, the engine-vs-Python test replaced by `test_engine_gross_match_on_the_pinned_fixture` with L' 1.1782 pinned as a literal, no Python oracle); `test_wave_driver.py` (other-exe parent NAV, reader reuse, timings: same subjects on the `-gm` NAV / `cycle-w1-gm`, readers `mech-parent` / `mech-matched`); `test_wave_review.py` (stale-on-resume moved to `none`, the engine run pins are `test_research_wave.py:353`); `test_wave_review_minor.py` (two binding tests on `none`, see S-3); `test_wave_scoreboard.py` (lineage `-gm`); `test_wave_speed.py` (`--stop-after w`, one NAV 41.5 s) |
| 6 item 6 D2R-2 (a) | pending by rule; root's (C3B-6) | B2's redo is not in root; text at `scripts/cycle/spec.py:240-244`, `:264-269`, `:280-283` (design (j)) |
| 7 optional S-13 | done, accurate | `atx-impl/src/book/nav_spec.hpp:32-35`: `--spo-gross G` a hard cap on planned gross, G in (0, 1.5 L], default L, spo-v2's 1: matches `strategy_nav_v7.cpp:1025-1028` (help text), `:1297-1303` (validated only when given), `strategy_spo.cpp:422-431` (`gross_budget_of` defaults to L; `validate_gross_budget` (0, `gross_budget_sanity` = 1.5 (`strategy_spo.hpp:140`) x L]) |
| 7 optional C3-10 log line, E2-11 | not done, reasons stated | not a one-line change (`book/nav_deploy.cpp:40` has no stream); no P9 SPO cell |
| 8 builds / suites | done in R8, rest deferred by TEST-1 | Debug `p9-c3-n`, `p9-c3-p`, Release `p9-c3-q` (receipts, source `f41719c2`): named 34 / 34 and whole 391 / 391 (1 disabled) on each; canary 15 / 15 on p and q; the one Release wall-time miss at `3129ca14` (15.3 s vs 15.0 s, digests matched) re-run once on a quiet host, 15 / 15 (C3-14); whole `scripts/tests` seed 1 at `3129ca14` VOID (ENV-1, 528 hits; the 4 failure sections without the code are the host-memory-gate tests and one nested pytest) and deferred to the gate |
| 9 report / handoff | done | Part B lists items, hunks, root's two evidence tools, what root re-proves; handoff has the R8 reconstruction and R9 |
| R8 reconstruction honest (EXE-CUR) | yes | every claimed run has its log or receipt in `c3-logs/` (receipts `build-equity/mega-p9-c3-{o,p,q}-receipt.json` byte-equal to the copies); canary logs (UTF-16) read: n 14 + 1 wall-time miss, p 15, q 15; build p's log compiles exactly one lane TU (`book/nav_run.cpp`) after the teeth build; `nav_run.cpp` SHA-256 `501cb0c1...` = HEAD blob now; `git diff f41719c2 b2517fe4` over C++ / CMake is `seal_guard.{hpp,cpp}` and `research_catalog_seal_test.cpp` only, in `atx-engine-research-catalog` (EXCLUDE_FROM_ALL, linked by no `atx-impl` target), so receipts p / q describe the head's four targets; the reconstruction marks what has no evidence as NOT done (Part B, object-tree deletion) |
| root's evidence tools that call deleted functions | listed | `m2e-logs/tools/c2_gm_pair.py:156-159` (`WR.matched_leverage`, `WR.gross_matches`), `m2h-logs/m2h2-engine-o1.py:125` (variant `"py"` at `pm6-6`) |
| SQL2-CLS | nothing new | the only `atx.<name>/v<n>` literal on an added line is `atx.nav-construction-grid/v1` in the new test, a schema the grid reader already had (`nav_run.cpp:113`) |

## Quality verdict

- No Python mirror of an engine rule survives in the wave path. `git grep` finds `matched_leverage` / `gross_matches`
  only in `FROZEN_PAIRS` (`test_no_python_mirror.py:101`, so a reintroduction anywhere in SCOPE fails the guard) and in
  root's evidence tool; no `L x G_parent / G` arithmetic remains outside tests (the fake exe's inline L' in
  `scripts/tests/wave_fixture.py:343` is a test oracle; tests are outside the G-P5 scan by design). `match_engine`
  compares the exe's own record with the mechanics reader (equality), and `GROSS_MATCH["pm6-6-engine"]["tolerance"]` is
  a parameter handed to the exe (`--calibrate-tolerance`), not a rule.
- Flag-absent identity: `gross_match none` keeps its match receipt byte for byte (same keys, same order, `tolerance`
  None, same log line; `scripts/wave_stage_cell.py:258-273` vs the old branch); only the plan text changed (stated).
  Engine waves gain a read-only check. NAV: no byte moves for a file without a repeated key. No new key, flag or
  literal.
- C++: minimal and local (one function exported from `nav_spec`, one call site); `[[nodiscard]]`, contract comment,
  out-param cleared first; no shadowing; every changed C++ line within 100 columns; compiled under `/W4 /WX` in Debug and
  Release at `f41719c2` (receipts, exit 0). Python lines within the scripts' 120.
- Fail-closed paths: `match_none` refuses a live mode with no stage (`:263-264`); `retired_gross_match` ignores
  non-strings; the digest check reads only.
- Tests pin contracts (exit codes, the named file, no output written, the control passing), not implementation, with one
  weak spot (S-4).

## Suggested

| path:line | severity | problem | fix |
|---|---|---|---|
| `scripts/wave_result.py:103` | Suggested | `w.spec_digest(rel)` parses the spec (`research_spec.read` / `chain`): a cell spec committed in the S14 window as invalid JSON, or with a broken template chain, raises `TemplateError` out of `build`; the chain records it as a generic failure (exit 4, `stage_chain.py:276`), not the C2-10 refusal (exit 3). Still fail closed (nothing written) | catch `research_spec.TemplateError` / `ValueError` in `cell_digest_problems` and report the digest as unreadable (a mismatch), so every S14 edit gets the exit-3 message |
| `scripts/tests/test_wave_review_minor.py:146`, `:157` | Suggested | the two reader / bundle binding tests moved to `gross_match none`; the engine route that every P9 cell runs has no assertion that book / bundle bind the `-gm` NAV's `summary.json`, `daily_s2.csv`, `capacity_curve.csv` and that `mech-parent` / `mech-matched` bind their one NAV (not covered before C3b either: the default was the Python route) | add an engine-route variant with per-reader expected NAVs |
| `scripts/tests/test_wave_gross_match.py:127-136` | Suggested | `test_match_fails_closed_on_a_mode_without_a_stage` passes a `done` with only `spec.cell_spec`; with the fail-closed check removed (the lane's P4) it fails on `KeyError: 'run'`, not on the mode being matched as none: caught, but by an incidental crash | give it the run / spec receipts a real match stage reads, so the mutation shows as an uncorrected "none" receipt |
| `scripts/wave_manifest.py:264-265` with `scripts/wave_kinds.py:198` | Suggested | a v2 kind `leverage` manifest without `gross_match` gets two lines (C2-9's and the pre-existing `kind leverage: gross_match must be "none"`); they agree, the test (`test_wave_gross_match.py:155`) accepts either | leave, or let D3 skip its line when the key is absent (D3's file; not C3b's) |
| `atx-impl/src/book/nav_spec.cpp:347` | Suggested (P10) | owner priority (centralize): the duplicate-key JSON callback still has about twelve hand copies (`atx-engine/src/eval/trial_epoch.cpp:76`, `atx-impl/src/prereg.cpp:86`, `panel_artifact.cpp:192`, `stage_data_provenance.cpp:71`, `stage_equity_{baseline,book,ic,universe}.cpp`, `strategy_runner.cpp:310`, `:400`, `strategy_ic_admission.cpp:607`, `strategy_ic_rules.cpp:36`). C3b rightly reused part A's copy rather than adding one | one `atx-core` helper (with the depth cap several copies carry) in P10 |

## My own runs (TEST-1: the lane's pytest files and one Python mutation; no build)

- Interpreter `C:/Program Files/Python312/python.exe`; `PYTHONDONTWRITEBYTECODE=1`, `PYTHONHASHSEED=0`,
  `-p no:cacheprovider`, own `--basetemp` under my session scratchpad (parent created first); in pool-15 at `b2517fe4`.
- The lane's eight wave files (`test_wave_gross_match.py`, `test_research_wave.py`, `test_wave_driver.py`,
  `test_wave_review.py`, `test_wave_review_minor.py`, `test_wave_scoreboard.py`, `test_wave_speed.py`,
  `test_no_python_mirror.py`), one invocation: exit 0, **104 passed** in 276 s (the lane's R9: 104 passed).
- `scripts/tests/test_wave_kinds.py` (D3's file; C3b edited three hunks and its R9 run left it out), separate invocation:
  exit 0, **111 passed** in 115 s.
- Teeth (my own tool, saves and restores the bytes itself; TEETH-RESTORE): deleted the call site
  `refuse_cell_digests(w, cell)` in `wave_result.build` (`scripts/wave_result.py:54`; a different mutation from the
  lane's P6, which disabled the check inside the helper) and ran
  `test_a_cell_edited_during_judge_writes_no_wave_result`: **3 failed (record-spec, run-recipe, none-cell), 1 passed
  (control): CAUGHT**, each failing at `assert code == EXIT_PIN` (the wave recorded its result). Restored in the same
  run; checks after restore all true: bytes == pre-mutation, `git hash-object` == `HEAD:scripts/wave_result.py`
  (`234cd3b8`), LF-normalised SHA-256 == the blob's (`08e4d73d...`; the checkout is CRLF, `core.autocrlf=true`),
  `git status --porcelain` empty. No `git checkout` / `restore` / `stash` used.
- C++: from the lane's logs only (no built tree is left; I judged no build necessary: the C++ diff is 8 lines at one call
  site, the teeth signature at `p9-c3-o` is exactly the new test, the restore is evidenced, and EXE-CUR holds).
- Dry merges (read-only `git merge-tree <base> <a> <b>`, old mode, writes nothing): C3b x D3 `b9b288e5` and C3b x B2
  `46ffe3dc`: no conflict marker in either.

## What root must check at merge

1. ROOT-1a on the merged tree: `scripts/tests/test_wave_gross_match.py` and the wave suites (`test_research_wave.py`,
   `test_wave_driver.py`, `test_wave_review.py`, `test_wave_review_minor.py`, `test_wave_scoreboard.py`,
   `test_wave_speed.py`, `test_no_python_mirror.py`, D3's `test_wave_kinds.py`); a Debug build of
   `atx-impl-strategy-target-tests` and the named filter
   `NavSpec.*:NavRules.*:CostLawRegistry.*:TradeRateRegistry.*:TargetRegistry.*:NavFieldsSeal.*:NavBookWorkers.*:NavBookRule.*:NavAnnMeanGross.*:CalibrateGross.*:CovStore.*`
   = 34 / 34. Whole suites, Release, canaries: at the gate (TEST-1).
2. Keep-both with D3: the eight kinds fixtures line 18 (`"pm6-6-engine"`; D3's walk-forward hunk at lines 26-31 is
   separate) and the three `test_wave_kinds.py` hunks (D3's head has them at `:626-629`, `:749`, `:776`); git merges
   them cleanly against `b9b288e5`, but D3's own tests that run fixtures to match / verify (`test_wave_kinds.py:877`,
   `:889`, `:978`, `:1517` at D3's head) then run the engine route: run `test_wave_kinds.py` after the merge. The five
   stale docstrings (`:543`, `:563`, `:584`, `:734`, `:737` at C3b's head) can be fixed then (comments only).
3. With B2 (merges first): `scripts/tests/wave_fixture.py`, `test_no_python_mirror.py`, `test_wave_kinds.py` are
   touched by both and merge cleanly; B2's new `scripts/tests/test_wave_eval_verb.py` builds a v1 world with
   `F.build(tmp_path)`, whose default becomes `pm6-6-engine` with C3b: run it on the merged tree. B2 writes no spec file
   after the match stage (checked), so C3-12's digest re-check should not refuse B2's waves.
4. PRE-40 before the re-pin: the two cell templates (`scripts/specs/p9/templates/mom-volman.json`,
   `theme-cov-hedge.json`) carry no `gross_match` key (it is a manifest key); what must name `pm6-6-engine` is root's
   P9-M / P9-H wave manifests (an absent key is refused at plan, C2-9; `pm6-6` at preflight, C2-11). The stale
   `manifest_note` in the two rule files is not edited (D3W-STALE / PRE-40 form).
5. Item 6 (D2R-2 (a)) is root's text-only slip at B2's merge (C3B-6): the three hunks of `scripts/cycle/spec.py`.
6. Root's evidence tools now fail on deleted functions: `m2e-logs/tools/c2_gm_pair.py:156-159`; `m2h-logs/m2h2-engine-o1.py:125`
   (run only the engine variants). Also SQL3's `sql3-evidence/sql3-fakewave-digests.py` runs the fake wave on the
   fixture default: re-run after C3b it prints engine-route digests, so compare it only with a run on the same head,
   never with its stored `fakewave-*.txt`.
7. SQL2-CLS: no new `atx.<name>/v<n>` literal, flag or JSON key; nothing for `classes.json`.
8. G-P5 at the freeze: the kept `sign_pm7_35` row (`test_no_python_mirror.py:75`) still names retiring lane `"E2"`;
   under C3-11 it is lane SIGN's (or P10's). Before root sets `P9_FREEZE = True`, relabel or let SIGN delete it, or the
   freeze test counts it.
