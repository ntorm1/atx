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
5. Intended argv change (Ruling E2-1): a resumed cell with an existing `-run2+` IC / NAV / fit dir now plans
   `--attempt k` in its runner head. That is the only plan change.

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
