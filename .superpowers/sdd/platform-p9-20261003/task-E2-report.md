# Lane E2 report (P9 wave 2: research cycle split, manifest kinds, capabilities from the exes)

## Outcome
DONE_WITH_CONCERNS. Every brief deliverable and required carry item is committed, and so is Ruling E2-1's
`--attempt k` argv change. Five of the six carried E1 minors are fixed and the sixth was already covered by E1.
`scripts/tests` passes under PYTHONHASHSEED 0 and 1: 420 passed, 4 skipped, 0 failed. The concerns are about the
process, not the code: the suite-command trap, PM7-35 deferred, and post-merge fixture checks (E2-2/3).

## Branch / SHA
- Pool `C:/atx-wt/pool-17` (reused, no lease call), branch `feat/p9-e2-20261003`, base `1239a5ff` (W2-EARLY).
- Code head `defa34c2`. This report is committed on top of it (see `git log -1`). Tree is clean after this commit.
- Resumed from `handoff-E2.md` (`a304ed2d`, about 85% done) under Ruling RESUME. Nothing was restarted.

| SHA | What |
|---|---|
| 98ffcf75 | M1a-RED: the mine field-rule test reads the registry of record at the lock |
| 7e4573d5 | E1-N3: an inherited output is judged by the ancestor that made it (whole template chain) |
| 4d6699eb | move-only split of `research_cycle.py` into `scripts/cycle/{spec,resolver,phases,gate,compare,lock,cli}.py` |
| 4f770ec6 | `scripts/research_common.py`: one copy of the shared helpers; run-dir naming defined once |
| 8805ff97 | IC exe capabilities from `--list-rules --json` (K-P9-6 envelope), not `--help` |
| 88122c8a | R0-7-EOL: `.gitattributes` `scripts/specs/** text eol=lf` |
| 7f3bd88c | `atx.research-wave/v2` kinds (K-P9-9), `wave_kinds.py`, `wave_stage_kinds.py`, 10 kind fixtures |
| 0e6baa28 | D1 deviation: `inputs.theme_registry` passes `--theme-registry` (+ sha pin) to the u and w passes |
| fed4d4cb | A2 note: G-P5 allowlist row + frozen pair for the fields seal rule (cross-lane, T1's guard) |
| 11a0e6e4 | E1-N4: neutral exe notes, judged by the exes_sha256 pin rule |
| fa7d5805 | sticky exes_sha256 (the wave re-locks) + content digest drops nested time keys |
| a304ed2d | handoff at the owner stop (docs only) |
| 432f4caa | K-P9-10 `--attempt k` for numbered run dirs (Ruling E2-1); HostClaims lock failures are admission waits |
| de7c9344 | the side-by-side judge records consoles and process rows in call order |
| defa34c2 | test: D1's two rule schemas frozen for the D1-PSCHEMA checks (they outlive D2's re-pin) |

