# Handoff SIGN (pool-29, `feat/p9-sign-20261006`)

Lane SIGN (stretch; ruling C3-11): retire the Python PM7-35 sign rule (`scripts/wave_rules.py` `sign_pm7_35`,
`SIGN_RULES`, `screen_decision`) by a C++ verb of the IC exe over B1's header-only predicate `adm::pm7_35`. Spec:
`briefs/brief-SIGN.md` (root sprint dir). Merges only if APPROVE at the gate (GATE-1), else P10 whole.

## Head line

- R10 (RESUME 10) end: DONE_WITH_CONCERNS. Pre-deletion code head `8727f6d4` (root `f58e3a4c` merged); the records
  commit (report, this line, `sign-logs/` evidence) is its child; the deletion commit (`refactor(sign): retire the
  Python PM7-35 sign rule ...`) is the LAST commit above it (its SHA in the hand-back). Debug `p9-sign-a` (source
  `6d4b8bf9`): `SignVerb*` 6 / 6; lane identity 24 / 24 with the real exe; Python teeth 10 / 10; C++ teeth DEFERRED
  (HOST-4: a build was waiting). Everything is in `task-SIGN-report.md`. Next: the first review (PM); root's identity
  on the four ledgered screens before root merges the deletion commit. Object tree `build-equity` deleted by name
  after the deletion commit (receipts and logs kept; the tracked `audits/`, `v8-interim3-pitch-render-run2/` kept).
- STOP 9 state: head `422e9fad`, tree clean, three `WIP(sign)` commits above the merges of C3b `76b9f256`
  (`749d2c32`) and root `fcb0200e` (`9c360cde`). No handoff, no report, no log existed; `sign-logs/` held the four
  tools only. Nothing was ever compiled, built or run in C++; the pytest count in `422e9fad`'s body (19 passed,
  2 skipped) has no log behind it.
- RESUME 10 (SIGN-RESUME): this file is the first act, reconstructed from the three commit bodies,
  `git diff --stat 9c360cde..422e9fad` (11 files, +1232 / -8) and the four tools.

## What exists, per commit (file:line at `422e9fad`)

### `6800d077` WIP(sign): atx-equity-strategy-ic sign verb over adm::pm7_35 (not yet compiled)

- `atx-impl/src/strategy_sign_verb.hpp:1-37`: the verb contract in the header comment (argv
  `sign --rule pm7-35 --rows FILE --output NEWFILE`; the rows schema `{id, kind add|replace, prior_sign 1|-1, status,
  runner_sign -1|0|1}`, other keys ignored; exit 0 / 1 refused before any output / 2 usage); `kSignReceiptSchema =
  "atx.sign-receipt/v1"` (:33, a NEW schema literal); `dispatch_sign` (:36).
- `atx-impl/src/strategy_sign_verb.cpp` (371 lines):
  - `:34-35` limits (16 MiB rows file, 20000 rows); `:39-43` `kWaveSignRules` (the one wave id `pm7-35` ->
    `adm::kPm735SignRule`); `:46-51` `kKindNames` (`add` / `replace` -> `adm::CandidateKind`).
  - `:99-109` `small_integer` (integer only, unsigned range-checked before narrowing); `:111-114` `non_empty_string`.
  - `:118-151` `parse_row` (refusals: not an object, id, kind, unknown kind, prior_sign 1 / -1, no admission row
    = status or runner_sign absent "a stop, not a drop", status non-empty string, runner_sign -1 / 0 / 1).
  - `:153-174` `parse_rows` (JSON object, non-empty `rows` array, row limit, duplicate id).
  - `:177-193` `read_rows_file` (open, size limit, read).
  - `:195-197` `admitted` = `status == adm::status_name(Status::Admitted)` (screen.hpp; links `status_name` from the
    admission library).
  - `:216-236` `reason_of`: the words of `wave_rules.sign_pm7_35` keyed by the predicate's verdict (no second
    decision).
  - `:238-265` `receipt_json` (the only place a verdict is made: `adm::pm7_35(cfg.rule.predicate, kind, admitted,
    prior, runner)` at :244); keys `schema, rule, sign_rule, predicate, rows_file, rows_sha256, rows[{id, kind,
    prior_sign, status, runner_sign, verdict, reason}], kept, dropped`.
  - `:268-278` `write_receipt` (removes a partial file on a failed write); `:280-293` `run_sign` (output must not
    exist; read; SHA-256; parse; write).
  - `:309-343` `parse_args` (`--help`; unknown / repeated / value-less flag; unknown rule id; required flags);
    `:346-370` `dispatch_sign` (exit codes; catch-all for allocation / JSON exceptions).
