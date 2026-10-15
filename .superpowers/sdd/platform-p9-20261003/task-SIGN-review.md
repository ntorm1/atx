# Task SIGN review (first review; stretch lane; P9 RESUME 10)

Reviewer: fresh adversarial reviewer (FU-REVIEWS "SIGN first review"). Pool-29, branch `feat/p9-sign-20261006`.
HEAD `50a43599` (the deletion commit, LAST), pre-deletion head `c2010ed1` (records; code `8727f6d4`), BASE `f58e3a4c`
(the root head the lane merged last; `git merge-base 50a43599 f58e3a4c` = `f58e3a4c`, REV-BASE). Lane commits:
`git log --no-merges 50a43599 ^f58e3a4c` = `6800d077`, `558c4ed0`, `422e9fad` (R9 WIP), `e648e85a` (handoff,
SIGN-RESUME first act), `7c7e793e`, `136c4161`, `30816e9c`, `6d4b8bf9`, `c2010ed1`, `50a43599`. Read: the contract
files (resume4 / 6 / 7 / 8 / 9 (TEST-1) / 10), `r7-review-dispatch.md`, `r8-review-notes.md` "A4, ABS, SIGN, D3W-PB",
`r10-dispatch.md` "SIGN" and the SIGN bullet of "Scoped re-reviews", `briefs/brief-SIGN.md`,
`briefs/review-template.md`, the lane's `task-SIGN-report.md`, `handoff-SIGN.md`, `sign-logs/` (tools, r10 logs, the
`p9-sign-a` receipt), `.agents/cpp/agent.md` section 10, pool-15 `task-C3-report.md` "## Part B (C3b)", and the ledger
lines C3-11, C3B-S, SIGN-RESUME, SIGN-CLS, SIGN-TEETH, SIGN-WD, SIGN-SUBST, G-P5, W1-I1, W1-I2, B2-10, EXE-CUR,
EXE-FREEZE, HOST-4, HOST-5, TEST-1, TEETH-RESTORE, PERM-1.

## Verdict: APPROVE

0 Required, 5 Suggested. Spec: every brief item is done (or deferred to the gate under TEST-1). Quality: the verb
uses the engine's one predicate and adds no second rule. Every refusal and the decision wiring have teeth (my build
`p9-sign-b`: 23 / 23 caught, the tree restored byte for byte). No Python mirror is left (G-P5 13 functions in 5 rows
at HEAD). The records are blind. SIGN is a stretch lane, so it merges only if it is still APPROVE at the gate (C3-11 /
GATE-1).

## Merges in range

