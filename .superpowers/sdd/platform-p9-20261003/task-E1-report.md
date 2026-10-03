# Lane E1 report

## Task 0

### Outcome
DONE: P0-FIX (plan section 1.2 "before" rows, brief-E1 task 0 (a)-(g)) is one Python-only commit; no exe output byte
moves; scripts/tests passes under PYTHONHASHSEED 0 and 1, atx-engine/tools and atx-impl/tools pass.

### Branch / SHA
`feat/p9-e1-20261003` @ **`3fa2dd4affe4b4de718d222435801b318ebf5cf0`** (task-0 code commit; root merges exactly this
SHA in R0-2). This report is a separate commit on top of it. Leased tree left clean.

### Frozen base / lease
base_sha=d7c1c520c3caa162ef453349b256669ce8de3c80; worktree=C:\atx-wt\pool-17; lease_name=pool-17;
lease_run_id=p9-e1-20261003; heartbeat owner (owner=alive); keeper_pid=17736; acquired 2026-10-03T10:08:46Z.
Leased by root (not by this lane); `lease-worktree.ps1 -Status` shows
`pool-17 [cold] branch=feat/p9-e1-20261003 LEASED run_id=p9-e1-20261003 | agent=p9-e1 | owner_kind=heartbeat |
owner=alive | keeper_pid=17736`.

### Files changed (18)
| file | what |
|---|---|
| `scripts/research_tree.py` | (a) `RUNNER_MAX_SECONDS = 600`; `seconds_cap_refusal(key, value)` (the one message, naming the key) |
| `scripts/run_bounded_research.py` | (a) `:92-95` checks `0 < args.seconds <= research_tree.RUNNER_MAX_SECONDS` (message `<=600 seconds` unchanged) |
| `scripts/wave_manifest.py` | (a) `marginal.seconds` above the max refused at load; (b) budget `admission_cycle_prefixes` (list, non-empty, distinct, texts) or the old `admission_cycle_prefix` string, exactly one with `admission_cap`; `PREFIX_KEYS`, `BUDGET_KEYS`, `budget_prefixes(b)` |
| `scripts/wave_steps.py` | (a) `marginal_ruled` raises ValueError for a cap above the max |
| `scripts/research_cycle.py` (P3: E1 owns it in wave 1) | (a) `validate_runner_phases(phases, seconds)` refuses `runner.seconds` / `runner.phases.<p>.seconds` above the max (exit 2 at load); (c) `CAPACITY_FLAG`, `CAPACITY_FILES`, `Cycle.capacity_missing`: `nav_step` (`:1185`) is done only when `summary.json` and, with `--capacity-curve` in the argv, `capacity_curve.csv` + `v7_extras.json` exist (else state failed, note names the missing files); (d) `ref_step` (`:1015-1019`) keeps the skip on equal fields only while the NAV exe's SHA-256 is not known to differ from the parent NAV's receipt; `Cycle.nav_exe_shas`; usage doc lines |
| `scripts/cycle_resume.py` | (d) `completed_exe_sha256(res, out, attempts)`: `executable_sha256` of the last completed (exit 0) receipt of `<out>-run`, `<out>-run<k>` |
| `scripts/wave_stage_preflight.py` | (b) `admission_used` counts `startswith(tuple(budget_prefixes(b)))`; `budget_check` echoes the manifest's own prefix key (string form byte-identical) |
| `scripts/wave_result.py` | (b) budget line prints every prefix; (d) `nav_exe` block; timings rows carry `executable_sha256`; doc |
| `scripts/wave_stage_util.py` | (d) phase rows keep `executable_sha256`; `run_rows`, `completed_exe` |
| `scripts/wave_stage_record.py` | (d) verify records `nav_exe {parent, cell, equal, ref}`; refuses only when the exes differ and the cell spec's ref did not run on the cell's exe |
| `scripts/wave_readers.py` | (c) `capacity_expected(summary)` (`v7.declarations.capacity_curve` true); `capacity_x4(..., expected)` raises SystemExit on a missing file or no single finite 4x row when expected; `book` passes it |
| `atx-impl/tools/backtest_integrity.py` | (e) `ledger_append` only: verify-and-append under `<ledger>.lock` (O_CREAT\|O_EXCL), waits `lock_seconds` (default 60) then ValueError naming the holder; lock removed in `finally`; appended bytes unchanged |
| `scripts/tests/test_research_spec.py` | (f) `NULL_PINS` / `ADD_ALPHA_COPIES` file lists deleted; `spec_kind` (template / base / add-alpha copy / generated) and `spec_null_pins` (rule by kind); `test_spec_kind_null_pins` |
| `scripts/tests/run_two_seeds.py` (new) | (g) the documented two-seed suite command |
| `scripts/tests/test_wave_hardening.py` (new) | the task-0 tests (below) |
| `scripts/tests/test_wave_speed.py` | (a) `:80-95` asserts `RT.RUNNER_MAX_SECONDS` / 600, not 720 |
| `scripts/tests/test_research_cycle.py` | fake NAV writes `capacity_curve.csv` + `v7_extras.json` with `--capacity-curve` (as the exe), behaviour `capacity-crash` |
| `scripts/tests/test_research_mine.py` | cross-lane edit, see below |