- `atx-impl/tools/equity_strategy_ic.cpp:5` (include), `:16` (the verb-table line `{"sign", dispatch_sign}`).
- `atx-impl/CMakeLists.txt:302-306` (CMake tail append): `strategy_sign_verb.cpp` into `atx-impl-strategy`; that
  library now links `atx::engine-research-admission` PUBLIC (`atx-engine/CMakeLists.txt:373`, EXCLUDE_FROM_ALL).

### `558c4ed0` WIP(sign): SignVerb gtests (not yet built)

- `atx-impl/tests/strategy_sign_verb_test.cpp` (356 lines), six tests, named filter `SignVerb*`:
  `SignVerb.DecisionTableThroughTheVerb` (:114; the 12 rows of `research_admission_sign_test.cpp`'s wave column
  written out at :96-109, at prior +1 and mirrored at -1, 24 rows; verdicts, echo, kept / dropped, console line),
  `ReasonsAreTheWaveRulesWords` (:147; six reasons word for word), `ReceiptPinsTheRowsFileAndIgnoresOtherKeys`
  (:177; schema, rule, sign_rule, rows_file, rows_sha256 of the bytes; a recorded `decision` is ignored),
  `RefusesMalformedRowsBeforeAnyOutput` (:290; 27 malformed files, :235-287), `RefusesAMissingRowsFileAndAnExisting
  Output` (:305), `UsageErrorsWriteNothing` (:327).
- `atx-impl/tests/CMakeLists.txt:210-213` (tail append): the TU into `atx-impl-strategy-ic-tests` (and
  `atx-impl-tests` through its glob).

### `422e9fad` WIP(sign): the screen stage calls the IC exe's sign verb; identity pytest

- `scripts/wave_rules.py:82-127`: `SIGN_RECEIPT` (:85), `SIGN_ECHO` (:86), `sign_rows` (:89; the verb's input rows
  from the manifest candidates and the admission rows; a missing admission row is a `RuleError`), `verb_decision`
  (:102; reads the receipt and refuses one that is not the verb's answer to exactly these rows: schema, rule,
  rows SHA-256, count, echo / order, verdict, kept / dropped). `sign_pm7_35` (:39), `SIGN_RULES` (:61) and
  `screen_decision` (:64) are still defined (kept until the deletion commit).
