# Task SIGN report (lane report)

Lane SIGN (stretch; ruling C3-11), pool-29, branch `feat/p9-sign-20261006`. Spec: `briefs/brief-SIGN.md`. Rounds:
R9 (three `WIP(sign)` commits `6800d077`, `558c4ed0`, `422e9fad`; never compiled, no record) and R10 (SIGN-RESUME:
this report). Rulings: C3-11, C3B-S, SIGN-RESUME, W1-I2, B2-10 / B2-11, G-P5, EXE-CUR, SPDLOG-RELINK, TEST-1,
TEETH-RESTORE, HOST-4, HOST-5, SQL2-CLS, PERM-1. Stretch: merges only if APPROVE at the gate (GATE-1), else P10 whole
and the Python copy stays a named G-P5 exception (root relabels the row "P10 (SIGN not merged)", C3B-S).

Outcome: DONE_WITH_CONCERNS. The verb, its gtests, the stage call, the lane identity (24 / 24 with the real exe) and
the Python teeth (10 / 10) are done and green; the deletion of `sign_pm7_35` is the LAST commit. Concerns: the ONE C++
teeth build was deferred (HOST-4: a build was waiting), and one cross-lane test hunk (`test_wave_driver.py`) was
needed. Root merged last: `f58e3a4c` (merge `8727f6d4`; one CMake tail region, keep-both).

## Commits of the lane (first parent, R10)

| commit | what |
|---|---|
| `6800d077`, `558c4ed0`, `422e9fad` | R9 WIP: the verb, the gtests, the stage call + identity pytest (see `handoff-SIGN.md`) |
| `e648e85a` | handoff reconstructed from the WIP commits (SIGN-RESUME first act) + the four lane tools |
| `166df1aa` | merge of root `b182dfdd` (C3b in root; docs only for the lane, no conflict) |
| `7c7e793e` | fix: the sticky-pins wave world pins its NAV exe only; `sign_exe` names a missing pinned IC exe (+ test case) |
| `136c4161`, `30816e9c` | the Python teeth tool's P7 needle and needle pre-check; the HOST-4 / HOST-5 wait script |
| `6d4b8bf9` | fix: the gtests' result struct `Run` -> `VerbRun` (it hid `testing::Test::Run`: 13 errors at the first check) |
| `8727f6d4` | merge of root `f58e3a4c` (D3W, ABS); conflict `atx-impl/CMakeLists.txt` tail: keep-both, ABS block first |
| records commit (child of `8727f6d4`) | this report, the handoff's head line, `sign-logs/` evidence |
| deletion commit (LAST) | `sign_pm7_35`, `screen_decision` and the G-P5 row deleted (`sign-deletion.py`) |

## Exe and key chosen, and why

- **Exe `atx-equity-strategy-ic` (the IC exe), key `exes.ic`; verb `sign`** (a third entry of the exe's verb table
  beside `ic` and `marginal`, `atx-impl/tools/equity_strategy_ic.cpp:16`).
- Why: the screen stage's only process is `research_cycle run <library spec> --screen`, the IC pass, so every screen
  spec names `exes.ic` (`load_spec` refuses a spec with an `ic` section and no `exes.ic`) and `lock --exes` pins it in
  `exes_sha256.ic`; research_cycle's `verify_exes` (`scripts/cycle/phases.py:273-287`) holds it to the pin. The engine
  eval verb (`exes.eval`) is pinned only for v2 manifests and is the judge's (B2-10 / B2-11), so a v1 screen has no
  such key. The IC exe is therefore the one exe the screen-stage wave already pins in every wave, v1 and v2: no new
  pinned-exe key (W1-I2 and B2-10 unchanged). The decision lives in the engine (`adm::pm7_35`, B1's header-only
  predicate), the verb in `atx-impl-strategy`: the owner's C++ centralization.
- The verb holds no decision of its own: every verdict is `adm::pm7_35(kPm735SignRule, kind, admitted, prior,
  runner)` (`strategy_sign_verb.cpp:244`); the reason is a description keyed by that verdict (`reason_of`, :216-236).

## Items: verb -> file:line -> tests -> teeth