Tests named by the brief: `test_runner_max_refuses_720` (manifest, manifest load, spec, spec load, wave step, the
runner process), `test_budget_prefix_list_counts_v8ys`, `test_capacity_missing_is_not_done` (cycle run, resume,
reader), `test_exe_sha_in_phase_rows` (rows, verify record, fake wave's wave-result.json, forced ref run),
`test_ledger_append_lock_excl` (exact bytes, held lock, refusal inside the lock, 12 concurrent writers),
`test_spec_kind_null_pins` (in test_research_spec.py); plus `test_two_seed_suite_command`.

### Evidence
All runs on the working tree that is commit 3fa2dd4a (no edit after the runs started).

1. `"C:/Program Files/Python312/python.exe" scripts/tests/run_two_seeds.py` -> exit_code=0
   ```
   == PYTHONHASHSEED=0 -m pytest -q -p no:cacheprovider scripts/tests
   323 passed, 4 skipped in 366.54s (0:06:06)
   == PYTHONHASHSEED=0: exit 0
   == PYTHONHASHSEED=1 -m pytest -q -p no:cacheprovider scripts/tests
   323 passed, 4 skipped in 345.21s (0:05:45)
   == PYTHONHASHSEED=1: exit 0
   ```
2. `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-engine/tools` -> exit_code=0
   ```
   348 passed, 6 subtests passed in 213.97s (0:03:33)
   ```
3. `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools` -> exit_code=0
   ```
   625 passed, 2 skipped, 17 subtests passed in 251.55s (0:04:11)
   ```
4. `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_wave_hardening.py`
   -> exit_code=0: `6 passed in 6.11s`
5. `... -m pytest -q -p no:cacheprovider scripts/tests/test_research_spec.py -k test_spec_kind_null_pins` -> exit_code=0:
   `1 passed, 77 deselected in 1.14s`
6. F-9 equivalence (scratch script, not committed): the kind rule reproduces the deleted `NULL_PINS` list on all
   29 authored specs: `specs 29 old 29 diffs 0` (kinds: base-lo1, base-lo3 base; lib-v81-gm, lib-v8x3b-gm,
   lib-v8x7b-gm add-alpha copy; the 24 others template).
7. OR-5 discrimination (scratch script, not committed): the base d7c1c520 `ledger_append` with 12 concurrent writers
   breaks the hash chain in `broken trials: 10 of 10`; the locked one passes the same load in the test.
8. `wave plan` in pool-17 (it opens no output: pool-17's build-equity holds no NAV, fields or receipt of the wave):
   - `"C:/Program Files/Python312/python.exe" scripts/research_cycle.py wave plan scripts/specs/v8/waves/y-s.json`
     -> exit 2 (expected: the blocker is now caught at plan time):
     `research_cycle wave: wave manifest ...\y-s.json: marginal.seconds 720 is above the bounded runner's maximum
     600 s (research_tree.RUNNER_MAX_SECONDS; run_bounded_research.py refuses it)`
   - the same on a scratch copy with root's two amendments (`marginal.seconds` 600; `admission_cycle_prefixes`
     `["v8x", "v8ys"]`) -> exit_code=0, 57 plan lines, first `# stage 01 preflight: pending`.
   - `wave_manifest.validate(head + candidates)`: amended y-s.head.json `[]`; committed one refuses with the 720 line.

### How root verifies and amends (R0-2)
1. Merge exactly `3fa2dd4affe4b4de718d222435801b318ebf5cf0` (not this report commit).
2. Tests (repo root, `PY="C:/Program Files/Python312/python.exe"`):
   `$PY scripts/tests/run_two_seeds.py` (scripts/tests under seeds 0 and 1, exit 0 iff both pass);
   `$PY -m pytest -q -p no:cacheprovider atx-engine/tools`; `$PY -m pytest -q -p no:cacheprovider atx-impl/tools`.
   (Equivalent by hand: `PYTHONHASHSEED=0 $PY -m pytest -q -p no:cacheprovider scripts/tests`, then `=1`.)
3. Before amending: `$PY scripts/research_cycle.py wave plan scripts/specs/v8/waves/y-s.json` must exit 2 with
   `marginal.seconds 720 is above the bounded runner's maximum 600 s` (proves the merged check is live).
4. Amend both files (DEC-1, DEC-2), nothing else required:
   - `scripts/specs/v8/waves/y-s.json` `:41` and `y-s.head.json` `:17`: `"marginal": {..., "seconds": 600}` (was 720).
   - both: `"budget": {"id": "v8x-hand-25-plus-y-15", "admission_cap": 40, "admission_cycle_prefixes": ["v8x", "v8ys"],
     "construction_cap": 62}` (the key `admission_cycle_prefix` replaced by `admission_cycle_prefixes`; exactly one of
     the two may be present). The descriptions still say "a 720 s cap": root's wording to update, not checked.
   Commit both as the new pre-registration commit (preflight requires the manifest committed and clean).
5. `$PY scripts/research_cycle.py wave plan scripts/specs/v8/waves/y-s.json` -> exit 0. The preflight budget then
   counts admission lines of cycles `v8x*` and `v8ys*` (today 25 + 15 new = 40 <= 40; the Y lines are counted by
   the next wave).

Identity (no exe output byte moves; nothing here changes NAV, IC, fit, weights or fields bytes):
- Spec argv: unchanged for every spec except one case: a ref phase on fields equal to the parent's now runs when the
  NAV exe's SHA-256 differs from `executable_sha256` in the parent NAV's completed run receipt
  (`<reference_cell.dir>-run[<k>]/receipt.json`). Unknown either side keeps today's skip.
- Ledger lines: byte-identical (test pins the exact text); a transient `<ledger>.lock` exists during an append.
- Wave receipts: preflight's budget block byte-identical for the string form; phase rows gain `executable_sha256`;
  verify gains `nav_exe`; wave-result.json gains `nav_exe` and `executable_sha256` per timings row (receipts only).
- Intended behaviour changes: caps above 600 s refused at load; a NAV with `--capacity-curve` and a summary but no
  `capacity_curve.csv` / `v7_extras.json` is a failed attempt (never done); the book reader refuses a curve its
  summary declares but lacks.

### Deviations from brief
- (a) also refuses `runner.seconds` above the max (the base cap of every phase), not only `runner.phases`; same
  message shape. `--runner-override seconds=N` is not checked at parse (outside the named lines; the runner still
  refuses it with exit 2): open for task 1+.
- (d) verify also refuses one inconsistent case (exes differ, the cell spec has a ref phase, no completed ref run on
  the cell's exe), beyond recording; it cannot fire when research_cycle forced the ref as designed.
- (e) the lock code imports `os` / `time` inside `ledger_append` to keep the edit to that function only.
- (f) a `-gm` copy takes the kind of what it copies (template copy = template; hand copy of add-alpha's spec =
  "add-alpha copy", recognised from content: `lib-NAME-gm.json`, name `NAME-gm`, add-alpha's content for NAME).
- (g) the command is a small runner (`scripts/tests/run_two_seeds.py`, not collected by pytest) with the hand-run
  lines in its docstring.
- Task-0 tests live in a new `scripts/tests/test_wave_hardening.py` (all under the two-seed run), incl. the ledger
  lock test (through `research_ledger.backtest_integrity()`), to avoid touching B2's wave-2 test file.

### Cross-lane edits
- `scripts/tests/test_research_mine.py:131-135` (no lane owns it in wave 1): `test_constants_mirror_the_verb` grepped
  the literal `0 < args.seconds <= 600` in the runner; it now asserts `research_tree.RUNNER_MAX_SECONDS ==
  research_mine.RUNNER_MAX_SECONDS` and the runner's `0 < args.seconds <= research_tree.RUNNER_MAX_SECONDS`.
  `research_mine.py` keeps its own mirror constant (untouched).
- No `scripts/specs/**`, registration file, C++ or `atx-db/` file touched.

### Open risks
- Y-S forced ref: if `build-equity/bin/atx-equity-strategy-targets.exe` today differs from the exe in X-5's NAV
  receipt (`executable_sha256`), the Y-S cell's ref now runs (~45 s NAV) and its `ref-s2` compare must reproduce
  X-5's S2 daily byte for byte, else exit 4 (a PM8-4 (b) stop). Root can see it beforehand: compare the exe's SHA-256
  with that receipt's `executable_sha256` (a receipt, open).
- Capacity completeness on old outputs: a NAV dir with `--capacity-curve` that lacks `capacity_curve.csv` or
  `v7_extras.json` now reads as failed, and the book reader refuses a parent whose summary declares the curve but
  lacks the file. Root can check X-5's NAV dir by file names only (`ls`), no content read.
- A crashed ledger writer leaves `<ledger>.lock`; the next append waits 60 s, then refuses with the holder's pid
  (remove it after checking no writer is alive).

### Ledger candidates
- The unlocked `backtest_integrity.ledger_append` broke the hash chain in 10/10 trials with 12 concurrent writers;
  the O_EXCL `<ledger>.lock` (P9 E1 task 0, 3fa2dd4a) serialises them.
- The NAV verb writes `summary.json` before its capacity pass and `v7_extras.json`: a NAV with `--capacity-curve` is
  done only with `capacity_curve.csv` + `v7_extras.json` (research_cycle `CAPACITY_FILES`, P9 NV-4).
- Spec null pins by kind (`test_research_spec.spec_null_pins`) reproduce the deleted 29-entry `NULL_PINS` list exactly.