| merge | parents | check | result |
|---|---|---|---|
| `166df1aa` (root `b182dfdd`) | `e648e85a`, `b182dfdd` | `git merge-tree --write-tree` = `ca17fe92`, merge tree `ca17fe92` | plain |
| `8727f6d4` (root `f58e3a4c`) | `6d4b8bf9`, `f58e3a4c` | recompute `01b8448f` has one conflict, `atx-impl/CMakeLists.txt`; `git diff 01b8448f 74a38764` (merge tree) touches only that file | the one CMake tail region, resolved keep-both (ABS block first, then SIGN's 5 lines); nothing else differs from git's recompute |
| `749d2c32` (C3b `76b9f256`), `9c360cde` (root `fcb0200e`), R9, before BASE | | recompute = merge tree (`11799da8`, `bd09016c`) | plain |

## Spec verdict (brief item by item)

| item | verdict | evidence |
|---|---|---|
| Exe and key: an exe the screen stage already pins; no new pinned-exe key | DONE | `atx-equity-strategy-ic sign` (`equity_strategy_ic.cpp:16`), key `exes.ic`. `sign_exe` (`wave_stage_library.py:152-164`) takes `RC.effective_exes(spec)["ic"]`, the exe that `research_cycle run --screen` itself runs, and holds it to `exes_sha256.ic` when the spec pins it (the `verify_exes` rule, `cycle/phases.py:273-287`; a missing pinned exe is refused too). The lane's diff does not touch `scripts/cycle/**`, `research_cycle.py`, `wave_manifest.py`, `wave_stage_util.py`, `wave_context.py` or the schemas. W1-I2 and B2-10 are unchanged. |
| Verb input / output; refusals before any output | DONE | Header contract `strategy_sign_verb.hpp:3-27`. Order in `run_sign` (`.cpp:280-293`): an existing output is refused first (exit 1), then read (16 MiB cap), SHA-256, parse (every row validated, duplicate ids, 20000-row cap), and only then the write. Usage errors exit 2 (`parse_args` :309-343). Receipt `atx.sign-receipt/v1`: rule, sign_rule, predicate, rows_file, rows_sha256, rows[] with verdict and reason, kept, dropped. |
| Gtests: every row of `research_admission_sign_test.cpp:46-75` through the verb; refusals | DONE | `SignVerb.DecisionTableThroughTheVerb` (`strategy_sign_verb_test.cpp:96-143`) writes out the 12 rows of the table's wave column, at prior +1 and mirrored at -1, and runs them through `dispatch_sign`. The table's gate column (GatePriorV1) cannot be reached through the verb, because no wave rule id maps to it; my D1 mutation shows the test catches exactly that wiring (c4 / c16 flip). There are 27 malformed-file cases, the missing / existing-output cases and 6 usage cases. Lane: 6 / 6 on `p9-sign-a`. |
| The stage's one call line; `sign_pm7_35` no longer called | DONE | `wave_stage_library.py:122` `sign = sign_verb(w, spec, m["sign_rule"], sign_in)`. The decision is read back from the receipt by `WR.verb_decision` (`wave_rules.py:63`), which refuses a receipt that is not the verb's answer to exactly these rows. The function is deleted at HEAD, and `test_the_fake_verb_is_the_pinned_column` asserts it is gone. |
| Route absent: the ledgered v1 manifests still validate; their results untouched | DONE | No flag is added (the route is always on, as the brief asks). `test_ledgered_v1_manifests_still_validate_and_name_pm7_35` passes (my run). `git diff --stat f58e3a4c 50a43599 -- .superpowers/sdd/platform-v8-20260929 scripts/specs` is empty. The `wave-result.json` screen layout is unchanged (`wave_result.py` was not edited; `test_wave_sign_verb.py:143` pins the keys). The spec stage's inputs for a screen recorded before SIGN are unchanged (`:299`; the ledgered-manifest test is `:285`). SIGN-SUBST: nothing for the substitution list. |
| G-P5 row deleted in the SAME commit as the function (the LAST commit); the name stays detected | DONE | `50a43599` deletes `sign_pm7_35` and `screen_decision` (`wave_rules.py`) and the row (`test_no_python_mirror.py:64-66`, now a comment). It touches 4 files, all `scripts/`. Its blobs equal the lane's tested blobs (`r10-deletion-tested-blobs.txt`: 71a23ba3 / 9c6e3252 / 4c1c8393 / 2b60956e). `FROZEN_PAIRS` :91 still lists `("scripts/wave_rules.py", "sign_pm7_35")`, so a reintroduced def of that name fails `test_no_new_python_mirror`. C3B-S: the row was relabelled `SIGN` in `422e9fad` and deleted here. |
| Identity pytest (24 combinations vs the Python rule) before the deletion | DONE | `COMBOS` = 2 statuses x 3 runner signs x 2 kinds x 2 priors = 24, a superset of the eight cases of `test_sign_rule_pm7_35_as_ruled` and of the 12-row table (`test_the_combinations_cover...`). Real exe: 24 / 24 = `sign_pm7_35` = fake = pinned column, on the `p9-sign-a` IC exe at `6d4b8bf9` and again at `8727f6d4` (lane logs; EXE-CUR below). Fakes (my run at `c2010ed1`): `test_the_pinned_column_and_the_fake_verb_are_the_python_rule` passed. |
| Identity procedure for root on the ledgered screens BY PATH | DONE | Report :120-149 lists the four tracked paths (`.superpowers/sdd/platform-v8-20260929/waves/{y-2,y-3,y-5,y-s}/wave-result.json`), the argv, and the tool `sign-logs/sign-identity-ledgered.py`. The tool prints counts and ids only, and refuses (exit 1) on any difference or verb refusal. Neither the lane nor I opened those files. |
| Report contents (exe / key / why; verb -> file:line -> tests -> teeth; pre-deletion head and deletion commit; what root re-proves) | DONE | `task-SIGN-report.md` :28-207. |
| Builds / suites: whole target tests, Release, `scripts/tests` seeds 0 / 1, stage chain | deferred to the gate (TEST-1) | |

SIGN-RESUME order held: handoff `e648e85a` came first, then the root merge, checks, `p9-sign-a`, tests, identity,
Python teeth and report, with the deletion last. The C++ teeth went to this review (SIGN-TEETH).

## SIGN-WD (`test_wave_driver.py` sticky-pins hunk `7c7e793e`): ACCEPTABLE

- (a) No refusal assertion is removed. The sticky-pins segment
  (`test_lock_exes_pins_and_verify_compares`, :1077-1087) never asserted a moved-bytes refusal, before or after the
  hunk: it asserts that a pinning parent implies `lock --exes` on the library spec. `lock_exes` (`wave_stage_util.py:48-57`)
  is `bool(parent.exes_sha256)`, so it is still exercised with `{"nav": nav}`. The function's moved-bytes refusals
  ("PIN MISMATCH exe nav", verify's `exe_problem` on differing nav pins) are untouched and pass (28 / 28, my run). The
  new refusal that made the hunk necessary, a pinned IC exe whose bytes moved or that is missing, is asserted for the
  IC exe in `test_wave_sign_verb.py::test_the_ic_exe_must_be_its_pin` (pin match, "0"*64, missing, no `exes.ic`) and
  had teeth (lane P8).
- (b) The only change is the world's pin set (`{"ic", "nav"}` -> `{"nav"}`), plus 3 comment lines. The IC exe now has a
  Python-side pin check at the screen, and the fake world puts no exe on disk because W1-I1's cache bound would refuse
  it (`r10-pyt-driver-fix-s0.log`).
- (c) No assertion is weakened, and the 1-token hunk is the smallest change that passes.

## SIGN-CLS: `atx.sign-receipt/v1` belongs in class `ic-output` (O4), not `stage-receipt`

The receipt is written at `out/waves/<w>/sign/receipt-<k>.json` by `atx-equity-strategy-ic` (`ordered_json::dump(2)`,
insertion order). The class `stage-receipt` fits none of that:

- its glob `**/receipts/[0-9][0-9]-*.json` does not match the receipt path;
- its ingest `stage_receipt` (`ingest_stage.cpp:17-38`) needs a `receipts/` parent and the keys chain / stage / index /
  status / inputs / outputs / started_utc / seconds, so the receipt would be "unparsed";
- its render `py-indent2-sorted` would not match the verb's insertion-ordered bytes.

Listed there, the literal would pass the guard, but the file would still classify as `other`. `ic-output` fits:
- glob `**/*.json`;
- ingest `artifact-only`, render null;
- its writer is the IC exe. Root extends `writer` with "strategy_sign_verb (`atx-equity-strategy-ic sign`, P9 SIGN)"
  and `pinned_by` with "the screen receipt's `sign.receipt_sha256`; spec stage input `screen.sign.receipt_sha256`".

No class before it in the list holds the schema, so the file classifies there. This departs from SIGN-CLS's default
(`stage-receipt` "if classes.json has it"); the ruling's "else the nearest existing class" branch is the one that
applies. This is a PM note, not a lane finding.

## Quality verdict: PASS

- One rule, in the engine. The only verdict is `adm::pm7_35(cfg.rule.predicate, kind, admitted(row), prior,
  runner)` (`.cpp:244`). Admission is decided by `status == adm::status_name(Status::Admitted)` (:195-197), and the
  rule id table maps `pm7-35` to `adm::kPm735SignRule` (:39-43), so B1's single switch stays the switch. `reason_of`
  (:216-236) only describes a verdict already made (D2 shows the words are pinned by `ReasonsAreTheWaveRulesWords`).
- Python is input and receipt reading only: `sign_rows` (:50) and `verb_decision` (:63) decide nothing. The fake verb's
  `wave_fixture.sign_oracle` is a test oracle, out of the G-P5 scan by design (`test_no_python_mirror.py:23-24`),
  as with C3b's `FakeCycle.calibrate`. It is held to the real exe and to the pinned column.
- Refusals with teeth: C1-C21 are all caught (below).
- Bounds: integers are type-checked, and an unsigned value is range-checked before narrowing (:99-109; C5 shows a
  u64 max would otherwise wrap to -1).
- `/W4 /WX`: no warning from a lane TU in the lane's checks, or in my build of the merged head (only the third-party
  databento `getenv` deprecations print).
- 100 columns are held in all four C++ files, and the lane's Python lines are <= 120.
- Determinism: ordered_json, wave order, no unordered iteration.
- `.agents/cpp/agent.md` section 10: no UB or narrowing (every cast follows its check); `[[nodiscard]]` / `noexcept` /
  `const` where they hold; the `switch` is exhaustive; loops are bounded; the header documents the contract; the test
  sits beside its siblings. Hygiene preset not run (TEST-1).
- Modularity: the verb is one TU behind a two-symbol header, plus one verb-table line. The file IO is hand-rolled
  where an engine helper exists (S-2, P10).
- Blind: a grep of the lane's records (report, handoff, every committed `sign-logs/` file) and of its commit bodies
  for statistic terms and decimals found nothing. The report's disclosure (:211-214) names the incident (the ledger
  line R0-6 printed by a grep) and carries no number from it.