- `scripts/wave_stage_library.py`: `screen` :119 (`WR.sign_rows`), :122 (THE call line `sign_verb(...)`), :132
  (`"sign": sign` recorded in the screen receipt); `screen_plan` :138-145 (the plan prints the verb argv);
  `SIGN_DIR` :149; `sign_exe` :152 (the spec's effective `exes.ic`, held to `exes_sha256.ic` when pinned, exit 3);
  `sign_verb` :165 (rows-k / receipt-k, k the first free number; `w.run`; `WR.verb_decision`, exit 3);
  `spec_inputs` :274-281 (the spec stage pins `screen.sign.receipt_sha256`; a screen recorded before SIGN has no
  sign block and keeps the inputs of before).
- `scripts/tests/wave_fixture.py`: `FakeCycle.__call__` :237 (routes `<exe> sign`), `FakeCycle.sign` :374 (the fake
  verb: same receipt layout), `SIGN_ECHO` :426, `sign_oracle` :429 (test oracle).
- `scripts/tests/test_wave_sign_verb.py` (305 lines): `COMBOS` :53 (24 = 2 statuses x 3 runner signs x 2 kinds x 2
  priors), `PINNED` :56; tests: combinations cover the Python test and the table (:78), pinned column and fake =
  Python rule (:84), REAL-EXE identity on the 24 (:93, skipped when the exe is absent; `ATX_EQUITY_IC_EXE` or
  `<repo>/build-equity/bin/atx-equity-strategy-ic.exe`, :44), REAL-EXE refusals (:113), the stage on fakes (:145,
  :178, :187, :229 x9 tamper cases, :238, :252, :265), route absent (:285 ledgered v1 manifests validate; :299 older
  screen inputs).
- `scripts/tests/test_no_python_mirror.py:37` (`P9_LANES += "SIGN"`), `:64-66` (the `sign_pm7_35` G-P5 row relabelled
  `retired_by` `"SIGN"`, producer named); `FROZEN_PAIRS` :91 already names `sign_pm7_35` (the name stays detected after
  the deletion).

## Tools in `sign-logs/` (uncommitted lane tools; sprint dir is git-ignored)

- `sign-teeth-py.py`: ten Python mutations P1-P10 (`verb_decision` checks P1-P6, `sign_rows` P7, `sign_exe` pin P8,
  spec-stage receipt pin P9, the stage back on the Python rule P10), each restored by the tool, SHA / blob / porcelain
  asserted (TEETH-RESTORE).
- `sign-teeth-cpp.py`: 21 C++ mutations C1-C21 of `strategy_sign_verb.cpp` applied at once, ONE build (`-Tag`),
  `SignVerb*`, restore in `finally`, bytes / blob / porcelain asserted; `--check` verifies every needle is found once.
- `sign-deletion.py`: the LAST commit's edits (deletes `sign_pm7_35`, `screen_decision`; `SIGN_RULES` becomes the
  tuple `("pm7-35",)`; the G-P5 row deleted with a comment; `test_research_wave.py::test_sign_rule_pm7_35_as_ruled`
  and three `test_wave_sign_verb.py` tests moved off the Python rule).
- `sign-identity-ledgered.py`: root's identity on the ledgered wave results' screen rows (the verb vs the recorded
  decision and, while present, `sign_pm7_35`), before root merges the deletion commit.

## Untested at `422e9fad`

Everything in C++: no single-TU check, no build, no gtest. Python: no logged run; the real-exe cases never ran (no
exe); no teeth run (Python or C++); the deletion tool never run; the identity tool never run.

## Missing against the brief

1. Merge of root's current head (after `fcb0200e`).
2. Single-TU checks of `strategy_sign_verb.cpp`, `strategy_sign_verb_test.cpp`, `equity_strategy_ic.cpp`; ONE Debug
   build `p9-sign-a` of `atx-equity-strategy-ic,atx-impl-strategy-ic-tests`; `SignVerb*`.
3. Pytest (TEST-1): `test_wave_sign_verb.py` with the real exe; the wave files; `test_no_python_mirror.py`.
4. Teeth: Python (`sign-teeth-py.py`), C++ (`sign-teeth-cpp.py`, build `p9-sign-b`, iff no build is waiting).
5. `task-SIGN-report.md` (exe and key; verb -> file:line -> tests -> teeth; identity procedure for root by path;
   what root re-proves; pre-deletion head). New literal `atx.sign-receipt/v1` needs root's `classes.json` row
   (SQL2-CLS).
6. The deletion commit LAST (`sign-deletion.py`), then the object tree deleted by name.
7. Deferred to the gate (TEST-1): Release build, whole `atx-impl-strategy-ic-tests`, whole `scripts/tests` seeds 0 / 1,
   the stage chain.

## Next step for a fresh agent

Follow "Missing against the brief" in order; the exact argv are in each tool's docstring.
