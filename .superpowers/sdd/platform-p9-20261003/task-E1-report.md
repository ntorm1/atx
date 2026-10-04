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

## Tasks 1+

### Outcome
DONE_WITH_CONCERNS: brief-E1 "wave 1 proper" is in seven Python-only commits (tasks 1-7). The killed implementer's
10 dirty files (partial task 2) were mapped: the cycle_resume / runner / fake-runner parts were kept, the rest was
rewritten or finished; nothing of theirs is lost or committed unreviewed. Every new manifest key and CLI flag is
opt-in: with all absent, the tiny-world wave's argv, specs and written bytes match e93d5c2b (evidence 5). Concerns:
see Open risks (the stale-code reuse refusal is unconditional; `--candidates` waits for S1's exe).

### Branch / SHAs
`feat/p9-e1-20261003`, on top of the task-0 code `3fa2dd4a` and its report `a57961bb`:

| task | commit | subject |
|---|---|---|
| 1 | `e93d5c2b` | task-0 review minors 1-4 and the `--runner-override` cap |
| 2 | `c925ea2d` | K-P9-10 receipts, OR-3 resume argv check, OR-4 attempt sub-dirs, manifest `driver` block |
| 3 | `5e00576c` | F-5 (a) launch admission, OR section 5 host memory semaphore and parallel steps |
| 4 | `92b4f203` | OR-2 `lock --exes` pins `exes_sha256`, runs check them, verify compares parent and cell |
| 5 | `e5c219d6` | OR section 3: content receipt digests, manifest record date, code-keyed reuse, per-run verdicts |
| 6 | `159ab299` | OR section 5: complete timings in `wave-result.json` and `scoreboard --timings` |
| 7 | `d19606dd` | K-P9-11 registration keys, `candidates pin --by` role list, ruling P4 `candidates_only` |

This report is a separate commit on top of `d19606dd`.

### What changed, per task
- **Task 1** (`atx-impl/tools/backtest_integrity.py` ledger lock only, `cycle_resume.py`, `research_cycle.py`,
  `wave_stage_record.py`, `wave_stage_util.py`, `test_research_spec.py`, `test_wave_hardening.py`): lock removal
  retried on a sharing violation then warned (never raised over a finished append); a timed-out wait names the
  holder pid and whether it runs; one NAV run-dir enumeration (`cycle_resume.nav_run_dirs` / `completed_exes`) used
  by verify; the spec-kind test checks independent evidence; `--runner-override seconds=N` above 600 refused at parse.
- **Task 2** (`run_bounded_research.py`, `research_tree.py`, `cycle_resume.py`, `research_cycle.py`,
  `wave_context.py`, `wave_manifest.py`, `wave_stage_util.py`, `wave_steps.py`): K-P9-10: every `start.json` /
  `receipt.json` carries `argv_sha256` (`research_tree.argv_sha256`, the command after its executable), `attempt`
  (`--attempt K`, 1..99, default 1), `executable_sha256`, `build_type` (`--build-type Debug|Release`, else null;
  research_cycle passes it only for a spec with a `build` key via `research_tree.BUILD_TYPES`). OR-3:
  `cycle_resume.check_receipt_argv`: a done bounded output is reused only when its receipt's command equals the one
  the spec runs now (`REUSE_NEUTRAL`: `--save-combined`, `--no-composition`, the fit's theme-resid parent pair, aside);
  a mismatch is HARD-STOP exit 3. OR-4: `research_cycle --auto-attempt`: a step whose latest attempt has an
  `AUTO_OUTCOMES` receipt (prelaunch-memory-refusal, system-memory-limit, prelaunch-admission-timeout) and no output
  runs again in `<run dir>/attempt-k` (k <= `MAX_ATTEMPTS`); later reads use the latest sub-dir (receipt, NAV
  binding); the wave lists sub-dirs (`Wave.run_dirs`: timings, seal scan, verify). Manifest `driver` block
  (`wave_manifest.DRIVER_KEYS`, every key opt-in; `driver.auto_attempt` -> `run --auto-attempt`).
- **Task 3** (`run_bounded_research.py`, `research_tree.py`, `research_cycle.py`, `wave_context.py`,
  `wave_manifest.py`, `wave_stage_cell.py`, `wave_stage_record.py`, `wave_steps.py`): runner
  `--admission-wait-seconds S`: before launch, wait <= S for free >= `max_rss_mib + min_free_mib` and no
  cl / clang-cl / ninja / lld-link process; timeout = outcome `prelaunch-admission-timeout`, nothing launched.
  `--host-budget-mib N` (needs the wait): host semaphore, one claim file (declared cap) per admitted process under
  `--host-claims` (default `%TEMP%/atx-host-claims`), written under an `O_EXCL` lock, released in `finally`; dead
  owners' claims and locks dropped. research_cycle `--admission-wait` / `--host-budget-mib` pass both to every
  bounded process; under a budget `PARALLEL` pairs (ref || u, card || marginal) run in two threads; a phase cap above
  the budget is refused when planned. Wave: `driver.admission_wait_seconds` / `host_budget_mib` reach research_cycle,
  readers and bundle; under a budget the judge runs summ || bundle || book reader.
- **Task 4** (`research_cycle.py`, `wave_manifest.py`, `wave_result.py`, `wave_stage_library.py`,
  `wave_stage_record.py`, `wave_stage_util.py`, `wave_steps.py`): `lock --exes` writes `exes_sha256` {exe key: SHA}
  of the effective exes (a template's into `change.set`); a spec carrying it is checked when a Cycle is built (moved
  exe = exit 3) and the header lists the pins. `driver.lock_exes`: `lock SPEC --exes --write` for the screen
  library, a b library and a rule cell before their commit; verify records `{parent, cell, differ}` of spec pins and
  refuses a pinned NAV exe that moved without its ref run (`exe_problem`).
- **Task 5** (`atx-engine/tools/stage_chain.py` + test, `cycle_verdict.py`, `research_cycle.py`,
  `research_wave.py`, `wave_manifest.py`, `wave_result.py`, `wave_stage_cell.py`, `wave_stage_library.py`,
  `wave_stage_record.py`, `wave_stage_util.py`, `wave_steps.py`, `tests/wave_fixture.py`): `Chain(digest="content")`
  chains `previous_receipt_content_sha256` over canonical JSON without `TIME_KEYS` (`driver.receipt_digest:
  "content"`, also for `wave-result.json` receipt digests); `driver.record_date` dates the record stage's queue
  history; reader / bundle reuse refused (exit 3) when a `*.py` their run bound no longer hashes as bound;
  `research_cycle --keep-verdicts` (`driver.keep_verdicts`) leaves every `cycle_verdict.json` write in
  `<cycle dir>/verdicts/<mode>-<k>.json` (exclusive create) and the screen / judge pin that copy.
- **Task 6** (`wave_context.py`, `wave_manifest.py`, `wave_result.py`, `wave_scoreboard.py`, `wave_stage_library.py`,
  `wave_stage_util.py`, `wave_stages.py`): `driver.timings`: every command and git query timed; each stage receipt
  gains `outputs.processes`; the screen records its phase rows (u / fit / card / marginal); `wave-result.json` adds
  reader (`reader:<name>`) and bundle rows, `stage_seconds`, `processes`; `scoreboard --timings` adds register and
  git phases and a by-stage table.
- **Task 7** (`wave_manifest.py`, `wave_queue.py`, `wave_steps.py`, `wave_stage_library.py`, `research_cycle.py`):
  K-P9-11 optional `source_sample_end` (YYYY), `predicted_mechanism` (one line), `data_class` (H|W|P|N) on candidates
  and `rule_cell`; `candidates new --source-sample-end / --predicted-mechanism / --data-class`; `candidates pin --by`
  checked against `wave_queue.PIN_ROLES = ("pm", "root", "owner")`, any case. Ruling P4: `marginal.candidates_only`
  (bool) -> `WS.marginal_ruled` writes `marginal.candidates` (register: every manifest id; b library: the kept ids) ->
  research_cycle passes `--candidates <cycle dir>/marginal-candidates-<sha12>.txt` (UTF-8, one id per line, bound in
  the receipt, written when the step starts, never overwritten).

Tests (all in the new `scripts/tests/test_wave_driver.py`, 13): `test_receipt_k_p9_10_keys`,
`test_resume_refuses_argv_mismatch`, `test_attempt_subdir_after_floor_kill` (planted floor kill on the fake tools
resumes to completion in `attempt-2`; partial-output and `MAX_ATTEMPTS` cases; NAV binding in the sub-dir),
`test_driver_auto_attempt_manifest_flag`, `test_launch_waits_for_free_memory`, `test_parallel_steps_under_host_budget`,
`test_lock_exes_pins_and_verify_compares`, `test_receipt_digest_time_free`,
`test_reader_reuse_keyed_on_code_and_verdict_per_run`, `test_timings_complete`, `test_registration_keys_k_p9_11`,
`test_pin_by_role_list`, `test_marginal_candidates_only`. Each opt-in test also asserts the key-absent case.

### Evidence
All runs on the working tree that is commit `d19606dd` (no edit after the runs started).

1. `"C:/Program Files/Python312/python.exe" scripts/tests/run_two_seeds.py` (`PYTHONDONTWRITEBYTECODE=1`) -> exit 0
   ```
   == PYTHONHASHSEED=0 -m pytest -q -p no:cacheprovider scripts/tests
   338 passed, 4 skipped in 323.91s (0:05:23)
   == PYTHONHASHSEED=0: exit 0
   == PYTHONHASHSEED=1 -m pytest -q -p no:cacheprovider scripts/tests
   338 passed, 4 skipped in 266.33s (0:04:26)
   == PYTHONHASHSEED=1: exit 0
   ```
2. `... -m pytest -q -p no:cacheprovider atx-engine/tools` -> exit 0: `349 passed, 6 subtests passed in 162.60s`
3. `... -m pytest -q -p no:cacheprovider atx-impl/tools` -> exit 0:
   `625 passed, 2 skipped, 17 subtests passed in 199.09s`
4. `... -m pytest -q -p no:cacheprovider scripts/tests/test_wave*.py scripts/tests/test_research_cycle.py` -> exit 0:
   `156 passed, 3 skipped in 101.24s` (after task 7, before the full runs).
5. Identity, tiny-world wave (scratch script, not committed): the tests' `wave_fixture` world run end to end
   (`research_wave.main(["run", ...])`, fake cycle tools, sign rule drops alpha_b, no `driver` key) once with the
   e93d5c2b scripts (`git archive`) and once with HEAD's, plain and with a PM8-15 marginal ruling. With the temp
   root, time keys and receipt-file SHAs (which hash time fields) normalised: research_cycle argv 17/17 and 18/18
   identical, every library spec byte-identical, all 42 / 43 files under `out/waves/w1` identical in content (stage
   receipts, wave-result.json, wave-log.md, readers, bundle receipt), driver log identical. The only differences
   are git's "LF will be replaced by CRLF" warnings in 1-2 commit console logs, which differ between two HEAD runs
   too (git stat-cache noise).
6. Identity, plans (scratch, not committed): `research_cycle.py wave plan` of `y-s.json` with root's two DEC-1 /
   DEC-2 amendments (scratch copy): e93d5c2b vs HEAD output byte-identical, exit 0, 57 lines. `research_cycle.py plan`
   of all 39 `scripts/specs/v8/*.json`: identical output, but every one exits 3 at input resolution in pool-17
   (`input role missing: build-equity/train-2020-2023-lo3/manifest.json`: no data in this tree), so it proves little;
   the full-argv equalities in `test_research_cycle.py` (e.g. `test_screen_stops_before_w`) cover spec argv.

### Identity (flags and keys absent)
- Unconditional, by contract: runner receipts gain `argv_sha256`, `attempt`, `build_type` (K-P9-10; receipts are
  never byte-reproducible, they hold `started_utc`). `--build-type` is passed only for a spec with a `build` key
  (none of the 39 v8 specs has one).
- Unconditional, behaviour: (a) OR-3 refuses a done output only when its receipt has `argv_sha256` and a different
  command (pre-K-P9-10 receipts reused as before); (b) reader / bundle reuse refuses when a bound `*.py` changed
  (before: silent reuse); (c) `candidates pin --by` must be pm / root / owner (all 15 pins in
  `scripts/specs/v8/candidates/` are by PM; queue files are never rewritten by the check); (d) K-P9-11 keys are
  optional (no file changes).
- Everything else is behind a `driver` key or a CLI flag: `auto_attempt`, `admission_wait_seconds`,
  `host_budget_mib`, `lock_exes`, `receipt_digest`, `record_date`, `keep_verdicts`, `timings`; research_cycle
  `--auto-attempt`, `--admission-wait`, `--host-budget-mib`, `--keep-verdicts`, `lock --exes`; spec
  `exes_sha256`, `marginal.candidates`; manifest `marginal.candidates_only`.

### Deviations from brief
- Attempt sub-dirs live under the runner's run dir (`<out>-run/attempt-k`), not `<output>/attempt-k/`: the output dir
  must stay absent for "never overwritten" and the done check; the run dir is what the runner owns. Attempt 1 is
  the run dir itself; the receipt's `attempt` is that index. The fit's `-run<k>` passes keep their own numbering.
- Auto-advance happens on the next invocation with `--auto-attempt` (the refused attempt is reported as a stop that
  names the flag), not inside the same invocation.
- "Queue history dated from the manifest" and "`cycle_verdict.json` written per run, never overwritten" are opt-in
  (`driver.record_date`; `--keep-verdicts` copies to `verdicts/<mode>-<k>.json` while `cycle_verdict.json` keeps its
  path for every consumer) to honour the identity rule.
- Parallel ref || u: under a budget u runs beside ref and ref's compare runs after both. Record-stage seconds are not
  in `stage_seconds` (the record stage writes `wave-result.json` itself).
- The brief's "tiny-world wave end to end with a planted floor kill" is covered at the cycle level
  (`test_attempt_subdir_after_floor_kill` on the fake cycle tools); the fake wave (`FakeCycle`) fakes research_cycle
  itself, so a wave-level floor kill would only re-test the fake. Root's tiny-world run is the real one.
- `--candidates` is deliberately not in `research_cycle.MARGINAL_BUILT` (see merge notes).

### Cross-lane edits
- `scripts/research_cycle.py` (ruling P3: E1 owns it in wave 1): tasks 1-7 (runner flags, `launch_flags`,
  `PARALLEL` and the run-loop split into `parallel_partner` / `start_step` / `run_processes` / `step_outcome` /
  `finish_step`, exe pins, `--keep-verdicts`, `marginal.candidates`, docstring).
- `scripts/tests/test_research_cycle.py` (its test): fake runner behaviours `floor-kill`, `floor-kill-partial`.
- `scripts/tests/wave_fixture.py` (shared wave fixture): `FakeCycle.write_verdict` mirrors `--keep-verdicts`.
- `atx-impl/tools/backtest_integrity.py`: task 1 touches `ledger_append`'s lock only (the task-0 lines).
- `scripts/tests/test_research_spec.py`: task 1 review minor 4 only.
- No `scripts/specs/**`, registration file, C++ or `atx-db/` file touched.

### Merge notes for root
1. Merge `d19606dd` (tasks 1-7). No spec or manifest needs an edit: every new key is opt-in.
2. After S1 merges, `test_research_cycle.py::test_marginal_argv_is_the_verbs_full_cli` (it re-reads
   `strategy_marginal_ic.cpp`) fails until research_cycle names S1's new verb options: add `MARGINAL_CANDIDATES` to
   `MARGINAL_BUILT` and place S1's other new options (`--pair-cache`, `--verified-digests` if they land) in
   `MARGINAL_BUILT` or `MARGINAL_SPEC_FLAGS`. Until S1's exe is built, do not set `marginal.candidates_only` in a
   manifest (today's exe would refuse `--candidates`).
3. E2 (wave 2) moves `research_cycle.py`: carry the task 2-7 additions listed above in the move.
4. To opt in, a new wave manifest adds e.g. `"driver": {"auto_attempt": true, "admission_wait_seconds": 900,
   "host_budget_mib": 3584, "lock_exes": true, "receipt_digest": "content", "keep_verdicts": true, "timings": true}`
   before its pre-registration commit (`host_budget_mib` needs `admission_wait_seconds` and >= 1536). Corrected in
   fix round 1: the budget must not exceed the host's free memory at wave start (about 3,584 MiB today, plan
   section 0.6). The value 12000 given here before never binds on this host.
5. Verify: `$PY scripts/tests/run_two_seeds.py`; `$PY -m pytest -q -p no:cacheprovider atx-engine/tools`;
   `$PY -m pytest -q -p no:cacheprovider atx-impl/tools`; `$PY scripts/research_cycle.py wave plan
   scripts/specs/v8/waves/y-s.json` must print the same lines as before the merge.

### Open risks
- Stale-code refusal is unconditional: a wave in flight across a merge that changes `scripts/wave_readers.py` or
  `atx-impl/tools/nav_summ.py` stops (exit 3) when a resumed stage would reuse a reader or bundle output made with the
  old code; the message says to move the output aside or start a new state dir. Finish in-flight waves first.
- The host claims dir defaults to the host temp dir, shared by every worktree on purpose (one host budget); a claim
  file of a live but hung process holds its share until that process ends.
- `candidates pin --by` now refuses names other than pm / root / owner (any case); a lane script that pinned with
  another name stops with exit 2.

### Ledger candidates
- Two runs of the same fake wave differ in git's CRLF warnings in commit console logs: compare wave outputs by
  content, never console logs, for identity.
- K-P9-10 receipts make OR-3 checkable: a done bounded output is reused only on the command (argv_sha256) that made
  it (`cycle_resume.check_receipt_argv`, P9 E1 task 2, c925ea2d).

## Fix round 1

### Outcome
DONE: the review's major and the two E1-REUSE minors (PM ruling) are fixed in three Python-only commits on FIX_BASE
`e40c9152`. The other review minors stay deferred as ruled. Flag-absent identity still holds against e93d5c2b and
against the review base 3fa2dd4a (evidence 4).

### Commits
| fix | commit | subject |
|---|---|---|
| E1-REUSE (a) | `e26292e4` | resume reuses a done bounded output only on the same executable |
| E1-REUSE (b) | `c422313e` | code-keyed reuse covers the in-repo import closure |
| major | `210d5de8` | host-budget admission checks free memory under the claims lock |

This section is a separate commit on top of `210d5de8`.

### What changed
- **Major** (`scripts/run_bounded_research.py`, docs in `research_cycle.py` and `wave_manifest.py`): under
  `--host-budget-mib`, `HostClaims.try_claim` reads free memory inside the claims lock. It admits only when both
  hold: `free - sum(max(0, claim.mib - tree_rss_mib(claim)))` over the other live claims is at least
  `max_rss_mib + min_free_mib`, and the claims plus this one fit the budget. Both checks are in the same critical
  section. `tree_rss_mib` is the RSS of the claiming runner's descendants plus the adopted child's tree. After launch
  the runner records its child in its claim (`HostClaims.adopt`), so the claim lives while the runner or its child
  does: an orphaned child of a hard-killed runner keeps its share, and its RSS shrinks the claim's reservation.
  The admission block gains `reserved_by_others_mib` (receipts only carry it with the flags). `admit` /
  `HostClaims` take an optional `owner` (pid, create_time), default this process; tests use it.
  Without `--host-budget-mib` the code path, refusal text and order are unchanged.
- **E1-REUSE (a)** (`scripts/cycle_resume.py`, `research_cycle.py`): `check_receipt_exe` runs on every done bounded
  step, nav and ref included. When the K-P9-10 receipt (it has `argv_sha256`) records `executable_sha256` and the
  step's executable (argv after `--`) is on disk with another SHA, the step is a HARD-STOP, exit 3. The message
  says to move the output aside or run under a fresh `--suffix`. Receipts without `argv_sha256` or
  `executable_sha256`, and a hash-only exe, are reused as before. A matching receipt logs `   receipt exe: ...`.
  The `exes_sha256` PIN MISMATCH message now says that outputs of the old exe will be refused.
- **E1-REUSE (b)** (`scripts/wave_stage_cell.py`): `stale_code` adds `changed_imports`. The bound `*.py` files get
  their transitive in-repo import closure (`code_closure`): AST imports anywhere in the module, plus the names
  passed to backtest_integrity's `_engine_module(...)`. Names resolve to `<name>.py` in the importer's directory or
  in the repo's `scripts`, `atx-impl/tools` and `atx-engine/tools`. For the readers and the bundle the closure is
  wave_readers.py, nav_summ.py, backtest_integrity.py, dsr_total.py, engine_tools.py, and atx-engine/tools'
  era_pool.py and research_window.py. A closure module that differs from the run's commit (receipt `source_sha`,
  `git diff --name-only <source_sha> -- <modules>`) makes the output stale (exit 3). The runner refuses a dirty
  code pathspec, so `source_sha` is the code the run used. No argv, binding or receipt byte changes, so identity
  holds. Not checked: receipts without `source_sha` (`--no-git`) and modules outside the wave root. The module doc
  states that cycle-level Python phases (card, the fields builder) are not code-keyed.

Tests (all in `scripts/tests/test_wave_driver.py`, now 16):
- `test_two_launches_never_overcommit`: two admits with u (2,560 + 512) and ref (1,536 + 512) against a constant
  3,584 MiB free start at the same instant, using a barrier on the pre-lock compiler check. Exactly one is admitted;
  the other times out with `reserved_by_others_mib` = the winner's cap. One after the other, ref waits while u's
  claim reserves 2,560 MiB. The adopted child keeps u's claim alive after its runner is killed, and when the child
  ends the claim is dropped.
- `test_resume_refuses_exe_mismatch`: u and nav receipts stamped with K-P9-10 keys. A rebuilt `bin/ic.exe` or
  `bin/nav.exe` stops with exit 3 and nothing re-runs. Legacy receipts and receipts without `executable_sha256` are
  reused.
- `test_code_reuse_keyed_on_import_closure`: the real reader/bundle closure contains backtest_integrity.py,
  dsr_total.py, era_pool.py and research_window.py. In a committed synthetic reader, changing a module imported two
  levels down inside a function makes `stale_code` name it, both in the tree and when committed after the run;
  `refuse_stale_code` exits 3. A receipt without `source_sha` is not checked.

### Evidence
All on the clean tree at `210d5de8`.
1. `"C:/Program Files/Python312/python.exe" scripts/tests/run_two_seeds.py` (`PYTHONDONTWRITEBYTECODE=1`) -> exit 0
   ```
   == PYTHONHASHSEED=0 -m pytest -q -p no:cacheprovider scripts/tests
   341 passed, 4 skipped in 317.27s (0:05:17)
   == PYTHONHASHSEED=0: exit 0
   == PYTHONHASHSEED=1 -m pytest -q -p no:cacheprovider scripts/tests
   341 passed, 4 skipped in 273.75s (0:04:33)
   == PYTHONHASHSEED=1: exit 0
   ```
2. `... -m pytest -q -p no:cacheprovider atx-engine/tools` -> exit 0: `349 passed, 6 subtests passed in 160.80s`
3. `... -m pytest -q -p no:cacheprovider atx-impl/tools` -> exit 0:
   `625 passed, 2 skipped, 17 subtests passed in 194.45s`
4. Identity, tiny-world wave (scratch, not committed; same method as Tasks 1+ evidence 5, no `driver` key, plain
   and PM8-15-ruled): HEAD `210d5de8` vs `3fa2dd4a` (the review base): 0 differences. That covers argv 17/17 and
   18/18, files 42/42 and 43/43, log lines 51/51 and 53/53, and library specs 3/3, with time keys and receipt-file
   SHAs normalised. HEAD vs `e93d5c2b`: the same, except git's CRLF warnings in 1-2 commit console logs, which
   also differ between two HEAD runs.

### Deviations
- (a) applies to every K-P9-10 receipt, not only when the spec carries `exes_sha256`: a rebuilt exe makes an old
  output stale whether or not the spec pins it. Receipts written before K-P9-10 are untouched, so existing outputs
  resume as before.
- (b) is keyed on the run's commit (`source_sha`), not on new bindings. Binding the closure in the reader / bundle
  argv would have changed every wave's argv and receipts (identity). Consequence: a module outside the wave root,
  or a `--no-git` run, is not checked.
- Major: on top of the required check, the claim adopts the child (the review's orphaned-child point).

### Merge notes (correction)
- Tasks 1+ merge note 4 is corrected above: `host_budget_mib` must not exceed the host's free memory at wave start.
  N bounds the sum of declared caps; the in-lock free-memory check is what stops over-commit.
- Merge `210d5de8` (or this report commit). Verification is the same as Tasks 1+ note 5.

### Open risks
- `changed_imports` runs `git diff` against the receipt's `source_sha`. If that commit is no longer in the repository
  (rebased away and gc'd), git fails and the stage stops with exit 3 rather than reusing (fails closed).
- The deferred minors remain: lock robustness, the template / add-alpha inheritance of `exes_sha256`, nested time
  keys in content digests, completion order under parallel judge, and K-P9-10 attempt numbering.

## Fix round 2

### Outcome
DONE: re-review 1's N1 (fixed under PM ruling E1-REUSE-a2) and N2 are fixed in two Python-only commits on
FIX_BASE `124e2f4e`. Flag-absent identity against 3fa2dd4a still holds (evidence 4).

### Commits
| fix | commit | subject |
|---|---|---|
| N1 / E1-REUSE-a2 | `af1038d0` | exe check on reuse refuses only against an exes_sha256 pin |
| N2 | `a830deca` | claim adopt never kills the launched child |

This section is a separate commit on top of `a830deca`.

### What changed
- **E1-REUSE-a2** (`scripts/cycle_resume.py`, `research_cycle.py`, `wave_stage_util.py`, `wave_stage_cell.py`,
  `wave_result.py`):
  - `check_receipt_exe` now returns (how, note). For a done bounded step whose K-P9-10 receipt records
    `executable_sha256`, it asks `Cycle.reuse_pin(st)` for the step's exe key (the argv after `--`, matched against
    `exes`) and that key's `exes_sha256` pin.
  - Which pin applies: for an output the template inherited from its parent (`Cycle.inherited`: same output name
    via `PHASE_OUTPUTS`, same out_root, no `--suffix`), the pin comes from the parent's resolved spec
    (`Cycle.parent_spec`) only. Otherwise it comes from this spec.
  - Pinned and different: HARD-STOP, exit 3 (`... the parent's|the spec's exes_sha256 pins ic <sha>: refusing to
    reuse it`). Pinned and equal: logs `receipt exe: ... = the spec's|parent's exes_sha256 pin of <key>`.
  - Unpinned: a receipt that names another SHA than the exe on disk is reused, with one cycle log line
    `   receipt exe NOTE: <phase> output ... reused (no exes_sha256 pin; ruling E1-REUSE-a2)`.
  - The wave's run stage checks the cell's phase rows (`exe_notes`: completed K-P9-10 receipts whose `command[0]`
    now hashes differently). On a mismatch it logs one line, writes `outputs.exe_notes` into `05-run.json`, and
    `wave-result.json` carries `cell.exe_notes`, which `wave-log.md` prints as one `**Exe notes**` line. With no
    mismatch there is no key and no line.
- **N2** (`scripts/run_bounded_research.py`): `HostClaims.adopt` takes the claims lock (bounded wait
  `ADOPT_LOCK_SECONDS` = 5 s; a busy lock does not stop the write). `write` retries `os.replace` on
  `PermissionError` for `REPLACE_RETRY_SECONDS` = 2 s, then removes its temp file. Any adopt failure (lock,
  replace, a child that already exited) is a stderr warning that keeps the runner-pid claim. It never raises, so it
  can no longer record runner-error and kill the child it just launched.

Tests (`scripts/tests/test_wave_driver.py`, now 19; `test_resume_refuses_exe_mismatch` is replaced):
- `test_resume_exe_check_pinned_refuses_unpinned_notes`:
  - unpinned: after an IC rebuild the cycle exits 0, logs the NOTE line, and re-runs nothing;
  - pinned: receipts matching the pin are reused (logged); a stale u or nav receipt exits 3 with nothing re-run;
  - receipts without `executable_sha256`, or written before K-P9-10, are reused as before.
- `test_nav_only_template_after_ic_rebuild`: a NAV-only child of a parent whose u / w receipts carry the old IC exe,
  in four cases:
  - unpinned: exits 0, notes u and w, and only N2 runs;
  - cell pinned to the rebuilt exes, parent unpinned: inherited outputs are noted, not refused;
  - parent pinned to the old exe: reused, logged as "= the parent's pin";
  - parent pin moved: exit 3, nothing runs.
  The test also asserts that `inherited` is true for fields / u / fit / w and false for card (absent here) and nav.
- `test_run_stage_notes_unpinned_exe_reuse`: a planted K-P9-10 u receipt whose exe was rebuilt gives one driver
  log line, `05-run.json` exe_notes, `wave-result.json` `cell.exe_notes` and one wave-log.md line. A clean world
  gets none of them.
- `test_adopt_never_kills_the_child` (Windows): with a reader holding the claim open for 0.5 s, adopt writes it
  once the reader closes. With the claim held open past the retries, adopt warns, keeps the runner-pid claim, and
  leaves no temp or lock file.

### Evidence
All on the clean tree at `a830deca`.
1. `"C:/Program Files/Python312/python.exe" scripts/tests/run_two_seeds.py` (`PYTHONDONTWRITEBYTECODE=1`) -> exit 0
   ```
   == PYTHONHASHSEED=0 -m pytest -q -p no:cacheprovider scripts/tests
   344 passed, 4 skipped in 347.63s (0:05:47)
   == PYTHONHASHSEED=0: exit 0
   == PYTHONHASHSEED=1 -m pytest -q -p no:cacheprovider scripts/tests
   344 passed, 4 skipped in 300.10s (0:05:00)
   == PYTHONHASHSEED=1: exit 0
   ```
2. `... -m pytest -q -p no:cacheprovider atx-engine/tools` -> exit 0: `349 passed, 6 subtests passed in 174.46s`
3. `... -m pytest -q -p no:cacheprovider atx-impl/tools` -> exit 0:
   `625 passed, 2 skipped, 17 subtests passed in 193.04s`
4. Identity, tiny-world wave (scratch, not committed; no `driver` key, plain and PM8-15-ruled), HEAD `a830deca`
   vs `3fa2dd4a`:
   - ruled world: 0 differences (argv 18/18, files 43/43, log lines 53/53, specs 3/3);
   - plain world: argv 17/17, files 42/42, log lines 51/51 and specs 3/3 identical, except git's CRLF warning in
     one commit console log, which also differs between two HEAD runs.

### Deviations
- "Receipt note" is implemented as the wave run stage's `exe_notes` (05-run.json, wave-result.json `cell`), with
  one wave-log.md line and one driver log line. Bounded-runner receipts are write-once and are not edited. A
  standalone `research_cycle.py run` logs the NOTE line only.
- "Inherited" means the template keeps the parent's output name for that phase (research_spec's reuse rule). A
  `--suffix` run, or a different out_root, inherits nothing.

### Open risks
- `Cycle.parent_spec` loads the parent with `load_spec`. A parent that no longer resolves stops the child with the
  usual spec error. It resolved when the child itself was loaded, so this matters only if the parent changes
  between the two loads.
- The other review minors stay deferred, as listed in fix round 1.