- Scope: 13 code / test files, all listed. The cross-lane hunks are the `test_wave_driver.py` hunk (accepted above)
  and the shared fixture's `sign` route; no kind file and no B2 file is touched.

## Findings

| path:line | severity | problem | fix |
|---|---|---|---|
| `.superpowers/sdd/platform-p9-20261003/sign-logs/sign-teeth-cpp.py:25-99` (the lane's tool, never run) | Suggested | (1) Applied at once, its mutations are predicted not to compile under clang-cl `/W4` (= `-Wall -Wextra`) with ATX_WERROR's `/WX`. C4 + C5 leave `lowest` / `highest` unreferenced (`-Wunused-parameter`); C8 + C10 leave `non_empty_string` unused (`-Wunused-function`); C16 leaves `kRowsLimit` unused (`-Wunused-const-variable`). (2) The C19-C21 markers can show without their mutation: once C1 leaves a receipt at the shared path, `EXPECT_FALSE(fs::exists(receipt)) << needle` prints the bare needle in later usage cases. The test-name markers are always present, in the `[ RUN ]` lines. (3) No decision mutation. | Use the form in the review's copy `sign-logs/review-teeth-cpp.py` (uncommitted, beside its log): N1-N4 put `[[maybe_unused]]` on those four names (no behaviour change); markers are `[  FAILED  ] <test>` and `needle: `; D1 / D2 are added. This is an evidence tool only, and no merged byte changes. |
| `atx-impl/src/strategy_sign_verb.cpp:177-193`, `:268-286` | Suggested | The bounded read and the check-then-truncate write are hand-rolled. The engine already has both: `research/fields/file_io.hpp:24` `read_bounded` and `:37` `write_exclusive` (C11 "x", O_EXCL). With an exclusive write, the header's "NEWFILE (exclusive; never overwritten)" (`.hpp:19`) would hold without the window between `fs::exists` (:283) and the truncating `ofstream` (:269). The house pattern elsewhere is the same check-then-write (`nav_publish.cpp:130`, `strategy_exposures_verb.cpp:127`). | P10, with C3B-S S-e: one bounded-read / exclusive-write helper in atx-core, used by the verbs. |
| `scripts/tests/test_wave_sign_verb.py:93`, `:110` | Suggested | The two real-exe cases (the C++ identity and the real refusals) skip silently without an exe, so a run without `ATX_EQUITY_IC_EXE` is green and proves nothing about the verb (my HEAD run: 19 passed, 2 skipped). | Turn the skip into a failure under a REQUIRED env (as the canary's `ATX_CANARY_REQUIRED=1`), or root records "0 skipped" at merge, as the lane's log does. |
| `atx-impl/src/strategy_sign_verb.hpp:5-6`, `strategy_sign_verb.cpp:117`, `:214`, `atx-impl/tests/strategy_sign_verb_test.cpp:25`, `:145`; `strategy_sign_verb_test.cpp:84`, `:190`, `:295` | Suggested | After `50a43599` the comments name a deleted function (`wave_rules.sign_pm7_35`; ":44-57" is stale). The continuation lines lost their alignment in the `Run` -> `VerbRun` rename (`6d4b8bf9`). | Wording / indent only: name the decision table or `wave_fixture.sign_oracle`, or add "(retired by SIGN)". |
| `.superpowers/sdd/platform-p9-20261003/sign-logs/sign-identity-ledgered.py:58-72` | Suggested | (verdict, reason) is compared as one pair, so root cannot tell a wording drift from a decision change without opening the rows. A recorded row from the pre-`RuleError` branch (`return DROP, "no admission row"`, as at `487faf0b`) would be refused by the verb and print as a refusal. I checked from git code only: the scored-row branches and words of `sign_pm7_35` are identical from `487faf0b` (an ancestor of all four results' first commits `d9e337b6`, `1e7e96be`, `a063ad51`, `666436a8`) to `c2010ed1`; only that None-row branch changed. | Print verdict-identity and reason-identity counts separately. Root reads a refusal of a recorded "no admission row" row as the pre-SIGN rule change (a PM ruling), not as a verb defect. |

## Verified by my own runs vs from the lane's logs

Mine (pool-29; Python 3.12 explicit, `-p no:cacheprovider`, own `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`, no
`ATX_EQUITY_IC_EXE`; logs `sign-logs/review-*.log`, git-ignored, uncommitted):

| run | head | result |
|---|---|---|
| `scripts/tests/test_wave_sign_verb.py` | `50a43599` (no exe in the tree) | 19 passed, 2 skipped (the two real-exe cases: no exe) |
| `scripts/tests/test_no_python_mirror.py` (own invocation) | `50a43599` | 7 passed; "G-P5: 13 mirrored rule function(s) in 5 row(s)" |
| `scripts/tests/test_wave_driver.py` | `50a43599` | 28 passed (incl. `test_lock_exes_pins_and_verify_compares`) |
| identity pytest on fakes: `test_wave_sign_verb.py` of the pre-deletion tree (`git -c core.autocrlf=false archive c2010ed1` of `scripts`, `atx-engine/tools`, `atx-impl/tools`, `atx-impl/strategies` into my scratch; blobs checked = `c2010ed1`'s) | `c2010ed1` | 19 passed, 2 skipped (real exe); `test_the_pinned_column_and_the_fake_verb_are_the_python_rule` and `test_the_combinations_cover...` passed: on all 24, the Python rule = the fake = the pinned column |
| C++ teeth, ONE build `p9-sign-b` (`atx-impl-strategy-ic-tests`; `research-build.ps1`; configure `equity-dev` first, the lane had deleted its tree). Waited under HOST-4 / HOST-5 behind AL-CLOCK's `p9-alclock-m` (`review-build-wait-1.log`, quiet at 22:06) | `50a43599` + the mutations (receipt: Source `50a43599`, DirtyEntries 1, 341 TUs, exit 0, 307 s, tests exe `4b174f4f...` = a TEETH exe, never evidence) | 23 / 23 caught: C1-C21 (every refusal) + D1 (DecisionTable: c4 and c16, the add / admitted / runner 0 rows, flip to drop) + D2 (Reasons: "against prior 1"). `SignVerb*`: 5 failed as designed; `ReceiptPins...` passed (no mutation aims at it). RESTORE: bytes equal, SHA-256 `b7d03769...` = kept, `git hash-object` = HEAD blob, `git status --porcelain` empty. Log `review-teeth-cpp-b.log`; tool `review-teeth-cpp.py` |

Teeth form, disclosed:
- The tool is the lane's `sign-teeth-cpp.py` mechanics, unchanged (keep bytes, every mutation at once, one build,
  restore in `finally`, assertions), with the three changes in finding 1.
- It builds the test target only, as the lane's tool does, not the IC exe the dispatch names. The gtests call
  `dispatch_sign` in process, and a mutated IC exe at `build-equity/bin` is where `test_wave_sign_verb.py` looks by
  default.
- The object tree (2.4 GB) was deleted by name afterwards. Kept: the receipts / logs of `p9-sign-a` and `p9-sign-b`,
  `vcpkg-manifest-install.log`, and the tracked `audits/` and `v8-interim3-pitch-render-run2/`. Porcelain stayed
  empty.

From the lane's logs only:
- `SignVerb*` 6 / 6 clean (`r10-gtest-a-signverb.log`).
- Real-exe identity 24 / 24 and the real refusals (`r10-pyt-signverb-real-a.log` 40 passed at `6d4b8bf9`;
  `r10-pyt-signverb-merged-pre.log` 21 passed at `8727f6d4`; post-deletion bytes 40 passed).
- Python teeth P1-P10 10 / 10 (`r10-teeth-py.log`).
- The classes guard red only on `atx.sign-receipt/v1`.
- EXE-CUR: `r10-exe-sha-a.txt` = the `p9-sign-a` receipt's SHA-256 (IC `76dbd261...`, tests `514e2ff1...`; source
  `6d4b8bf9`, dirty 0); the sprint copy of the receipt is byte-identical to `build-equity/mega-p9-sign-a-receipt.json`.
- Those exes predate the root merge `8727f6d4`, which changed IC-target C++ (D3W, ABS) but not the verb's sources, so
  the clean `SignVerb*` and the identity are current for the verb only. Root re-proves them on its own build at merge.
- The lane's TU checks show no warning.

## What root must check at merge

1. **Merge the pre-deletion head `c2010ed1` first** (code = `8727f6d4`):
   - keep-both at the two CMake tails;
   - SQL2-CLS commit adding `atx.sign-receipt/v1` to `ic-output` (above; PM to confirm against SIGN-CLS's
     `stage-receipt` default), then the classes guard red -> green;
   - Debug build of `atx-equity-strategy-ic,atx-impl-strategy-ic-tests` (next root tag) and `SignVerb*` = 6 / 6;
   - `test_wave_sign_verb.py` with `ATX_EQUITY_IC_EXE` = that exe: 21 passed, **0 skipped** (the 24-row identity
     against the Python rule);
   - `test_wave_driver.py` (28), `test_research_wave.py`, `test_no_python_mirror.py`.
2. **Root identity on the ledgered screen rows, by path**: `sign-logs/sign-identity-ledgered.py` (argv at report
   :133-140) on that tree, with `wave_rules.sign_pm7_35` present and the exe from step 1 (tag, SHA-256 and source stated,
   EXE-CUR; re-receipt after any SPDLOG relink), on `.superpowers/sdd/platform-v8-20260929/waves/{y-2,y-3,y-5,y-s}/
   wave-result.json`. Expected: IDENTICAL for each library wave and `RESULT PASS`. A refusal of a recorded "no
   admission row" row is the pre-SIGN rule change (finding 5), for the PM; any other difference is an identity miss
   and `50a43599` is not merged (C3-11).
3. **Only on PASS, merge `50a43599`** (4 files; blobs = `r10-deletion-tested-blobs.txt`):
   - re-run `test_wave_sign_verb.py` (real exe), `test_research_wave.py`, `test_no_python_mirror.py`;
   - the `sign_pm7_35` G-P5 row is gone and the count drops by exactly 1 function / 1 row (14 / 6 -> 13 / 5 on the
     lane's branch; root's absolute numbers differ after D3W-PB);
   - `FROZEN_PAIRS` still names `sign_pm7_35`.
   If root stops after step 1, the row still says `retired_by "SIGN"` (a P9 lane), and
   `test_rows_retired_by_a_p9_lane_are_gone_at_the_freeze` fails at `P9_FREEZE = True`. Under C3B-S root then relabels
   it to `P10`, with a comment: the label must be in `LANES`, so not the literal "P10 (SIGN not merged)".
4. **EXE-FREEZE**: the merge adds `strategy_sign_verb.cpp` to `atx-impl-strategy` and links
   `atx-engine-research-admission` PUBLIC into it.
   - The IC exe's bytes change (a new verb).
   - Every exe linking `atx-impl-strategy` relinks: `atx-equity-strategy`, `-targets` (NAV), `-risk`, `-mine`, and
     `atx-impl` through the pipeline. Re-receipt their SHA-256.
   - SIGN must merge before the wave-3 P9-B0 / re-pin, else P9-B0 and the re-pin are redone (EXE-FREEZE).
5. **Stretch**: merges only if APPROVE at the gate (C3-11, GATE-1); else P10 whole, with the Python copy as a named
   G-P5 exception. SIGN-SUBST: no substitution entry; the prereg 2b-ii names the receipt as a per-wave file the judge
   pins.
6. **Gate (TEST-1)**: whole `atx-impl-strategy-ic-tests`, Release build, `scripts/tests` seeds 0 / 1, the stage chain.

## Permission refusals

None.