| item | change (file:line at the pre-deletion head) | tests | teeth |
|---|---|---|---|
| verb contract | `atx-impl/src/strategy_sign_verb.hpp:1-37` (argv, rows schema, exits; `kSignReceiptSchema` :33) | `SignVerb.*` (6) | C1-C21 |
| rule ids | `strategy_sign_verb.cpp:39-43` (`pm7-35` -> `adm::kPm735SignRule`), `parse_args` :323-329 | `UsageErrorsWriteNothing` (:327) | C1 |
| rows parsing / refusals | `parse_row` :118-151, `parse_rows` :153-174, `read_rows_file` :177-193, `small_integer` :99-109 | `RefusesMalformedRowsBeforeAnyOutput` (:290; 27 files), `RefusesAMissingRowsFileAndAnExistingOutput` (:305) | C2-C17 |
| decision | `receipt_json` :238-265 (`adm::pm7_35` at :244), `admitted` :195-197 (`status_name(Status::Admitted)`) | `DecisionTableThroughTheVerb` (:114; the 12 rows of `research_admission_sign_test.cpp`'s wave column at prior +1 and mirrored at -1), `ReasonsAreTheWaveRulesWords` (:147) | (the decision is the engine's; identity below) |
| receipt | `receipt_json` keys; `write_receipt` :268-278; `run_sign` :280-293 (output exclusive) | `ReceiptPinsTheRowsFileAndIgnoresOtherKeys` (:177) | C18 |
| usage | `parse_args` :309-343, `dispatch_sign` :346-370 | `UsageErrorsWriteNothing` | C19-C21 |
| verb table | `atx-impl/tools/equity_strategy_ic.cpp:5`, `:16` | real-exe pytest (below) | - |
| CMake tail appends | `atx-impl/CMakeLists.txt:302-306` (TU in `atx-impl-strategy`; links `atx::engine-research-admission` PUBLIC); `atx-impl/tests/CMakeLists.txt:210-213` (TU in `atx-impl-strategy-ic-tests`; `atx-impl-tests` by glob) | build `p9-sign-a` | - |
| the stage's call line | `scripts/wave_stage_library.py:119` (`WR.sign_rows`), `:122` (THE call: `sign_verb(...)`), `:132` (`"sign"` in the screen receipt); `screen_plan` :138-145; `sign_exe` :152-164 (exes.ic held to its pin; a missing pinned exe refused); `sign_verb` :166-186; `spec_inputs` :275-282 (pins `screen.sign.receipt_sha256`) | `test_wave_sign_verb.py` :145, :178, :187, :229 (x9), :238, :252, :265 | P8, P9, P10 |
| receipt reading | `scripts/wave_rules.py:82-127` (`SIGN_RECEIPT`, `SIGN_ECHO`, `sign_rows`, `verb_decision`) | :178, :229 (x9) | P1-P7 |
| fake verb | `scripts/tests/wave_fixture.py:237`, `:374-393` (`FakeCycle.sign`), `:426-446` (`sign_oracle`) | held to the real exe: :93 | - |
| identity (lane) | `scripts/tests/test_wave_sign_verb.py:49-56` (`COMBOS` 24, `PINNED`), :78, :84, :93 (REAL exe vs `sign_pm7_35` vs fake vs pinned), :113 (REAL refusals) | see "Builds and tests" | - |
| route absent | `test_wave_sign_verb.py:291` (ledgered v1 manifests validate, name `pm7-35`), :305 (a screen recorded before SIGN keeps the spec stage's inputs) | | - |
| G-P5 | `scripts/tests/test_no_python_mirror.py:37` (`P9_LANES` += `SIGN`), `:64-66` (row `retired_by` `SIGN`), `FROZEN_PAIRS` :91 (already names `sign_pm7_35`: a reintroduction stays detected); the row is deleted in the deletion commit | `test_no_python_mirror.py` 7 | - |
| deletion (LAST commit) | `sign-logs/sign-deletion.py`: deletes `sign_pm7_35`, `screen_decision` (`wave_rules.py:38-79`), `SIGN_RULES` -> `("pm7-35",)`, the G-P5 row; moves `test_research_wave.py::test_sign_rule_pm7_35_as_ruled` and three `test_wave_sign_verb.py` tests off the Python rule | post-deletion runs below | - |

## Builds and tests (logs in `sign-logs/`; receipts in `build-equity/`)

| run | head / exe | result | log |
|---|---|---|---|
| configure `equity-dev` | `30816e9c` | exit 0 (`engine_git_sha` 30816e9c) | `r10-configure-equity-dev.log` |
| check `strategy_sign_verb.cpp` | `30816e9c` | exit 0 | `r10-check-strategy_sign_verb.log` |
| check `strategy_sign_verb_test.cpp` | `30816e9c` | exit 1: 13 errors, `Run` inside a TEST body names the private `testing::Test::Run` (slip) | `r10-check-strategy_sign_verb_test.log` |
| the same, after `6d4b8bf9` | `6d4b8bf9` | exit 0 (the object of both `atx-impl-tests` and `atx-impl-strategy-ic-tests`) | `r10-check-strategy_sign_verb_test-2.log` |
| check `equity_strategy_ic.cpp` | `30816e9c` | exit 0 | `r10-check-equity_strategy_ic.log` |
| Debug `p9-sign-a`: `atx-equity-strategy-ic,atx-impl-strategy-ic-tests` (HOST-4 / HOST-5 wait: `r10-build-wait-{a..d}.log`) | source `6d4b8bf9`, dirty 0, 3 jobs, 131 TUs, 7 links, 309 s | exit 0, no warning from a lane TU; IC exe `76dbd261e3162b9d0d86d0f5e916db8c068f09fcb74c5712c6a5889f9cbed1bd`, tests exe `514e2ff16146f2df3e27e7e5fb2f71ac738cea6c640aadf89ecbd2362c91550f` (re-hashed = receipt) | `build-equity/mega-p9-sign-a-{receipt.json,build.log}`, `r10-build-a.out`, `r10-exe-sha-a.txt` |
| `SignVerb*` | p9-sign-a tests exe | 6 / 6 passed | `r10-gtest-a-signverb.log` |
| the wave pytest files on fakes, seed 0: `test_wave_sign_verb`, `test_research_wave`, `test_wave_driver`, `test_wave_gross_match`, `test_wave_review`, `test_wave_review_minor`, `test_wave_scoreboard`, `test_wave_speed`, `test_wave_hardening`, `test_wave_queue`, `test_wave_eval_verb`, `test_wave_kinds`, `test_research_store_hook` (every file that runs `wave_fixture`) | `166df1aa` (code `422e9fad`) | 253 passed / 1 failed / 3 skipped (the 2 real-exe cases, no exe yet; the store real-exe case). The red: `test_wave_driver.py::test_lock_exes_pins_and_verify_compares` (the screen refused the sticky-pins world's IC pin: no exe on disk); fixed in `7c7e793e` (cross-lane section) | `r10-pyt-wave-files-s0.log` |
| first fix attempt (a `bin/ic.exe` in that world), rejected | working tree | 27 passed / 1 failed: ruling W1-I1's cache bound refused at preflight, as the fixture's note says | `r10-pyt-driver-fix-s0.log` |
| the fixed node | `7c7e793e` | 1 passed | `r10-pyt-driver-fix2-s0.log` |
| `test_no_python_mirror.py` (own invocation) | `166df1aa` | 7 passed; G-P5 14 functions in 6 rows | `r10-pyt-mirror-pre-s0.log` |
| classes guard `atx-engine/tools/test_research_store_classes.py` (own invocation) | `166df1aa` | 1 failed / 4 passed / 96 subtests: only `atx.sign-receipt/v1` unregistered (SQL2-CLS: root's row) | `r10-pyt-classes-guard.log` |
| `test_wave_sign_verb.py` + `test_research_wave.py` with the REAL exe (`ATX_EQUITY_IC_EXE` = p9-sign-a IC exe) | `6d4b8bf9` | 40 passed (21 + 19), 0 skipped: identity 24 / 24 and the real refusals ran | `r10-pyt-signverb-real-a.log` |
| `test_wave_sign_verb.py` real exe; `test_no_python_mirror.py` | `8727f6d4` (root `f58e3a4c` merged) | 21 passed; 7 passed (G-P5 14 / 6) | `r10-pyt-signverb-merged-pre.log`, `r10-pyt-mirror-merged-pre.log` |
| post-deletion: `test_wave_sign_verb.py` + `test_research_wave.py` (real exe); `test_no_python_mirror.py` | `8727f6d4` + the deletion edits (the deletion commit's exact blobs, `r10-deletion-tested-blobs.txt`) | 40 passed; 7 passed, G-P5 13 functions in 5 rows | `r10-pyt-post-deletion.log`, `r10-pyt-mirror-post-deletion.log` |
| whole `atx-impl-strategy-ic-tests`, Release build, whole `scripts/tests` seeds 0 / 1, stage chain | - | deferred to the gate (TEST-1) | |

EXE-CUR: the p9-sign-a exes are built from `6d4b8bf9`. The root merge `8727f6d4` changed 27 C++ / CMake files of the
IC targets (D3W, ABS) but none of the verb's sources or includes (`git diff --stat 6d4b8bf9 8727f6d4` over
`strategy_sign_verb.*`, `strategy_sign_verb_test.cpp`, `equity_strategy_ic.cpp`, `research/admission/**`, `atx-core`
is empty), so the verb's evidence stands; the exes are NOT current for the whole IC targets at `8727f6d4` and root
rebuilds them at merge (ROOT-1a). No second lane build (TEST-1: one per round).

## Teeth

- **Python, by file (TEETH-RESTORE): 10 / 10 caught.** `sign-logs/sign-teeth-py.py` at `136c4161` (code
  `7c7e793e`): P1-P6 the receipt checks of `verb_decision`, P7 `sign_rows`' missing-row refusal, P8 `sign_exe`'s pin
  check, P9 the spec stage's receipt pin, P10 the stage put back on the Python rule; each mutation alone, its own
  node(s) red (pytest exit 1, every listed node FAILED), the file restored by the tool (bytes and `git hash-object` ==
  HEAD blob), `git status --porcelain` empty at the end. Log `sign-logs/r10-teeth-py.log`. The tool's first run
  stopped at P7 (its needle matched `sign_rows` and `screen_decision`), after P1-P6 had run and restored (porcelain
  empty; that run wrote no log); the needle was made unique and every needle is now checked before any write
  (`136c4161`).
- **C++: deferred (HOST-4).** At 21:10 the A4 reviewer's build / teeth tool (pool-27, `review-f-teeth.py`) was waiting
  (`sign-logs/r10-teeth-cpp-decision.txt`), so the one teeth build `p9-sign-b` was not started. The tool is ready:
  `sign-teeth-cpp.py --check` = 21 mutations, every needle found exactly once (the verb TU is unchanged since). For
  the reviewer's one build: `"C:/Program Files/Python312/python.exe" sign-logs/sign-teeth-cpp.py --root <pool> --tag
  <tag> --log <log>` (C1-C21 at once, one build of `atx-impl-strategy-ic-tests`, `SignVerb*`, restore in `finally`,
  bytes / blob / porcelain asserted; expected `RESULT PASS` 21 / 21: each mutation's markers in the failure output).

## Identity

### Lane (on fakes + the fixture rows, before the deletion)

`test_wave_sign_verb.py::test_the_verb_equals_the_python_rule_on_every_enumerated_combination` runs the REAL exe on
the 24 rows of `COMBOS` = {admitted, reject_redundant} x {1, 0, -1} x {add, replace} x {+1, -1} (a superset of the
eight cases of `test_research_wave.py::test_sign_rule_pm7_35_as_ruled` and of the 12-row decision table of
`research_admission_sign_test.cpp`, checked by :78) and asserts, row for row, (verdict, reason) of the receipt ==
`wave_rules.sign_pm7_35` == `wave_fixture.sign_oracle`, the verdict == the pinned column, the echo == the input and
the receipt's SHA-256 == the rows file's. Result: PASSED with the p9-sign-a IC exe (`76dbd261...`) at `6d4b8bf9` and
again at `8727f6d4` (`r10-pyt-signverb-real-a.log`, `r10-pyt-signverb-merged-pre.log`): 24 / 24 rows identical, kept
12 / dropped 12. After the deletion the same test holds the verb to the fake and the pinned column (the Python rule is
gone; `test_the_fake_verb_is_the_pinned_column` asserts it is): 21 passed on the deletion's bytes.

### Root (the ledgered wave results' screen rows; before root merges the deletion commit)

Root opens these files; the lane did not (they carry no statistic, but they are root's to read):

- `.superpowers/sdd/platform-v8-20260929/waves/y-2/wave-result.json`
- `.superpowers/sdd/platform-v8-20260929/waves/y-3/wave-result.json`
- `.superpowers/sdd/platform-v8-20260929/waves/y-5/wave-result.json`
- `.superpowers/sdd/platform-v8-20260929/waves/y-s/wave-result.json`

(all four tracked; `git diff --stat` of the lane over `.superpowers/sdd/platform-v8-20260929/waves` and `scripts/specs`
is empty). Procedure, on the tree at the pre-deletion head merged into root, with an IC exe built from it (EXE-CUR:
tag, SHA-256 and source commit stated; SPDLOG-RELINK: re-receipt an exe relinked since its receipt):

```
"C:/Program Files/Python312/python.exe" C:/atx-wt/pool-29/.superpowers/sdd/platform-p9-20261003/sign-logs/sign-identity-ledgered.py \
    --root <tree> --exe <tree>/build-equity/bin/atx-equity-strategy-ic.exe --work <a new dir> \
    <tree>/.superpowers/sdd/platform-v8-20260929/waves/y-2/wave-result.json \
    <tree>/.superpowers/sdd/platform-v8-20260929/waves/y-3/wave-result.json \
    <tree>/.superpowers/sdd/platform-v8-20260929/waves/y-5/wave-result.json \
    <tree>/.superpowers/sdd/platform-v8-20260929/waves/y-s/wave-result.json
```

For each library wave the tool writes `{rows: screen.rows}` as they stand (the verb ignores the recorded `decision` /
`reason` keys), runs `<exe> sign --rule <screen.sign_rule> --rows .. --output ..`, and compares every row's (verdict,
reason) with the recorded (decision, reason) and with `wave_rules.sign_pm7_35` on the same row (while it exists), and
kept / dropped with the recorded lists; a rule wave (screen null) is listed and skipped. It prints counts and ids
only. Expected: `IDENTICAL` per library wave, `RESULT PASS`, exit 0. A difference, or a refusal (for example a
recorded row whose `runner_sign` is not an integer -1 / 0 / 1: the engine's admission writes it as an `i32`, so none is
expected), is an identity miss: the deletion commit is not merged (C3-11). The decision table of
`research_admission_sign_test.cpp` row for row is `SignVerb.DecisionTableThroughTheVerb` plus the lane identity above.

## Route absent (flag-absent identity) and new names

- No flag. The screen stage always calls the verb (the brief: "the Python `sign_pm7_35` is called no more").
- Ledgered v1 manifests still validate and name `pm7-35` (test :291); their results are not touched (diff above).
- A screen recorded before SIGN (no `sign` block) keeps the spec stage's inputs of before (test :305).
- `wave-result.json`'s screen layout is unchanged: keys `spec, gate_exit, rows, kept, dropped, sign_rule`; rows
  `id, kind, prior_sign, status, runner_sign, decision, reason`; the reasons are the Python rule's words (test :145).
- What a NEW run adds, by design: `out/waves/<w>/sign/rows-<k>.json` and `receipt-<k>.json`; key `sign` = `{exe,
  exe_sha256, rows, rows_sha256, receipt, receipt_sha256}` in the screen receipt (`03-screen.json` outputs); the spec
  stage's input `screen.sign.receipt_sha256` (`04-spec.json`); the plan's screen line names the verb argv. A re-run of
  a wave therefore differs from its pre-SIGN receipts only by these keys and files (substitution list for root).
- New literal: `atx.sign-receipt/v1` (`strategy_sign_verb.hpp:33`, `wave_rules.py:85`). SQL2-CLS: root adds its
  class row at merge; until then `atx-engine/tools/test_research_store_classes.py` is red on exactly this literal
  (run in R10: 1 failed / 4 passed / 96 subtests, the one unregistered literal named in three files,
  `sign-logs/r10-pyt-classes-guard.log`). Suggested: a wave class beside `wave-reader` (path
  `out/waves/*/sign/receipt-*.json`); root rules.
- New flags (verb only): `sign --rule --rows --output --help`. New JSON keys: the receipt's `schema, rule, sign_rule,
  predicate, rows_file, rows_sha256, rows[], kept, dropped`; the screen receipt's `sign`. No rule id: `pm7-35` exists.

## Cross-lane edits (smallest possible; listed by file and hunk)

- `scripts/tests/test_wave_driver.py`, one hunk in `test_lock_exes_pins_and_verify_compares` (:1077-1081, R10 commit
  `7c7e793e`): the sticky-pins world's parent pins `{"nav": nav}` instead of `{"ic": ic, "nav": nav}`, with a
  three-line comment. Cause: the fake world has no exe on disk by design (`wave_fixture.pin_eval_verb`'s note: an exe
  on disk turns on ruling W1-I1's cache bound; R10 tried writing `bin/ic.exe` and hit exactly that refusal), and the
  screen now holds the IC exe to its pin (a missing pinned exe refused, as research_cycle's `verify_exes` does). The
  case still checks what it checked (a parent that pins its exes implies `driver.lock_exes`).
- `scripts/tests/wave_fixture.py` (shared fixture): the `sign` route of `FakeCycle.__call__` (:237), `FakeCycle.sign`,
  `SIGN_ECHO`, `sign_oracle`; docstring line 3.
- `scripts/tests/test_no_python_mirror.py` (G-P5): `P9_LANES`, the row relabel (`422e9fad`), the row deleted with a
  comment (deletion commit).
- `scripts/tests/test_research_wave.py`: `test_sign_rule_pm7_35_as_ruled` moved onto the fake verb + `sign_rows` +
  `verb_decision` (deletion commit).
- CMake tail appends: the two above.

## What root re-proves at merge (ROOT-1a) and at the gate (TEST-1)

1. At merge of the pre-deletion head: SQL2-CLS row for `atx.sign-receipt/v1` (one commit; the classes guard before /
   after); a Debug build of `atx-equity-strategy-ic,atx-impl-strategy-ic-tests` (next root tag) and `SignVerb*` = 6 / 6;
   `test_wave_sign_verb.py` with `ATX_EQUITY_IC_EXE` = root's exe (no skip: 21 passed), `test_wave_driver.py`,
   `test_research_wave.py`, `test_no_python_mirror.py`; then the ROOT identity above on the four ledgered results.
2. Only if the identity is `RESULT PASS`: merge the deletion commit; `test_wave_sign_verb.py`, `test_research_wave.py`,
   `test_no_python_mirror.py` again (G-P5 count drops by one function / one row).
3. At the gate (deferred to the gate, TEST-1): whole `atx-impl-strategy-ic-tests`, the Release build, the whole
   `scripts/tests` seeds 0 / 1, the stage chain.
4. C3B-S: if SIGN does not land, root relabels the row "P10 (SIGN not merged)" before the freeze.

## Pre-deletion head and the deletion commit

- **Pre-deletion head**: code `8727f6d4` (root `f58e3a4c` merged); the records commit that adds this report is its
  child and changes only sprint-dir records, so its tree's code is `8727f6d4`'s. Root's identity runs there.
- **Deletion commit**: the LAST commit of the branch, `refactor(sign): retire the Python PM7-35 sign rule
  (sign_pm7_35) and its G-P5 row`, the child of the records commit; its SHA is in the hand-back (it cannot name itself
  here without another commit above it). Its four files are byte for byte the tested ones
  (`r10-deletion-tested-blobs.txt`: `wave_rules.py` `71a23ba3`, `test_no_python_mirror.py` `9c6e3252`,
  `test_research_wave.py` `4c1c8393`, `test_wave_sign_verb.py` `2b60956e`); root merges it only after the identity
  passes.

## Blindness

No 2020-2023 statistic was opened on purpose. One incident, disclosed: a ledger grep for the strings "Y-2|Y-S" (looking
for the ledgered wave-result paths) printed the first 300 characters of five ledger lines, one of which (R0-6) carries
Y-S's verdict numbers; nothing of it was used or recorded, and the paths were then taken from `git ls-files` names
only. No file dated 2024-01-01 or later was opened; `holdout_gate.py` was not run; the trial ledger was not opened.

## Permission refusals

None.

## Open questions (one line each)

- SQL2-CLS: which class takes `atx.sign-receipt/v1` (a new wave class, or `stage-receipt`)? Root's row at merge.
- The C++ teeth (21 mutations) are deferred: the reviewer's one build, or root's?
- Is the `test_wave_driver.py` hunk (the sticky-pins world pins its NAV exe only) the accepted form, rather than a
  fixture IC exe (which trips W1-I1 in the fake world)?
- A re-run of a v1 wave now writes `sign/rows-k.json`, `receipt-k.json` and the receipts' `sign` keys: confirm these
  belong on root's substitution list (the brief's route is always on; no flag).