## Files changed
Owned (E1's inherited files + brief + carry):
- `scripts/research_cycle.py` (now a facade that re-exports every name), `scripts/cycle/__init__.py`, `cli.py`,
  `compare.py`, `gate.py`, `lock.py`, `phases.py`, `resolver.py`, `spec.py` (new, move-only split)
- `scripts/research_common.py` (new), `scripts/cycle_resume.py`, `scripts/research_spec.py`,
  `scripts/run_bounded_research.py`
- `scripts/wave_context.py`, `wave_manifest.py`, `wave_result.py`, `wave_scoreboard.py`, `wave_stage_cell.py`,
  `wave_stage_library.py`, `wave_stage_preflight.py`, `wave_stage_record.py`, `wave_stage_util.py`, `wave_steps.py`
- `scripts/wave_kinds.py`, `scripts/wave_stage_kinds.py` (new)
- `atx-engine/tools/stage_chain.py` + `atx-engine/tools/test_stage_chain.py`
- `scripts/specs/p9/kinds/*.json` (10 kinds) + `scripts/specs/p9/kinds/templates/*.json` (5) (new)
- tests: `scripts/tests/test_research_cycle.py`, `test_research_spec.py`, `test_wave_driver.py`,
  `test_wave_kinds.py` (new), `test_research_mine.py` (carry M1a-RED)

Cross-lane (each minimal, appended at the tail):
- `scripts/tests/test_no_python_mirror.py` (T1's guard): one ALLOWLIST row and one FROZEN_PAIRS pair (A2 note).
- `.gitattributes`: one line + one comment (R0-7-EOL).
- `backtest_integrity.py` (B2) was NOT touched.

New schema literals (Ruling SQL2-CLS; root adds them to `classes.json` at merge):
- `atx.research-wave/v2` (new, E2: the v2 wave manifest).
- Read, not owned: `atx.composition-rules/v1` (D1's IC `--list-rules --json`), `atx.nav-rules/v1` (C1's NAV
  `nav --list-rules --json`; C1 is not in this base, the fixtures and tests name it).

## Required carry items (wave2-carry "E2")
- **M1a-RED (required): fixed.** Root cause: prereg item 4 applies the mined-fields rule "at the lock, to the registry
  of record". The lock is H-F = X-5, commit `a5df67ae`. The test read the live registry, which also has the Y-S wave
  (`a5914373`, after the lock). That wave added so_wang_rev (`ea_window_pre5`), dato (`noa_lag4`) and ins_cluster
  (`ins_cluster_buy`). Fix: `REGISTRY_OF_RECORD_WAVES` = the `added_in` waves of the registry at `a5df67ae` (v4 v6 v61
  v70 v71 v80 v81 v8x2 v8x3 v8x4 v8x7). `registry_of_record()` asserts those waves are still present and filters to
  them. The constant comes from the registry at the lock commit, not from the failing set, and no expected value was
  edited (`EXCLUDED_BY_CLASS` is unchanged). Checked: the filtered live registry equals the registry at `a5df67ae` (ids
  and dsl).
- **E1-N3 (required): done.** `Cycle.template_chain()`, `parent_spec()`, `introducer(phase)`, `inherited(phase)` in
  `scripts/cycle/phases.py`. `introducer` walks up while the phase output name and out_root are equal and there is no
  suffix. The reuse pin names its owner as "the spec's", "the parent's" or "the ancestor's (k templates up)". Test
  `test_wave_driver.py::test_inherited_output_judged_by_the_ancestor_that_made_it` (3 levels G -> P -> C).
- **P14 / brief: done.** Move-only split; `research_common.py` (RUN_SUFFIX, `fmt_argv`, `sha256_file`, `execute`,
  `run_dir_of`, `free_run_dir`, `dirty_or_error`, `read_ledger`, `ledger_view`; users: research_spec,
  run_bounded_research, wave_steps, wave_context, cycle_resume, wave_stage_preflight, wave_scoreboard); the wave reads
  `Cycle.phase_bases()`, so run-dir naming is defined once (OR section 4). v2 manifest kinds and 10 fixtures (8
  runnable, walk-forward and new-cost-model schema only until D3 / C3). The `backtest_integrity.py` edits P14 allows
  were not needed.
- **P9 (PM7-35 Python copy, `wave_rules.py:50-51`): NOT retired, by design.** The carry says retire only after
  root's identity run against B1's C++ predicate. B1 note kept: `gate_counts` != the gate's count (runner-sign-0
  strings start counting when unified). Wave 3 / after root's identity.
- **D1-PSCHEMA: done.** `wave_kinds.checked_params` validates rule `params` against `params_schema` read from the exes'
  `--list-rules --json`. Params are never checked against a local copy of a schema.
- **D1 deviation: done.** `inputs.theme_registry` -> `--theme-registry PATH --theme-registry-sha256 PIN` on the u and
  w passes (spec `INPUT_KEYS` tail). The fitter's early theme-order refusal is no longer vacuous for a spec that sets
  the input. It stays vacuous for specs without it (the built-in 13-theme default).
- **R0-7-EOL: done** (blobs unchanged).
- **A2 note: done.** G-P5 row `atx-engine/tools/prepare_research_fields.py` `require_research_seal` <->
  `atx-engine/include/atx/engine/research/fields/reuse.hpp`, retire by P10.
- **SQL-11: no SQLite index flags added.** Record writers: the move-only split put the cycle's record writers
  (`write_verdict` = cycle_verdict.json, `admission_trials`, `copy_ledger`) in `scripts/cycle/gate.py`. Their code is
  unchanged; SQL3 takes them from there.
- **`--candidates` in MARGINAL_SPEC_FLAGS: not needed.** S1 already carries it via `marginal.candidates` /
  `MARGINAL_BUILT`.

## Carried E1 minors
| minor | state | commit |
|---|---|---|
| N4 exe notes ignore the pin rule / mislabelled | fixed | 11a0e6e4 |
| HostClaims lock failure -> runner-error | fixed: PermissionError on the create is held like FileExistsError; an unreadable lock last written > 5 s ago is reclaimed (`HostClaims.reclaim`); the lock wait is min(30 s, the admission wait left); a held lock is one more refused check, so it ends as `prelaunch-admission-timeout` (AUTO_OUTCOMES), never as runner-error | 432f4caa |
| `exes_sha256` inherited by child specs / add-alpha copies | fixed: `wave_stage_util.lock_exes` is true under `driver.lock_exes` OR when the parent spec pins exes, so the wave re-locks every spec it writes. A manual `add-alpha` outside a wave still copies the pins (fails closed; re-lock by hand) | fa7d5805 |
| content digests move with nested time values | fixed: `stage_chain.TIME_KEYS` (started_utc, seconds, wall_seconds, peak_mib) dropped at any depth | fa7d5805 |
| summ/bundle/book log + receipt row order under a budget | fixed: `Wave.side_by_side` buffers each call's process rows and consoles per thread and writes them in call order after all calls end; driver log lines stay live (progress) | de7c9344 |
| receipt `attempt` / `build_type` 1 / null for most runs (K-P9-10) | fixed per Ruling E2-1: research_cycle passes the runner `--attempt k` for k >= 2 of the IC passes' `<out>-run<n>`, the NAV's `<out>-run<k>` (`--attempt nav=k`) and the fit's `-run<j>` passes, right before `--output`; k = 1 keeps every argv. An OR-4 sub-dir still records its own k (retarget replaces the value). Consumer note in the `run_bounded_research.py` docstring: `attempt` is the index in the run dir's own numbering, and `build_type: null` = no `build` key = the default equity (Debug) build | 432f4caa |
| code-keyed reuse of imported modules (`wave_stage_cell.py:73-78`) | already covered by E1's E1-REUSE (b) fix (import closure, `test_code_reuse_keyed_on_import_closure`). Cycle-level Python phases are still not code-keyed; not in the E2 carry list, left as is | - |

## Contracts
- K-P9-9: `atx.research-wave/v2` (`wave_kinds.py`, `wave_stage_kinds.py`, `wave_manifest.py` accepts v1 or v2).
- K-P9-6 / K-P9-7 (consumer): IC capabilities and rule rows from `--list-rules --json`; NAV rule rows from
  `nav --list-rules --json` (`x-flag`).
- K-P9-10: `--attempt k` (k >= 2) for the numbered run dirs; build_type consumer note.
- K-P9-1: run-dir naming once (`research_common.run_dir_of`, `Cycle.phase_bases`).

## Tests written / changed
- `test_wave_kinds.py` (new, 42 tests): the ten fixtures, v1 manifests unchanged, v2 refusals, schema subset, rule flags
  from the tables (D1-PSCHEMA checks now against frozen copies of D1's two rows, `D1_SCHEMAS`), every fixture plans in a
  fake world, rule kinds write the derived constants, field / role builds, walk-forward schema only.
- `test_wave_driver.py`: `test_inherited_output_judged_by_the_ancestor_that_made_it` (N3),
  `test_claims_lock_failures_are_admission_waits` (new: PermissionError create, abandoned / fresh unreadable lock, lock
  wait bounded by the admission wait, held lock -> AdmissionTimeout), `test_judge_records_in_call_order` (new: a judge
  whose book reader finishes first records the same consoles and process rows as one finishing in call order),
  K-P9-10 attempt block in `test_receipt_k_p9_10_keys`, the sticky-pin block in
  `test_lock_exes_pins_and_verify_compares`, the N4 label assertion.
- `test_research_cycle.py`: `test_theme_registry_input_reaches_the_ic_passes`, the list-rules capability fake.
- `test_stage_chain.py`: nested time values in `test_content_digest_chains_without_time_keys`.
- `test_research_mine.py`: M1a-RED (`registry_of_record`).

## Evidence
All at code head `defa34c2`, tree clean (`git status --porcelain` empty), run from `C:/atx-wt/pool-17`. Explicit paths
only (PY-HYG). Each `--basetemp` is in the session scratchpad.

```
PYTHONHASHSEED=0 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/e2-pt-s0 scripts/tests
-> 420 passed, 4 skipped in 1094.61s (0:18:14); exit 0
PYTHONHASHSEED=0 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/e2-pt-c0 atx-engine/tools/test_stage_chain.py
-> 9 passed in 0.77s; exit 0
PYTHONHASHSEED=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/e2-pt-s1 scripts/tests
-> 420 passed, 4 skipped in 1319.59s (0:21:59); exit 0
PYTHONHASHSEED=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/e2-pt-c1 atx-engine/tools/test_stage_chain.py
-> 9 passed in 0.79s; exit 0
```
The 4 skips are the same in both seeds, and all need live inputs: `test_cycle_e2e.py:439` (ATX_EQUITY_BIN),
`test_research_cycle.py:56`, `:706` and `:946` (RESEARCH_CYCLE_LIVE_ROOT).

Targeted runs during this session (all exit 0): `test_wave_driver.py -k "k_p9_10 or attempt"` 3 passed;
`-k "claims_lock or launch_waits or two_launches or adopt_never or k_p9_10"` 5 passed; `-k judge_records` 1 passed;
`test_wave_kinds.py` 42 passed; `test_research_mine.py` 42 passed; `test_wave_review.py test_wave_review_minor.py` 30
passed.

Diagnostics (failed attempts; they support no success claim): the combined command
`pytest scripts/tests atx-engine/tools/test_stage_chain.py` (PYTHONHASHSEED=0) gave 31 failed, 398 passed, 4 skipped.
The cause is the conftest window bind (concern 1), not this lane's code. Pairing confirmed it:
`pytest scripts/tests/test_research_mine.py atx-engine/tools/test_stage_chain.py` gave 26 failed, 25 passed;
`pytest scripts/tests/test_wave_review_minor.py atx-engine/tools/test_stage_chain.py` gave 2 failed, 16 passed. The
same files alone pass.

## How root verifies
Build targets / gtest filters: none. This lane is Python only and wrote no C++.

Identity (flag absent = byte-identical):
1. `research_cycle.py plan` and `research_wave.py plan` on every v8 spec and wave manifest, base `1239a5ff` vs this
   head: byte-identical. The lane checked the hash-only plan of all 65 committed specs, plain and `--screen` (3,509
   lines), after the split, after the dedupe and after theme_registry (Resolver `known=` pins, no output opened). It
   was not re-run after 432f4caa. By construction 432f4caa changes a plan only where an attempt k >= 2 exists on disk
   or is given (`--attempt PHASE=k`). The new `test_receipt_k_p9_10_keys` block asserts that k = 1 argvs carry no
   `--attempt`. The command after the runner's `--` (what `argv_sha256` and the OR-3 resume check digest) never
   changes. When comparing, use only the 130 original keys of the plan snapshot; the p9 kind templates add 10.
2. `research_cycle.py --help` and its usage errors: byte-identical (checked after the split).
3. After the eol=lf checkout, re-check the spec pins (blobs unchanged; working-tree bytes may change on Windows).
4. The real IC exe's `--list-rules --json` capabilities include list-rules, marginal, no-composition and
   theme-registry.
5. Intended plan changes: (a) Ruling E2-1: a resumed cell with an existing `-run2+` IC / NAV / fit dir now plans
   `--attempt k` in its runner head. (b) Sticky pins (fa7d5805, review S7): a wave whose parent spec pins
   `exes_sha256` now locks every spec it writes with `lock --exes --write` even without `driver.lock_exes`, which
   adds those lines and pins to a v1 wave's plan and run. No committed spec pins `exes_sha256` today, so no committed
   manifest moves. (c) Fix round 1 adds refusals only (W1-I1, W1-I2 (a), R2): a plan within the bounds is unchanged;
   a v2 field / role wave's verify plan line names its builds, and every v2 wave's verify plan line states the
   W1-I2 (a) refusal (Ruling E2-6). A v1 manifest's verify plan line keeps its bytes, and is stale (see Fix round 1).

Post-merge items (Ruling E2-2/3; the exes these need are not in this base):
- After D2's merge (D2 re-pins `atx-impl/tests/fixtures/composition_rules_list.json`) and C1's merge, with the exes
  built (`<BIN>` = the dir of `atx-equity-strategy-ic.exe` / `atx-equity-strategy-targets.exe`), from the merged tree:
  ```
  <BIN>/atx-equity-strategy-ic.exe --list-rules --json > <scratch>/ic-rules.json
  <BIN>/atx-equity-strategy-targets.exe nav --list-rules --json > <scratch>/nav-rules.json
  "C:/Program Files/Python312/python.exe" -c "import json,sys; sys.path.insert(0,'scripts'); import wave_kinds as WK; t={'ic': WK.parse_listing(open(sys.argv[1]).read(), WK.IC_RULES), 'nav': WK.parse_listing(open(sys.argv[2]).read(), WK.NAV_RULES)}; [print(n, WK.rule_flags(json.load(open('scripts/specs/p9/kinds/'+n))['rules'], t)) for n in ('new-combination-rule.json', 'new-nav-rule.json')]" <scratch>/ic-rules.json <scratch>/nav-rules.json
  ```
  Expect `new-combination-rule.json ({'fit': {'--composition': 'ew-theme-std-v1'}}, [])` and
  `new-nav-rule.json ({'nav': {'--aim-leverage': '1.5'}}, [])`. Problems not empty = stop: the D2 / C1 schema and the
  fixture disagree. Do not edit the fixture to fit. Then
  `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_wave_kinds.py`
  must still pass (its D1-PSCHEMA checks read frozen D1 rows, so D2's re-pin does not turn them red).
- The kinds fixtures on tiny-world under the wave harness (8 runnable: all but new-cost-model and walk-forward):
  `research_wave.py plan <fixture> --root <tiny-world root>` with `scripts/specs/p9/kinds/templates/` copied under that
  root. Note: the tiny-world `tiny.json` has no `summ.ledger`, so a `run` past preflight needs the harness root supplies.
  new-nav-rule and new-horizon need C1's NAV exe; new-combination-rule needs D2's schema.

## Decisions
- Moved long lines are kept verbatim (move-only). New code wraps at 120.
- The `stage_chain` / `backtest_integrity` sha256 copies stay for layering (atx-engine/tools must not import scripts/).
- The composition-rule fixture keeps `rule = <name>` only when the schema pins a const.
- Sticky pins: the wave re-locks (one rule, fail-closed outside a wave). I did not strip pins in derive / resolve,
  because that would weaken N3's ancestor judging.
- K-P9-10 attempt: the receipt's `attempt` is the index in the run dir's own numbering. A sub-dir `attempt-k` inside
  a numbered `-run<n>` records k, and its path tells the numbering. One integer cannot carry both, and the path
  already does.
- HostClaims: the stale-lock rule uses mtime (5 s) only for an unreadable lock. A readable lock with a live holder is
  always held. At most 3 reclaims per acquire, so a lock that will not go away falls back to the bounded wait.
- Judge order: buffering per thread (not per-job reserved console numbers), because one job can run several commands
  (and git queries); `console()` returns None for a buffered console (no caller used the value).
- `test_wave_kinds.py`: the D1-PSCHEMA assertions now read frozen copies of D1's two rows (`D1_SCHEMAS`, verbatim
  from the base fixture) instead of the live fixture, which D2 re-pins. The validator checks and the expected messages
  are unchanged. This is not a weakening: without it they would go red at D2's merge for a legitimate re-pin.

## Concerns / open risks
1. **Suite command trap (not a code regression).** The handoff's full-suite command put `scripts/tests` and
   `atx-engine/tools/test_stage_chain.py` in one pytest session. pytest loads `atx-engine/tools/conftest.py` at
   startup, and that file binds the superseded research window (A1-C: TRAIN end 2023, seal 2025) for the whole
   session. Under that command 31 scripts tests fail at this head: 26 in test_research_mine ("windows must be ...
   inside TRAIN [2020-01-01, 2023-01-01)") and 5 in test_wave_review / test_wave_review_minor (seal-scan hits).
   Confirmed by pairing alone: `test_research_mine.py` + `test_stage_chain.py` fails the same 26, and each file alone
   passes. The conftest is in base (`880faac7`, kept by A1's `a2d783a8`), so base has the same trap. The evidence
   below runs them as separate invocations. Root's gate must do the same. T2 removes the bind (G-P9).
2. The lane did not re-run the hash-only plan identity after 432f4caa: the scratch tool of the earlier session is
   gone. The argument is by construction, and the new test asserts that k = 1 argvs are unchanged. Root's identity
   run (item 1 above) is the check.
3. PM7-35 Python copy (`wave_rules.py:50-51`) is still there. It is retired only after root's identity against B1's C++
   predicate (wave 3).
4. `new-combination-rule.json` is refused against D1's schema until D2's merge (D1-PSCHEMA, fail-closed).
   `new-nav-rule.json` and `new-horizon.json` need C1's NAV exe. Both are root post-merge checks (commands above).
5. HostClaims: two runners that reclaim the same dead or abandoned lock at the same instant can delete each other's
   fresh lock (remove, then create, is not atomic). The dead-pid path in base already has this race. The new
   unreadable-lock path widens it only for a lock abandoned between create and write. That is rare and was not
   addressed.
6. Cycle-level Python phases (card, the fields builder) are still not code-keyed: the remainder of the E1 minor at
   `wave_stage_cell.py:73-78`. It is outside the E2 carry list.

## Deviations from brief
- PM7-35 retirement deferred (carry: only after root's identity vs B1).
- Kinds fixtures: 8 runnable + 2 schema only (as P14 says). The tiny-world end-to-end runs are root's post-merge items
  (E2-2/3).

## Ledger candidates
- Never run `scripts/tests` and any `atx-engine/tools` test in one pytest session: `atx-engine/tools/conftest.py`
  binds the superseded research window (A1-C: TRAIN end 2023, seal 2025) for the whole session, and 31 scripts tests
  then fail (26 test_research_mine "windows ... inside TRAIN [2020-01-01, 2023-01-01)", 5 wave_review* seal-scan
  tests). Run them as two invocations. Measured P9 E2, 2026-10-03.
- research_cycle `--attempt k` (k >= 2) for numbered run dirs moves only the runner head, never the command after
  `--`, so OR-3 resume and argv_sha256 are unaffected (P9 E2, Ruling E2-1).

## Fix round 1
Review fixed: `task-E2-review.md` (`780a4934`, CHANGES_REQUIRED: R1, R2, S1-S9), plus the PM's W1-I1 and W1-I2 (a)
(rulings in pool-2 `progress.md`, details in `wave1-whole-review.md` I-1 / I-2). Rulings applied: E2-5, E2-6, E2-7.
Resumed under RESUME 4 from `handoff-E2.md` (`f692ea36`). Nothing was restarted or squashed.

| SHA | What |
|---|---|
| fe140597 | wip at the owner stop: R1, R2, W1-I1, W1-I2 (a), S1-S6, S9, with tests |
| f692ea36 | handoff at the owner stop (docs only) |
| this commit | Ruling E2-6: a v2 wave's verify plan line states the W1-I2 (a) refusal (+ test); this section |

| item | state | where | test |
|---|---|---|---|
| R1 seal scan of the builds | DONE | `wave_stage_kinds.BUILDS` / `build_bases`; `wave_seal.wave_logs` adds `<out_dir>/builds/<what>-run<k>`; `run_build` refuses a name outside BUILDS; a v2 field / role wave's verify plan line names the builds | `test_wave_kinds.py::test_build_logs_are_seal_scanned`: a field wave whose fake build log says 2024-06-28 (synthetic, T2-SYN) gives verify exit 4 naming the log; a clean log passes |
| R2 raw rule flags | DONE | `wave_kinds.table_flags` / `raw_rule_flags` (the named rules' derived flags plus every table rule's flags: composition flags, NAV x-flags, param x-flags); preflight checks both `rule_cell.constants.flags` and the template's `change.flags`; `owned_flags` removed (superseded) | `test_rule_wave_refuses_raw_rule_flags` (3 params: the named rule's `--aim-leverage`, an unnamed `--vol-target`, `--theme-erc`; constants and template; `--cadence` still allowed); `test_rule_flags_from_the_tables` |
| W1-I1 path length | DONE | `cycle/phases.py` `CACHE_*`, `deepest_cache_path`, `cache_path_problem`, `library_ids`, `Cycle.cache_problem` / `cache_path_check` (in `steps()` before u; exit 2); wave `wave_stage_preflight.cache_path_check` (exit 3, before register); facade re-exports. Layout from the C++ (comment block): Release identity `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0`, `_audit` +6 under `--eval-mode audit-exact`, the fp_ level always counted, the `.partial` leaf. The exe prints no identity, so this is a documented bound. Applied only while the spec's IC exe is on disk (Ruling E2-5) | `test_wave_driver.py::test_cache_path_bound_w1_i1`: under the v8 cache of pool-2 the path is 226 + len(id), matching the review's derivation, so ids <= 24 pass (<= 18 with audit-exact); the cycle and the wave refuse; nothing is refused without the exe; CAND_ID_RE stays at 64 |
| W1-I2 (a) parent exe | DONE | `wave_stage_record.exe_problem`: for `ref == "none"` (a cell without a reference construction, e.g. a rule cell) it refuses when the receipts' exes differ, when the pins name two NAV exes, or when only one side records an exe; it passes only when neither does (synthetic). Both hashes are in the message | `test_rule_cell_parent_nav_from_another_exe_is_refused` (5 params, full fake rule wave to verify); `test_lock_exes_pins_and_verify_compares`: the old "rule cell: recorded only" assertion is replaced by the ruled refusal |
| E2-6 verify plan line | DONE | `wave_stage_record.verify_plan`: a v1 manifest's line keeps its bytes (`V1_EXE_TEXT`); every v2 manifest's line states the refusal (`V2_EXE_TEXT`) | `test_wave_kinds.py::test_verify_plan_line_states_the_rule_cell_refusal`: the v1 line equals the literal line of before; 4 v2 fixtures (nav rule, combination rule, leverage, new signal) state the refusal and do not say "recorded;" |
| S1 unwritable claims dir | DONE | `run_bounded_research.ClaimsDirError` (a PermissionError for the whole wait with no lock on disk); `reclaim` returns True only when it removed the lock | added to `test_claims_lock_failures_are_admission_waits` |
| S2 replay on interrupt | DONE | `Wave.side_by_side` replays its buffers in `finally` | none new (the judge-order test passes) |
| S3 schema type list / malformed keywords | DONE | `wave_kinds.schema_problems` | `test_schema_subset` |
| S4 placeholder regex | DONE | `PLACEHOLDER_RE = [A-Z][A-Z0-9_]*` | `test_schema_subset` |
| S5 normalised field.output | DONE | `wave_kinds._norm` (drops `.`, case-blind) | `test_v2_refusals` (+2 rows) |
| S6 build reuse | DONE (receipt part); role seal / end left to the builder (Ruling E2-7) | `run_build` reuses an existing output only when this wave's last `<what>` run completed it with this command's argv_sha256, else exit 3 | `test_field_wave_builds_then_registers_on_the_build` (+2 refusals); KindsFake receipts carry argv_sha256 |
| S7 report plan changes | DONE | "How root verifies" item 5 lists the sticky-pin plan change (and now E2-6) | - |
| S8 in_code copy | NOT FIXED (accepted, Ruling E2-7) | moving it into `research_tree` is a cross-lane edit to a shared module, and `wave_manifest` <-> `wave_kinds` would import each other | - |
| S9 missing tests | DONE | leverage bounds, persistent PermissionError, build seal scan | `test_leverage_wave_refuses_a_value_outside_the_nav_table` and the tests above |

Test-side edit in an E2-owned file: `scripts/tests/test_research_spec.py` gains the fixture `stand_in_ic`, which lifts
`CACHE_PATH_MAX` for the 3 add-alpha tests on `v8_root`. Their IC exe is a stand-in (`bin/ic.cmd`) that writes no
cache, and pytest tmp roots are deeper than any pool, so W1-I1 would refuse the committed v7.1 / v8 ids there. No
assertion or expected value was changed.

### Evidence
Run in `C:/atx-wt/pool-17` on the tree of this commit (WIP `fe140597` + the E2-6 change), explicit paths,
`PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`, each `--basetemp` its own dir in the session scratchpad,
`scripts/tests` and `test_stage_chain.py` as SEPARATE invocations, one after the other. Logs:
`.superpowers/sdd/platform-p9-20261003/logs/e2f1-{s0,c0,s1,c1}.log` (pool-17, git-ignored, not committed).
```
PYTHONHASHSEED=0 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/e2f1-s0 scripts/tests
-> 434 passed, 4 skipped in 1108.96s (0:18:28); exit 0
PYTHONHASHSEED=0 ... --basetemp=<scratch>/e2f1-c0 atx-engine/tools/test_stage_chain.py
-> 9 passed in 0.67s; exit 0
PYTHONHASHSEED=1 ... --basetemp=<scratch>/e2f1-s1 scripts/tests
-> 434 passed, 4 skipped in 1667.62s (0:27:47); exit 0
PYTHONHASHSEED=1 ... --basetemp=<scratch>/e2f1-c1 atx-engine/tools/test_stage_chain.py
-> 9 passed in 0.94s; exit 0
```
The 4 skips are the live-input gates of before (same in both seeds): `test_cycle_e2e.py:439` (ATX_EQUITY_BIN),
`test_research_cycle.py:56`, `:706`, `:946` (RESEARCH_CYCLE_LIVE_ROOT). 434 = the 420 of round 0 + 14 new
(test_wave_kinds 42 -> 50; test_wave_driver +6: `test_cache_path_bound_w1_i1` and the 5 parent-NAV params). No skip
or xfail mark was added. The ledger's known-red `test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry`
(M1a-RED) was E2's and was fixed in round 0 (`98ffcf75`, root cause in "Required carry items"); it passes in both
seeds here. Targeted during the round (exit 0): `test_wave_kinds.py -k "verify_plan_line or every_fixture_plans"` 11
passed; before the owner stop `test_research_spec.py` 87 passed, `test_wave_kinds.py` 49 passed, and the
test_wave_driver cache_path_bound, parent_nav (x5), lock_exes, claims_lock, launch_waits, two_launches, adopt_never,
judge_records and k_p9_10 tests. Diagnostic (supports no success claim): the first seed-0 run before `stand_in_ic`
gave 4 failed / 429 passed / 4 skipped (the 3 v8_root add-alpha tests, plus
`test_the_whole_file_passes_with_a_generated_spec_present`, which re-runs them): W1-I1 refusing in pytest's deep tmp
roots, the cause stated above.

### Decisions
- W1-I1 is gated on the IC exe being on disk (Ruling E2-5): only the exe writes the cache, so with no exe nothing is
  computed and nothing needs refusing. It also keeps hash-only plans and fake-tool fixtures as before. The cycle check
  needs the library on disk (its ids); a hash-only plan skips it. A spec whose exe appears after plan is checked at
  run, because `steps()` runs the check again before u.
- The W1-I1 bound is conservative: the fp_ level is counted for every candidate (base candidates do not have it), and
  the Release identity is used for every build (the Debug identity adds no level).
- W1-I2 (a) is keyed on `ref == "none"` (no reference construction), which is broader than "rule cell" by name: a
  library cell without a ref phase has the same exposure and is refused alike.
- E2-6: the new plan text is keyed on the manifest being v2 (any kind), not on kind `rule` only, because the refusal
  is keyed on the cell's ref phase (above): a v2 library-kind wave's line would otherwise be stale the same way. v2
  has no plan identity to keep. v1 keeps its bytes. **Known stale v1 plan line (Ruling E2-6):** a v1 manifest's
  verify plan line still says "NAV exe sha256 of the parent vs the cell (recorded; differing, the cell's ref must
  have run on the cell's exe)", while verify now refuses a v1 cell without a ref construction whose NAV exes differ
  or are recorded on one side only.
- S6 role seal / end and S8: not done here, per Ruling E2-7 (the role builder owns its manifest layout and enforces
  seal / end; S8 is a cross-lane import cycle). R1's build-log seal scan and the builder's refusal cover S6's risk.

### W1-I1 margin on committed specs (finding for the PM / root)
A scan over every committed cell spec with an IC pass (Release bound, longest id per spec) found the margin tight.
The longest id is `seasonality_same_month` (22 chars). The worst spec is `scripts/specs/v70-lo3.json` (cache
`build-equity/mega-candidate-cache-v70-lo3`, no audit-exact). Re-derived this round with
`cycle.phases.deepest_cache_path`: **249** at `C:/atx-wt/pool-2`, **250** at `C:/atx-wt/pool-17` (equal to the bound,
so not refused), **239** at `C:/atx`. A pool path one character longer (e.g. `pool-100`) or a longer cache dir refuses
that committed spec at plan once the IC exe is built there. Root's plan-identity run in pool-2 stays within the bound.
The ruling's "AL / PRE ids <= 24 chars" matches exactly: under the v8 cache in pool-2 the bound admits 24 (18 with
audit-exact).

### Not fixed / open
- S8 (accepted, E2-7) and S6's role-manifest seal / end check (left to the builder, E2-7).
- The stale v1 verify plan line (E2-6, above).
- Root at merge: re-review the fix SHA. The review's "What root must check at merge" items still hold: separate
  pytest invocations, and plan identity with the real IC exe on disk (which now also runs the W1-I1 check at plan).

## Fix round 2
Review fixed: `task-E2-rereview-1.md` (`867f40f4`, CHANGES_REQUIRED: R2b, N1-N5), on top of `05a0fcd1`. Rulings
applied: E2-8 (R2b + N1 + N2, one guard), E2-9 (N4 fixed, N3 fixed as a one-line use of the cell's own cache dir, N5 a
P10 note, `allow_abbrev=False` to D3). EXE-FREEZE is root's sequencing (no code here). Nothing squashed.

| item | state | where | test |
|---|---|---|---|
| R2b (E2-8 1, 3, 5) raw rule flags on the RESOLVED cell | DONE | `wave_stage_kinds.rule_flag_guard` (preflight, before register). (a) Every option map (`rule_cell.constants.flags`, the template's `change.flags`, `horizon.flags`) is checked by `wave_kinds.raw_rule_flags`: a token is matched with `=value` stripped, and a proper prefix of an owned flag is refused as an abbreviation (`wave_kinds.abbreviated`). The derived flags would otherwise silently override a raw one, so the map check stays. (b) The rule cell is resolved as the spec stage writes it (`kind_constants`, shared with `rule_cell`; `wave_steps.rule_cell_doc`; `research_spec.resolve`). For every owned option, its occurrences in the cell's flag lists (`argparse_values`: exact, `=value`, abbreviations) must equal those of the parent's lists with only the named rules' derived flags applied. Any difference is refused: added, dropped or re-valued. The message names the token(s) and the route (`touching`: constants.set / change.set / change.unset key, or the flag map entry). `wave_kinds.owned_flags` = `table_flags` + derived | `test_wave_kinds.py::test_raw_rule_flags_refused_on_the_resolved_cell` (11 probes, below) and `test_rule_flags_from_the_tables` (`=value`, abbreviation, `abbreviated`) |
| N2 (E2-8 2) every v2 kind | DONE | `preflight_kind` reads both tables for every v2 kind and runs the guard. Template kinds (rule, horizon, role, leverage with or without a rule_cell) are resolved and compared. Library kinds (add, replace, field) and walk-forward have no rule cell: add-alpha copies the parent's flag lists and no route can set one, so only the table checks apply there | probes on horizon, role and leverage; N1 test on add, rule and horizon |
| N1 (E2-8 4) unmapped IC rule | DONE | `wave_kinds.unmapped_ic_rules`: an IC row `COMPOSITION_FLAGS` does not map refuses the wave by name ("add their rows (e.g. MR-4) first"). An unreadable table is a refusal too, since the guard needs both | `test_raw_rule_flag_guard_allows_free_flags_and_names_unmapped_ic_rules` |
| N3 (E2-9) wave W1-I1 cache dir | DONE (one line) | `wave_stage_preflight.cache_path_check`: a library wave's cell is checked on add-alpha's cache dir (the parent's `ic.cache` only when `research_add_alpha.shared_stores(parent)`, else the derived dir; the rule is imported, not copied) | `test_cache_path_bound_w1_i1` (the wave half computes the cell's dir the same way) |
| N4 (E2-9) W1-I1 test vs `--basetemp` | DONE | `test_wave_driver.py::test_cache_path_bound_w1_i1` sets `cycle.phases.CACHE_PATH_MAX` from the computed deepest path (monkeypatch), so nothing depends on pytest's tmp depth. Positive cases added: a fitting id passes (cycle, audit-exact at +6, and the wave's preflight with the exe on disk) | the test itself |
| N5 (E2-9) `ClaimsDirError` last-exception race | NOT FIXED (P10 note) | - | - |
| fitter `allow_abbrev=False` | NOT TOUCHED (D3 carry, E2-9) | the guard refuses abbreviations at preflight; a hand-typed one outside a wave still reaches the fitter until D3 | - |

The 11 probes (each preflight exit 3 before register, with the message text asserted):
- `constants.set nav.flags += --vol-target` (rule)
- `constants.set fit.flags = --theme-erc` (rule)
- template `change.set fit.flags` (rule)
- `fit --theme-erc=theme-erc-v1` (`=value`)
- `fit --theme-er` (abbreviation)
- `fit --composition=ew-theme-std-v1`: now refused at preflight, not at spec (the R6B-C-4 exit 4)
- `horizon.flags nav --vol-target` (horizon)
- `constants.set nav.flags += --risk-target` (role)
- template `change.unset nav.flags`, which drops the parent's `--aim-leverage` (role)
- template `change.flags nav --risk` (abbreviation of 3 NAV flags, role)
- `constants.set nav.flags += --two-speed` (leverage with a rule_cell)

Allowed and asserted: `--cadence` by `constants.flags` (the round-1 test) and by `constants.set nav.flags`. The
horizon fixture's `--cadence` and every fixture's spec stage pass as before.

**Mutation check:** I stashed only the three source files (`wave_kinds.py`, `wave_stage_kinds.py`,
`wave_stage_preflight.py`) back to `05a0fcd1` and ran the new tests. Result: 12 failed (all 11 probes + the
free-flag / N1 test). With the fix: 12 passed. The stash was popped clean.

Plan changes (v2 only, no plan identity):
- Every v2 plan now prints both `--list-rules --json` argvs and one "ruling E2-8" line.
- Every v2 preflight receipt records both table SHA-256s, not only for rule and leverage.
- A v1 plan prints neither. `test_verify_plan_line_states_the_rule_cell_refusal` asserts this, and the v1 verify line
  stays byte-equal.

### Evidence
Run in `C:/atx-wt/pool-17` on the tree of this commit (`05a0fcd1` + this round). Explicit paths,
`PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`, each `--basetemp` its own dir in the session scratchpad, and
SEPARATE invocations. Logs: `.superpowers/sdd/platform-p9-20261003/logs/e2f2-{s0,c0,s1,c1}.log` (git-ignored).
```
PYTHONHASHSEED=0 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/e2f2-s0 scripts/tests
-> 446 passed, 4 skipped in 907.88s (0:15:07); exit 0
PYTHONHASHSEED=0 ... --basetemp=<scratch>/e2f2-c0 atx-engine/tools/test_stage_chain.py
-> 9 passed in 0.95s; exit 0
PYTHONHASHSEED=1 ... --basetemp=<scratch>/e2f2-s1 scripts/tests
-> 446 passed, 4 skipped in 936.64s (0:15:36); exit 0
PYTHONHASHSEED=1 ... --basetemp=<scratch>/e2f2-c1 atx-engine/tools/test_stage_chain.py
-> 9 passed in 0.67s; exit 0
```
- 446 = 434 + 12 new tests in `test_wave_kinds.py` (50 -> 62). The `test_wave_driver.py` W1-I1 test was reworked, not
  added.
- The same 4 live-input skips as before. No skip / xfail mark added. No expected value edited: the round-1 R2 test's
  messages are unchanged, and `test_every_fixture_plans_in_a_fake_world` only gained assertions.
- Targeted runs this round (exit 0):
  - `test_wave_kinds.py` 62 passed.
  - `test_wave_driver.py -k cache_path_bound` 1 passed.

### Decisions
- **Compare against the parent's lists with only the derived flags applied, not a ban on owned tokens.** A parent may
  carry a rule flag legitimately (the fake parent's `--aim-leverage {leverage}`; v8 parents' `--composition`).
  Inherited occurrences are allowed. A dropped one (unset, or a set that replaces the list) is refused like an added
  one, because both change the construction rule unnamed.
- **Sections are compared on their own owned flags:** fit against the composition flags, nav against the NAV rows' and
  `two-speed`'s. `flag_lists` would also compare the ic lists, including the w pass's with `ic.w_flags` applied, but
  only for a section a table owns flags in, and no table owns an ic flag today. An owned flag written into another
  section's list is not compared there. The NAV and IC exes refuse unknown flags, and so does the fitter's
  `parse_args` (`atx-impl/tools/fit_composition_weights.py:2730`).
- **Duplicates are suppressed:** an option the map check already reported is not reported again by the resolved
  comparison. The resolved differences are grouped per route, list and token.
- **The guard is fail-closed:** a template that cannot be resolved, or an unreadable table, refuses the wave.
- **Accepted by E2-8:** a legitimate non-rule flag that is a proper prefix of a rule flag is refused and must be
  spelled another way. None is known: `--cadence` and `--min-names` prefix nothing.
- **N3 stays at one line:** a template cell is still checked on the parent's cache dir, e.g. the role fixture's
  template sets its own `ic.cache`. research_cycle checks the cell's own dir at plan and again before u, still before
  any compute (same exposure as N3; no P9 cell is affected).

### Not fixed / open
- N5 (P10 note, E2-9). S8 and S6's role seal / end check (E2-7). The stale v1 verify plan line (E2-6).
- Fitter `allow_abbrev=False` is D3's (E2-9).
- Root at merge: re-review this fix SHA.
  - When AL-COMB's C++ table lands without MR-4, every v2 wave now stops by name until `COMPOSITION_FLAGS` gains the
    rows (N1, intended).
  - The rereview's "What root must check at merge" items 1-11 still hold.

## Fix round 3
Review fixed: `task-E2-rereview-2.md` (`558070e5`, CHANGES_REQUIRED: R2c + S1-S3), on top of `d0054b32`. Rulings
applied: E2-10 (R2c, with S1-S3 in the same round), E2-11 (a report sentence only), E2-12 (the unmapped-rule example).
E2-8 and E2-9 still bind. History, nothing squashed: WIP `daaa483c` (code and tests, owner stop), handoff `42d7401e`,
then this commit (verification, two small test / typing fixes, this section).

| item | state | where | test (all in `scripts/tests/`) |
|---|---|---|---|
| E2-10 / R2c `nav.leverage` | DONE | `wave_stage_kinds.leverage_refusal`, called from `rule_flag_guard` once the cell is resolved (every template kind; library kinds copy the parent's nav, walk-forward is refused anyway). Refused: (a) any raw set / unset key `nav.leverage` (or under it) in any kind, by `rule_cell.constants.set`, the template's `change.set` or `change.unset`; (b) a resolved cell whose `nav.leverage` differs from the parent's (from `leverage.value` for kind leverage), by any route, e.g. a whole `nav` section set or the template itself. The message names the key, the value and the route, and cites E2-10. `raw_routes` now returns `{route: {key: value}}` so the value can be named. `preflight_kind` (leverage): refused when the NAV table lists no fixed-v1 `aim_leverage` range (nothing to range-check against), and above `wave_kinds.LEVERAGE_MAX` = 2.0 even when the table allows more (OD-P9-3) | `test_wave_kinds.py::test_nav_leverage_is_set_only_by_a_leverage_wave` (7 probes: 5.0 by `constants.set` on horizon and on a composition-rule wave with `gross_match: "none"`; by template `change.set` on role, `"none"` and pm6-6; a whole `nav` section; `change.unset`; a raw key in a leverage wave). `::test_leverage_wave_cap_and_table_and_the_matched_l_prime` (1.5 passes to spec; 2.5 refused by the cap on a table allowing 3; no fixed-v1 row refused; a pm6-6 horizon cell still publishes its L' in the `-gm` copy). 2.0 itself passes (`leverage-change.json` in `test_rule_kinds_write_the_derived_constants`) |
| S1 (N3 test with teeth) | DONE | `test_wave_driver.py::test_cache_path_bound_w1_i1`, new last block: a parent off the shared stores with its own shorter `ic.cache "c"`; the bound is set between the two depths; preflight refuses at the derived depth - 1 and passes at it | the test itself |
| S2 (case / single-dash spellings) | DONE | `wave_kinds.norm_option`: a leading single `-` read as `--`, the option name (before `=`) case-folded. Used by `abbreviated`, `raw_rule_flags`, `wave_stage_kinds.option_tokens` and `occurrences` (`argparse_values` on normalised lists, both sides) | 3 rows in `E2_8_PROBES` (`--Vol-Target` by set, `-theme-erc` by flag map, `-theme-erc` by set) |
| S3 (route message) | DONE | `wave_stage_kinds.touching` also names a flag-map entry whose VALUE is the token (`rule_cell.constants.flags nav --cadence --vol-target`) | 1 row in `E2_8_PROBES` |
| E2-12 | DONE | `test_wave_kinds.py`: `UNMAPPED = "zz-unmapped-rule-v0"` replaces `theme-cov-hedge-v1` in `test_rule_flags_from_the_tables` (plus `assert UNMAPPED not in WK.COMPOSITION_FLAGS`) and in `test_raw_rule_flag_guard_allows_free_flags_and_names_unmapped_ic_rules`. No `theme-cov-hedge` literal is left under `scripts/` (`git grep`) | the two tests |
| E2-11 | REPORT ONLY | No v2 wave hands an SPO cell a new risk model in P9: `--risk-model` stays reserved to the rules the NAV table lists (vol-target, risk-target, and `mv-aim-v1` once COV-MV adds its row), and SPO rows in the NAV rule table are an optional C3 carry (P9 registers no SPO cell) | - |

Fixed in this commit (after the WIP):
- `test_leverage_wave_cap_and_table_and_the_matched_l_prime` asserted the exit code only after reading the failed
  receipt. With the cap removed (mutation M2), it failed on `FileNotFoundError` instead of a clear assert. It now asserts
  `code == 3` first. No expected value changed.
- `raw_routes`: the `change.unset` read is narrowed through a local (mypy could not narrow a repeated `.get()`). No
  behaviour change.

### Evidence
Run in `C:/atx-wt/pool-17` on the tree of this commit (`42d7401e` + the two fixes above). Explicit paths,
`PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`, each `--basetemp` its own dir in the session scratchpad, and
SEPARATE invocations, one after the other. Logs: `.superpowers/sdd/platform-p9-20261003/logs/e2f3-*.log`
(git-ignored).
```
PYTHONHASHSEED=0 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/e2f3-s0 scripts/tests
-> 458 passed, 4 skipped in 782.48s (0:13:02); exit 0
PYTHONHASHSEED=0 ... --basetemp=<scratch>/e2f3-c0 atx-engine/tools/test_stage_chain.py
-> 9 passed in 0.77s; exit 0
PYTHONHASHSEED=1 ... --basetemp=<scratch>/e2f3-s1 scripts/tests
-> 458 passed, 4 skipped in 532.85s (0:08:52); exit 0
PYTHONHASHSEED=1 ... --basetemp=<scratch>/e2f3-c1 atx-engine/tools/test_stage_chain.py
-> 9 passed in 0.47s; exit 0
```
- 458 = round 2's 446 + 12 new cases in `test_wave_kinds.py` (62 -> 74): 7 `nav.leverage` probes, the cap test, and 4
  `E2_8_PROBES` rows. The `test_wave_driver.py` W1-I1 test was extended, not added (28 unchanged).
- The same 4 live-input skips as rounds 1 and 2: `ATX_EQUITY_BIN` x1, `RESEARCH_CYCLE_LIVE_ROOT` x3. 0 failed under
  both seeds. No skip / xfail mark added, no expected value edited. The E2-12 edit swaps a test's own synthetic id;
  it is not an expected hash, golden or pin.

**Targeted runs** (seed 0, exit 0):
- `test_wave_kinds.py`: 74 passed, after the E2-12 edit (`e2f3-kinds-s0.log`).
- The 26-test teeth set (below): 26 passed on the head (`e2f3-teeth-head.log`).

**Teeth.** Each mutation was made in the working tree with the Edit tool, a 26-test set was run (the three test
functions holding round-3 cases: `E2_8_PROBES` 15, `nav.leverage` 7, the cap test; the two E2-12 tests; and
`test_cache_path_bound_w1_i1`; `logs/e2f3-teeth-<label>.log`), and the edit was
reversed. `git diff` was empty after each one.

| label | mutation | result |
|---|---|---|
| M1 | `rule_flag_guard` no longer calls `leverage_refusal` | 7 failed: all 7 `nav.leverage` probes, each `assert 0 == 3` (preflight passed) |
| M2 / M2b | the 2.0 cap check disabled | cap test failed. M2 (before the reorder) failed on `FileNotFoundError` (no failed receipt); M2b (after it) failed on `assert code == 3` (exit 0) |
| M3 | no refusal when the table lists no fixed-v1 range (the `d0054b32` condition) | cap test failed at the no-fixed-v1 assert (exit 0) |
| M4 | `norm_option` made the identity | 3 failed: `--Vol-Target` by set and `-theme-erc` by set pass preflight (`assert 0 == 3`). `-theme-erc` by flag map is still refused at preflight without it, but only as "the rule cell cannot be resolved ...: `'-theme-erc'` is not an option" (as the re-review noted), so the test fails on its message, which now names the owned flag |
| M5 | `touching` back to `d0054b32` (key-only naming) | 1 failed: the S3 probe, on its message ("the resolved cell (no raw route names it)") |
| M6 | N3's line reverted to `cell = spec` | `test_cache_path_bound_w1_i1` failed at the new block, `assert 0 == 3` |
| M7 | `unmapped_ic_rules` returns `[]` (E2-12's literal still exercised) | 2 failed: `assert [] == ['zz-unmapped-rule-v0']`, and the N1 test's exit-3 assert |

The handoff's whole-file check (both source files stashed back to `d0054b32`, the 12 new tests failing) was the earlier
session's. This session's attempt to repeat it with `git restore --source=d0054b32 --worktree` was **denied by the
permission system**. It was not retried and not routed around; the line mutations above replace it.

**No committed spec newly refused.**
- A scratch pytest (not committed; it imports this lane's fake-world helpers) drove all 10 committed P9 kinds fixtures
  to preflight. Result: the 8 runnable ones exit 0; `new-cost-model` and `walk-forward` exit 3 for their schema-only
  reasons, with no E2-8 or E2-10 text. This is the same as round 2. The re-review's R2c probe (`set nav.leverage 5.0`,
  `gross_match: "none"`, on new-horizon, new-combination-rule and new-universe) now stops at preflight, exit 3, naming
  `rule_cell.constants.set nav.leverage = '5.0'` (`logs/e2f3-fixtures-probe.log`).
- A scan of all 101 JSON files under `scripts/specs/`:
  - No flag list or flag map has a single-dash or upper-case option token. The 10 single-dash strings are formula
    descriptions, e.g. `"-VOV, ..."`. So `norm_option` refuses nothing committed.
  - 11 v1-era v8 templates set `nav.leverage` in `change.set`: 8 `-gm` copies written by the gross-match stage, plus
    `x-leverage-L2.0`, `y-vol-target` and `y-vol-target-y-1`. None is a template of a v2 fixture. v1 manifests never
    reach the guard. Used as a v2 rule-cell template, each would now be refused, as E2-10 intends: the leverage comes
    from kind leverage or from match, never from a template.
- v1 plans: `test_verify_plan_line_states_the_rule_cell_refusal` (v1 verify line byte-equal, no `--list-rules` / E2-8
  line in a v1 plan) passes. Every round-3 helper (`norm_option`, `leverage_refusal`, the cap) is reached only through
  `rule_flag_guard` / `preflight_kind`, and both return early for v1 (`kind` is None).

### Decisions
- **An explicit `nav.leverage` check, not the reviewer's `{leverage}` substitution in `flag_lists`.** Leverage reaches
  the argv only through `{leverage}` (`cycle/phases.py:821`, token-exact). So the key check covers every route, and
  its message is clearer: it names the key and the value, not a re-valued `--aim-leverage`.
- **Library kinds have no route** to the cell's nav: add-alpha copies the parent's nav. Walk-forward is refused at
  preflight anyway.
- **A raw `nav.leverage` is refused in a leverage wave too**, because `leverage.value` would override it silently. It
  is also refused in any other kind when it equals the parent's value: E2-10 says "sets or changes".
- **Values are compared as `str()`**: the parent's `nav.leverage` may be a JSON number or a string.
- **"A kind leverage wave naming its NAV rule"** is read as: the wave's value is fixed-v1's `aim_leverage`, checked
  against fixed-v1's row in the NAV table. A table without that row refuses the wave. The leverage block (`{value,
  name?}`, K-P9-9) gains no rule key, so the contract is unchanged.
- **`norm_option` is the guard's reading only.** The exes and the fitter are unchanged. Both sides of every comparison
  are normalised, so normalising a value token (e.g. a negative number) can never make a difference by itself.

### Not fixed / open
- N5 (P10 note, E2-9). S8 and S6's role seal / end check (E2-7). The stale v1 verify plan line (E2-6). The fitter's
  `allow_abbrev=False` (D3 carry, E2-9).
- The 2.0 cap is kind leverage's (E2-10). A kind rule wave naming fixed-v1 has its `aim_leverage` bounded only by the
  table's `params_schema`, which is [1, 2] in C1's table today. If a table widened it, the cap would not apply there.
  Not ruled.
- Observation, ruled E2-13 (b) (wording widened in fix round 4, re-review 3 S3): a kind rule wave naming ANY NAV rule
  whose params give `aim_leverage` replaces `{leverage}` with that literal: fixed-v1 always (`aim_leverage` is
  required), vol-target-v1 and risk-target-v1 when the manifest gives it. Under gross matching the match stage's L'
  (`nav.leverage`) then does not reach the argv. Both modes fail closed: Python pm6-6's matched run repeats the
  calibration argv, so its G is unchanged and the stage stops after its one correction; pm6-6-engine refuses at match
  (EXIT_PIN, `wave_stage_cell.match_engine`'s `leverage_requested` check). For the P9 templates: a NAV rule under gross
  matching omits `aim_leverage` (C1's live table makes it optional for vol-target-v1 / risk-target-v1). The committed
  fixture uses `gross_match: "none"`.

### Cross-lane notes for root
- **MR-4.** Add `theme-cov-hedge-v1` and `mom-volman-v1` to `wave_kinds.COMPOSITION_FLAGS` in the same merge as
  AL-COMB's C++ table (re-review 2, root item 2). Until then, every v2 wave stops by name (N1, intended). After E2-12,
  no E2 test depends on either id being unmapped, so MR-4 breaks no E2 assertion.
- **Positive live-table check** (re-review 2, root item 1, replacing D2's old flip item):
  - After D2, add a pytest asserting that `WK.rule_flags([{"name": "ew-theme-std-v1"}], {"ic": ic_table(False)})`
    equals `({'fit': {'--composition': 'ew-theme-std-v1'}}, [])`, and run `new-combination-rule.json` to the spec stage
    on the live table.
  - New with E2-10: run `leverage-change.json` (2.0, at both the cap and the table's maximum) and `new-nav-rule.json`
    to preflight on C1's live NAV table. A NAV table that lists no fixed-v1 `aim_leverage` range now refuses every
    leverage wave.
- **C2's two lines in `wave_stage_record.py`** (C2 R4, the verify pin of the cell of record). Round 3 does not touch
  `wave_stage_record.py`: `git diff d0054b32 HEAD` is empty for it. So the C2 re-review-2 finding ("merge cleanly with
  E2's `d0054b32`") holds for this head. Root places them at the C2 / E2 merge.
- Re-review 2's "What root must check at merge" items 3-12 still hold. Re-review this fix SHA (item 13).

## Fix round 4
Review fixed: `task-E2-rereview-3.md` (`f525b195`, CHANGES_REQUIRED: R3a + S1-S4) on code head `ea7d9dde`. Rulings
applied: E2-14 (merge root first; R3a), E2-15 (S1 Required: the reference side; S2, S4 fixed; S3 a report edit).
E2-8, E2-10, E2-12, E2-13, W1-I2, MR-4 and C2-8 still bind. History, nothing squashed, no rebase: merge `d2cea6a6`,
code `ce8c4f43`, then this report commit.

### The merge (ruling E2-14)
- **Root sha merged: `42f42a07`** (pool-2's head as found; it contains `c2416d62`: C2, AL-COMB, S2R, B2 identity part).
  `git merge --no-ff --no-commit 42f42a07` in pool-17, merge base `1239a5ff`.
- **Conflicted files: 0.** git's automatic merge tree is `0f746a16` (equal to `git merge-tree --write-tree`, run
  first). Five files changed on both sides, all in disjoint hunks, all auto-merged:
  - `.gitattributes`: both sides added the same three `scripts/specs/** text eol=lf` lines.
  - `scripts/tests/test_no_python_mirror.py`: root's D3 / P10 owner edits and E2's `require_research_seal` row.
  - `scripts/wave_result.py`: C2's `record_spec` / `run_spec` cell fields and E2's exe-notes wording.
  - `scripts/wave_stage_cell.py`: C2's engine branch, calibrated / record docs and `match_engine`; E2's Python-branch
    wording.
  - `scripts/wave_stage_record.py`: B2's ledger head and C2's R4 pin; E2's W1-I2 (a) branch, `verify_plan` and
    `side_by_side`.
- **C2's two R4 lines survive unchanged** in `wave_stage_record.verify_inputs` (`if mt.get("record_spec"): ...
  now["record_spec_digest"] = ...`). So does C2's `wave_result.py` edit (`cell.spec` = `record_spec` when present;
  `run_spec` / `run_spec_digest`). AL-COMB / C2 / D2 / COV / B2 code outside E2's files is root's, unchanged.
- **In the merge commit, on top of `0f746a16`** (`git diff 0f746a16 d2cea6a6` is exactly these: 2 files, 5 insertions, 2 deletions):
  - **MR-4**: `COMPOSITION_FLAGS` gains `"theme-cov-hedge-v1": {"fit": {"--theme-hedge": "theme-cov-hedge-v1"}}` and
    `"mom-volman-v1": {"fit": {"--mom-volman": "mom-volman-v1"}}` (the fitter's flags, `fit_composition_weights.py`
    `:221-222`). E2-12's `zz-unmapped-rule-v0` stays unmapped; its assert still passes.
  - **C2 S13** (it sits in E2's `wave_stage_cell.py`): the engine branch of `run_stage` logs E2's two-line exe-notes
    wording and carries E2's `# ruling E1-REUSE-a2: outputs made by another exe than on disk now` comment, as the
    Python branch.
- **Review item 5 (d)**, C2 left it to E2: `wave_manifest.py`'s doc line names `"pm6-6" | "pm6-6-engine" | "none"`
  (in `ce8c4f43`).
- **One test expectation moved with the merge** (in `ce8c4f43`; not a hash, golden or pin): in
  `test_rule_flags_from_the_tables`, `abbreviated("--the", table_flags(...)["fit"])` now also lists `--theme-hedge`.
  The test reads the committed IC table (`composition_rules_list.json`), which lists AL-COMB's `theme-cov-hedge-v1`
  since the merge, and MR-4 maps it. It was the only red of the post-merge kinds / wave / driver run (121 passed, 1
  failed, `logs/e2f4-postmerge.log`).

### Per finding
| item | state | change (`ce8c4f43`) | tests (`scripts/tests/test_wave_kinds.py`) |
|---|---|---|---|
| R3a (E2-14) | DONE | `wave_kinds.MATCH_FLAGS = ("--calibrate-gross", "--calibrate-tolerance")`, `MATCH_STAGE = "pm6-6-engine"`, `match_flag_reads(token)`: the flags the token reads as after `norm_option` (case, single dash), `=value` stripped, equal or a proper prefix (abbreviation). `wave_stage_kinds`: `match_flag_map_hits` (every option map key or value, parent-value maps too; read BEFORE `resolve`, because `apply_flags` refuses a single-dash key or a missing parent value without naming the flag) and `match_flag_list_hits` (every token of every resolved flag list: `ic` / `ic.w_flags` / `fit` / `nav` / `card`, plus a ref's own flags; route via `touching`, or "the parent spec <path> <list> (the cell inherits it)"). Library kinds: the parent's lists (add-alpha's copy). One refusal, `match_flag_refusal`, exit 3, naming token and route and citing E2-14 / E2-10. The match stage's `-cal` recipe is written after preflight and never read by the guard | `R3A_PROBES` (13): constants.flags, template change.flags, constants.set nav.flags, horizon.flags, a leverage wave's map; `--Calibrate-Gross=0.95`, `-calibrate-tolerance`, `--calib` (both), a value slot, a parent-value map; `fit.flags`, a whole `nav` section, template change.set. `test_match_flags_inherited_from_the_parent_are_refused` (a library kind and a rule kind; the reader's spellings and non-hits). `test_the_match_stage_still_writes_its_own_leverage[pm6-6 / pm6-6-engine]` |
| E2-15 (b) (S1) | DONE | `ref_refusal`: any raw route to `ref` / `ref.*` is refused (rule_cell.constants.set / .flags, the template's change.set `ref.*`, change.unset, change.flags `ref`, a literal top-level `ref`), except the template's own `change.set ref` whole (`TEMPLATE_REF`; the manifest pins the template's bytes). Called before `resolve`, so a literal key is named | `E2_15_ROUTES` (10, incl. a leverage wave and the manifest replacing the template's own block); `test_the_templates_own_ref_block_is_allowed` (passes to spec) |
| E2-15 (a) | DONE | The resolved reference side (`ref_step`: the cell's nav with the ref's own rule / leverage / flags) is judged as the cell's: a ref's own flags join the E2-8 comparison as `ref.flags` against the parent's `nav.flags` with only the derived flags; a ref's own leverage must equal the cell's required leverage (`leverage_refusal`, E2-10); its flags are read for match flags (E2-14). A ref without its own flags / leverage inherits the cell's, already judged | `E2_15_SIDE` (5): L 5.0, an unnamed `--vol-target`, `--aim-leverage 5.0`, `--Vol-Tar`, `--calibrate-gross` |
| E2-15 (c) | DONE | Probed first (below): W1-I2 (a) WAS bypassed through a ref. `exe_problem(exe, pins, rule_cell)`; verify passes `SK.rule_cell_kind(w)` (v2 kinds rule, horizon, role, leverage): a v2 rule cell is judged by the W1-I2 (a) branch whatever its ref phase, because `summ` pairs it with `inputs.reference_cell` (the parent's NAV), never with its ref, and `resolve` drops the parent's `compare`. The refusal adds "(its template's own ref construction, ref ran, is not its paired reference: ruling E2-15 (c))". The v2 verify plan line (`V2_EXE_TEXT`) says so; v1 is unchanged | `test_w1_i2_a_holds_through_a_ref` (4: no ref, constants.set ref, template's own ref, like-with-like control); `test_verify_plan_line_states_the_rule_cell_refusal` (the v2 line's new text; the v1 line byte-equal as before) |
| S2 | DONE | `schema_problems`: a non-finite float (NaN, Infinity, -Infinity) is refused before any range keyword ("is not a finite number") | `test_non_finite_params_are_refused_at_preflight` (unit + NaN / Infinity `aim_leverage` at preflight) |
| S4 | DONE | `raw_routes` reads a dict-typed `change.unset` by key (as `research_spec.resolve` iterates it) | 1 row in `E2_10_PROBES` (`{"nav.leverage": true}`), 1 in `E2_15_ROUTES` |
| S3 | DONE (report) | Fix round 3's E2-13 (b) observation is widened in place (marked): any named NAV rule whose params give `aim_leverage` writes a literal over `{leverage}` (fixed-v1 always, vol-target-v1 / risk-target-v1 when given); both modes fail closed (Python: the matched run repeats the calibration argv; engine: `match_engine`'s `leverage_requested` check, EXIT_PIN); a P9 template's NAV rule under gross matching omits `aim_leverage` | - |

### The probe before the fix (E2-15 (c) asks for it)
A scratch script (not committed) drove this lane's fake-world helpers on the merge head `d2cea6a6`
(`logs/e2f4-probe-before.log`; the same script after the fix: `logs/e2f4-probe-after.log`):
- R3a: `--calibrate-gross` by constants.flags (horizon, `gross_match` none), template change.flags (composition rule),
  constants.set nav.flags (role), horizon.flags, a leverage wave's map, and `--Calibrate-Gross=0.95` on pm6-6: all exit
  0 at preflight. After: all exit 3, naming token and route.
- E2-15 (b): `ref` (L 5.0, an unnamed `--vol-target`) by constants.set (horizon) and by the template's change.set
  (composition rule): both passed preflight and spec. After: exit 3, by (b) for the manifest route and by (a) for the
  template's own block.
- W1-I2 (a) through a ref: parent NAV receipt by exe `a`, the cell's NAV and its ref by exe `b`, gross_match none,
  horizon. No ref: verify exit 4 (the control). A ref by constants.set: verify **exit 0**, `nav_exe` `{equal: false,
  ref: "ran"}`. A ref by the template's own block: verify **exit 0**, the same. So the bypass was real on both routes.
  After: the constants.set route stops at preflight (exit 3); the template's own block reaches verify and is refused
  (exit 4, "ruling E2-15 (c)"). The test pins all four cases.

### Evidence
Run in `C:/atx-wt/pool-17`; code at `ce8c4f43`, tree clean. Explicit paths, `PYTHONDONTWRITEBYTECODE=1`,
`-p no:cacheprovider`, each `--basetemp` its own dir in the session scratchpad, SEPARATE invocations, one after the
other. Interpreter `C:/Program Files/Python312/python.exe`. Logs: `.superpowers/sdd/platform-p9-20261003/logs/e2f4-*.log`
(git-ignored).
```
PYTHONHASHSEED=0 ... --basetemp=<scratch>/e2r4-suites/bt-s0 scripts/tests
-> 2 failed, 510 passed, 4 skipped in 608.29s (0:10:08); exit 1     (the 2: root's red, below)
PYTHONHASHSEED=0 ... --basetemp=<scratch>/e2r4-suites/bt-c0 atx-engine/tools/test_stage_chain.py
-> 9 passed in 0.39s; exit 0
PYTHONHASHSEED=1 ... --basetemp=<scratch>/e2r4-suites/bt-s1 scripts/tests
-> 2 failed, 510 passed, 4 skipped in 577.98s (0:09:37); exit 1     (the same 2: root's red)
PYTHONHASHSEED=1 ... --basetemp=<scratch>/e2r4-suites/bt-c1 atx-engine/tools/test_stage_chain.py
-> 9 passed in 0.34s; exit 0
PYTHONHASHSEED=0 ... --basetemp=<scratch>/e2r4-suites/bt-i0 atx-engine/tools/test_research_store_identity.py
-> 15 passed, 1 skipped, 6 subtests passed in 1.21s; exit 0
PYTHONHASHSEED=1 ... --basetemp=<scratch>/e2r4-suites/bt-i1 atx-engine/tools/test_research_store_identity.py
-> 15 passed, 1 skipped, 6 subtests passed in 1.38s; exit 0      (the skip: atx-research-store.exe not built)
```
- **Counts.** Collected 462 at `ea7d9dde` (458 + 4 skip), 516 now (`--collect-only`, node ids compared): +11
  `test_p9_al_sig_registrations.py` (root, AL-SIG), +5 `test_research_wave.py` (root, C2), +38 mine in
  `test_wave_kinds.py` (13 R3a, 1 inherited, 2 match stage, 10 + 5 + 1 E2-15 (b) / (a), 4 E2-15 (c), 1 S2, 1 S4 row).
  The one node id "removed" is renamed, not lost: the S4 row shares its parametrize id with the list-typed unset row,
  so pytest suffixed both. Every root test id is present on the head. The same 4 live-input skips as rounds 1-3
  (`ATX_EQUITY_BIN` x1, `RESEARCH_CYCLE_LIVE_ROOT` x3). No skip / xfail added; no hash, golden or pin edited.
- **The 2 reds are root's, attributed:** `test_research_spec.py::test_e27b_reads_what_the_fitter_and_nav_summ_parse`
  and `::test_the_whole_file_passes_with_a_generated_spec_present` (it re-runs the file in a subprocess; that run's
  only failure is e27b). D2 added `--composition-exe` to the fitter (`composition_engine_fit.py:124`), so every proper
  prefix of `--composition` (the test's `--compo`, `--compo=...`, `--c` spellings) is now ambiguous to the fitter's
  argparse (exit 2). The same test **fails on root's own code tree `42f42a07`** (git-archive copy, the identical
  message), passes on `ea7d9dde`, and fails identically on `ce8c4f43`. No line of it, of the fitter or of E-27b's
  guard is this round's. Not edited here: the test's expectation is about the fitter's abbreviations, and the
  fitter's `allow_abbrev` is D3's (ruling E2-9). The guard still refuses those spellings (fail closed). Listed for
  root (below).
- **Which `atx-engine/tools` / `atx-impl/tools` tests ran, and why.** Only two tests there import a module this lane
  changed (`grep` of their imports): `test_stage_chain.py` (`stage_chain`) and root's merge-changed
  `test_research_store_identity.py` (`cycle_resume`, `stage_chain`, `wave_context`, `wave_queue`). Both ran under
  both seeds. No `atx-impl/tools` test imports a lane module, so none ran.

**Teeth.** A scratch runner (not committed) applied one textual mutation at a time to the working tree, ran the named
tests (`-k`), and wrote the file back byte for byte (SHA-256 checked; `git status` clean after each;
`logs/e2f4-teeth.log`).

| label | mutation | result |
|---|---|---|
| T1 | option maps not read for match flags | 5 of 13 R3a probes fail (the other 8 are still refused by the resolved-list layer, 3 of them map routes named by `touching`: defense in depth) |
| T2 | resolved flag lists not read (rule kinds) | 7 failed (set routes, inherited, the ref's own flags) |
| T3 | a library kind's inherited lists not read | 1 failed (the inherited test) |
| T4 | `match_flag_reads` exact spelling only | 5 failed (case / dash / `=value` / abbreviation rows, the reader asserts) |
| T5 | `ref_refusal`'s route check removed | 10 failed: 9 of the 10 `E2_15_ROUTES` rows and `w1_i2[constants]` (the literal-key row has its own line: T14) |
| T6 | a ref's own flags not in the E2-8 comparison | 3 failed |
| T7 | a ref's own leverage not judged | 1 failed (L 5.0) |
| T8 | a ref's own flags not read for match flags | 1 failed |
| T9 | `exe_problem` back to `ref == "none"` only | 1 failed (`w1_i2[template-a]`: verify exit 0) |
| T10 | the non-finite check removed | 1 failed |
| T11 | `change.unset` back to lists only | 2 failed (the two dict-typed rows) |
| T12 | MR-4's `theme-cov-hedge-v1` row removed | 5 failed: the tables test, and the 4 `--until spec` fixture runs of `test_rule_kinds_write_the_derived_constants`, which stop on N1 (the committed IC table lists the row) |
| T13 | parent-value map values not read | 1 failed |
| T14 | the literal top-level `ref` check removed | 1 failed (`resolve` still refuses the unknown key, without the E2-15 route) |

**v1 plan output, byte-unchanged.** `research_wave.py plan` on the 5 committed v1 manifests (`y-2`, `y-3`, `y-5`,
`y-s`, `y-s.head`; `--root` pool-17, whose `build-equity` holds no data) from git-archive copies of `ea7d9dde`,
`d2cea6a6`, root's `42f42a07` and `ce8c4f43`: all four outputs byte-identical per manifest (4 at exit 0, `y-s.head`
at exit 2 in every tree).

**No committed spec newly refused.**
- Scan of all 109 JSON files under `scripts/specs/` (47 templates): no token reads as a match flag; no template sets,
  unsets or flags `ref`, `compare` or `static_check`, and none has a literal `ref`.
- To preflight in a fake world, on the `d2cea6a6` and `ce8c4f43` code trees (`logs/e2f4-preflight-*.log`), identical
  results:

| P9 spec | preflight |
|---|---|
| `kinds/leverage-change.json`, `new-combination-rule.json`, `new-field.json`, `new-horizon.json`, `new-nav-rule.json`, `new-signal.json`, `new-universe.json`, `replaced-signal.json` | exit 0 (8 runnable) |
| `kinds/new-cost-model.json` | exit 3, as before: `cost-sqrt-v2` in neither table (C3) |
| `kinds/walk-forward.json` | exit 3, as before: schema only (D3) |
| `templates/mom-volman.json` (AL-COMB) as a v2 kind rule wave `rules: [mom-volman-v1]`, `pm6-6-engine` | exit 0 (MR-4 derives `fit --mom-volman`) |
| `templates/theme-cov-hedge.json` (AL-COMB) as a v2 kind rule wave `rules: [theme-cov-hedge-v1]`, `pm6-6-engine` | exit 0 (MR-4 derives `fit --theme-hedge`) |

The AL-COMB templates carry `requires` and `parent: null`, so a real wave stops at run until root fills them; the probe
re-roots them on the fake parent. `diagnostics/`, `rules/`, `candidates/` and `blocked/` hold no wave or template.

### Decisions
- **Match flags are refused absolutely, not relative to the parent.** E2-8 compares owned flags with the parent's; a
  match flag is refused even when the parent carries it (a `-cal` run recipe named as a parent instead of its cell of
  record would otherwise pass). No committed parent carries one.
- **In every section and in every spelling**, not only `nav`: "only the gross-match stage may put them on an argv".
  The scan shows no committed token is a prefix of either flag, so the abbreviation rule refuses nothing committed.
- **"The template's own pinned ref block" = the template file's `change.set ref`, whole.** `ref` is not a template
  key, so this is the only form a template can carry. A dotted `ref.*` edit, even in the template, is refused (it
  edits a section; without the whole block it would build a partial ref).
- **E2-15 (c) is keyed on the v2 kind, not on the ref state.** After (b), a v2 rule cell's ref can only be the
  template's own, and the probe showed that one still bypassed W1-I2 (a). A v1 rule wave keeps today's verify
  behaviour: v1 is unchanged by contract, no committed template sets `ref`, and completed v1 waves are not re-run.
- **`compare` / `static_check` are not refused** (the reviewer's alternative fix): E2-15 names `ref`; adding them
  changes no NAV, and with (c) a ref proves nothing for a rule cell either way.
- **`inputs.reference_cell` is already closed**: `wave_stage_library.cell_out` refuses, at the spec stage, a cell
  whose paired reference is not the parent's NAV.

### Not fixed / open
- Root's red above (D2's `--composition-exe` vs E-27b's abbreviation spellings): root / D3 (`allow_abbrev`, E2-9).
- Unchanged: N5 (E2-9), S8 / S6's role seal (E2-7), the stale v1 verify line (E2-6), E2-11.
- One slip: my first post-merge run used the Bash `python`, which resolves to `C:/atx/atx-db/.venv` (atx-db's venv).
  It only failed collection (`psutil` missing) under `PYTHONDONTWRITEBYTECODE=1`, wrote nothing and touched no atx-db
  file. Every later run used `C:/Program Files/Python312/python.exe`.
- Two teeth-runner slips, fixed in place: its first pass errored on every row (the `--basetemp` parent did not exist;
  nothing was measured; every file was written back), and its second pass silently skipped T1, T2, T8 and T12 (their
  patterns held `\n`, the working tree is CRLF under `core.autocrlf`). Those four were re-run with CRLF-aware matching.
  The table holds valid runs only; `git status` was clean after every pass.
- The "before" / "after" probe logs were regenerated on git-archive copies of `d2cea6a6` and `ce8c4f43` (the first
  live "before" run aborted on its template row; the first "after" run predated a refactor). The results match the
  live runs.
- No permission refusal this round (no `git restore`; the teeth runner writes the file's own bytes back).

### What root must still check at its merge
1. **The positive live-table check.** E2's frozen `D1_SCHEMAS` is D1's shape on purpose. Check that the merged IC exe's
   `--list-rules --json` is the committed `composition_rules_list.json` that E2's tests read (AL-COMB checked
   `--list-rules` = fixture at p9-2f). Then:
   - `WK.unmapped_ic_rules` on that live table is `[]` (MR-4 is in this branch);
   - `WK.rule_flags([{"name": "ew-theme-std-v1"}], {"ic": <live>})` gives `({'fit': {'--composition':
     'ew-theme-std-v1'}}, [])`;
   - `new-combination-rule.json` to the spec stage on the live table;
   - `leverage-change.json` (2.0) and `new-nav-rule.json` to preflight on C1's live NAV table.
2. **The engine stage on real exes.** The lane proved P9-C's shape on `pm6-6-engine` in the fake world only
   (`test_the_match_stage_still_writes_its_own_leverage`: preflight passes with the reservation, the `-cal` argv
   carries the cell's L and `--calibrate-gross`, the `-gm` record is plain at L' to 4 dp, verify pins
   `record_spec_digest`). On real exes:
   - the `-cal` NAV calibrates and the `leverage_requested` check passes;
   - a named NAV rule with `aim_leverage` stops at match (EXIT_PIN);
   - C3's real trade-rate rows: if a row's x-flag is `--rule <id>`, it duplicates `nav_step`'s `--rule nav.rule`.
3. **Root's red**: `test_research_spec.py` e27b (and its whole-file re-run). It is red on root's own head; E2's merge
   neither causes nor fixes it.
4. **What this merge already did.** C2's R4 lines, C2's `wave_result.py` edit, MR-4 and S13 are in this branch, so at
   root's merge of E2 they are a re-proof, not hand-placed hunks. Root's own commits after `42f42a07` merge on top as
   usual.
5. Re-review 2's items 3-12 still hold (B2-12, the `atx.research-wave/v2` literal in `classes.json`, W1-I1 margins,
   PM7-35's Python copy, no merge while a wave runs, W1-I2 (b) sequencing, separate pytest invocations, the
   `test_cycle_e2e` margin, `allow_abbrev` to D3).
6. Re-review `ce8c4f43` (R3a, E2-15, S2, S4) and the merge `d2cea6a6` (its 5 + 2 lines over `0f746a16`).
