# Lane E2 review

## Verdict
**CHANGES_REQUIRED** (2 Required, 9 Suggested). Spec verdict: compliant except R1 / R2. Quality verdict: good; the
split, the runner `--attempt k`, the judge ordering and the record writers are sound, and the tests are real.
R1 (seal coverage of the new build run dirs) is the one that matters. R2 is a fail-open in the v2 `rules:` guard.
Both fixes are small (a few lines each, plus a test).

## Reviewed SHA
Head `5ca50ef5` (code `defa34c2`), base `1239a5ff`, branch `feat/p9-e2-20261003`, pool-17. Every fix needs a new
review.

## Evidence
Run inside `C:/atx-wt/pool-17` at `5ca50ef5`, tree clean, explicit paths, `PYTHONDONTWRITEBYTECODE=1`,
`-p no:cacheprovider`, `--basetemp` in a named scratch dir outside any git repo (removed after). `scripts/tests` and
`test_stage_chain.py` were run as SEPARATE invocations, per the conftest window trap.
```
PYTHONHASHSEED=0 python -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/e2-review-pt-s0 scripts/tests
-> SKIPPED test_cycle_e2e.py:439 (ATX_EQUITY_BIN), test_research_cycle.py:56/:706/:946 (RESEARCH_CYCLE_LIVE_ROOT)
-> 420 passed, 4 skipped in 879.03s (0:14:39); exit 0
PYTHONHASHSEED=0 python -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/e2-review-pt-c0 atx-engine/tools/test_stage_chain.py
-> 9 passed in 0.56s; exit 0
PYTHONHASHSEED=1 python -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/e2-review-pt-s1 scripts/tests
-> (same 4 skips) 420 passed, 4 skipped in 897.02s (0:14:57); exit 0
PYTHONHASHSEED=1 python -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/e2-review-pt-c1 atx-engine/tools/test_stage_chain.py
-> 9 passed in 0.75s; exit 0
```
The 4 skips are pre-existing live-input gates (no skip / xfail mark was added by the lane:
`grep '^+.*(skip|xfail|importorskip)'` over the diff is empty).

Independent checks (read-only, synthetic, nothing dated >= 2024-01-01 opened):
- **Move-only split.** I compared AST source segments of every top-level def/assign in `research_cycle.py` at
  `7e4573d5` (the split's parent) against `scripts/cycle/*.py` + `research_common.py` at `4d6699eb`. Of 98 defs, 96
  are byte-identical and none is missing. The 2 that differ are only the path anchors: `find_spec`
  `Path(__file__).parent` -> `SCRIPTS_DIR`, and `main --root` `parents[1]` -> `SCRIPTS_DIR.parent`, which are equal by
  construction. The `--help` text (the `cycle/cli.py` docstring) is byte-equal to the base module docstring (18,725
  chars). Every base def is re-exported by the facade. The 10 names it no longer exposes (`json`, `os`, `re`,
  `subprocess`, `hashlib`, `time`, `shutil`, `argparse`, `ThreadPoolExecutor`, `_write_verdict`) are read as
  `RC.<name>` nowhere in scripts/, atx-engine/tools or atx-impl/tools. `cycle/` has no `global` rebinding. Every
  entry module imports first in a fresh interpreter (no import cycle). No other `cycle` module is on any path.
- **Post-split diff** (`4d6699eb..5ca50ef5` over cycle/, read line by line). `run_dir_of` / `free_run_dir` yield the
  same strings as the inline f-strings they replace (`-run`, `-run<k>`; nav `k == 1` -> `-run`). `theme_registry` is
  `([], [])` without the input. `ledger_state` now reads, then takes the head (it was head, then read): both run
  `_walk` on the same file, so on success and on failure the result is byte-equal.
- **D1_SCHEMAS (defa34c2)** equals `params_schema` of both rows of `atx-impl/tests/fixtures/composition_rules_list.json`
  at `1239a5ff`, verbatim. The lane did not touch that fixture.
- **M1a-RED.** The `added_in` set of `registry.json` at `a5df67ae` is exactly `REGISTRY_OF_RECORD_WAVES`. The live
  registry filtered by it equals the lock registry (102 alphas, ids + dsl, same order); the only extra wave is `v8ys`.
  `v8ys` reads `ea_window_pre5`, `ins_cluster_buy` and `noa_lag4` (the failing three), and none of the 10 mined fields.
- **Fail-closed probes of `WM.validate` / `WK.schema_problems`** (scratch script, fixtures edited in memory). These are
  refused: no kind, kind as a list, kind null; `rules` empty, a string, with an extra key, with params as a list, or
  with a duplicate name; a block of another kind; a horizon flag value that is an int; `field.output` `./scripts/x` or
  `SCRIPTS/x`; `role.output` absolute or in `atx-engine/`; walk_forward `window: rolling`; schema v3; a v1 manifest
  carrying `rules` or `leverage`. Fail-open cases: R2, S3, S5 below.
- `git ls-files --eol scripts/specs`: all 101 blobs `i/lf`, so the `eol=lf` rule renormalises nothing.
- No committed spec carries `exes_sha256` (`git grep` over scripts/specs is empty), so the sticky-pin rule changes no
  committed v8 manifest's plan.
- **R1 demo** (fake world from `test_wave_kinds` helpers, scratch root outside git). A `new-field.json` wave run to
  `register` creates `out/waves/k-field/builds/fields-run1`. I wrote a `stdout.log` there containing
  "read rows through 2024-06-28". `wave_seal.wave_logs(w, {})` then returns only `consoles/001..004-*.log`:
  `any build log scanned: False`.

## Spec compliance (brief + wave2-carry "E2" + rulings), item by item
| item | verdict |
|---|---|
| M1a-RED (required) | PASS. The root cause is stated and verified above. The constant is derived from the registry at the lock (prereg `docs/plans/2026-10-01-v9-mine-campaign-prereg.md:70`, "applied at the lock to the registry of record"), not from the failing set; `EXCLUDED_BY_CLASS` is unchanged. Note: freezing the read set is right only because C1 already ran (`.superpowers/sdd/platform-v8-20260929/progress.md:2176`). The grant-time re-application at prereg `:196` / runbook `:57` would otherwise need the live registry. A future campaign needs its own registration and test. |
| E1-N3 (required) | PASS. `Cycle.introducer` walks the whole chain (`cycle/phases.py`), and a 3-level G -> P -> C test covers both a matching and a moved ancestor pin. |
| Move-only split, `research_common.py`, run-dir naming once (K-P9-1, OR section 4) | PASS (evidence above). See R1: the new kinds add run dirs that the seal scan does not read. |
| `atx.research-wave/v2` (K-P9-9); `rules:` against the exes' list-rules JSON (D1-PSCHEMA) | PASS with R2. Params are checked against the `params_schema` read at run time, never against a local copy. |
| kinds field / role / horizon / leverage / walk-forward (schema only) | PASS. 10 fixtures: 8 runnable + 2 schema-only (P14). |
| Capabilities from `--list-rules --json` | PASS. Consumers only test membership (`no-composition`, `marginal`). See root check 2. |
| `--candidates` | PASS: S1 built it (`marginal.candidates` -> `MARGINAL_BUILT`, `cycle/spec.py:55-59`), which is the brief's "if S1 did not" case. |
| P9 / PM7-35 Python copy | Correctly NOT retired (carry: only after root's identity vs B1). |
| D1 deviation (`--theme-registry`) | PASS. The flag and its bind come from the input only, and the test asserts nothing else moved. |
| R0-7-EOL | PASS. |
| A2 note (G-P5 row) | PASS. `require_research_seal` exists at `prepare_research_fields.py:2979`, and `reuse.hpp` exists. Cross-lane, listed. |
| SQL-11 | PASS. No SQLite index flag. The writers are in `scripts/cycle/gate.py` and named in the report. |
| Carried E1 minors | N4 PASS (neutral wording). HostClaims PASS with S1. Sticky pins PASS (S7 documents it). Nested time keys PASS. Judge order PASS for consoles and process rows; the driver log stays live by design, which is a deliberate partial of the carry's "log" (accepted). `attempt` / `build_type` PASS (E2-1). |
| E2-1 / E2-2/3 | PASS. The post-merge commands are listed, and no test is weakened or skip-marked. |

## Findings
`path:line | severity | problem | required fix`

**Required**

- **R1** `scripts/wave_seal.py:72-84` (with `scripts/wave_stage_kinds.py:166-173`) | major (seal coverage) |
  `wave_logs` scans the cell phase run dirs, the bundle, the readers and the consoles. It does not scan
  `<out_dir>/builds/<what>-run<k>`, the dirs the new `field` / `role` kinds create (`run_build` ->
  `free_run_dir(w.wave_path("builds", what))`). Those runs are `prepare_research_fields.py` and
  `prepare_recent_research.py role` on real data. Their `stdout.log` / `stderr.log` are never seal-scanned, by verify
  (`wave_stage_record.py:41`) or by record (`:223`). The consoles hold only the runner's own output. This is the
  failure mode the brief cites (OR section 4: "a rename silently shrinks the seal scan"), arrived at by adding new run
  dirs. It also makes the verify plan line ("seal scan of every log the wave produced") false for v2 field / role
  waves. | Add the build bases to `wave_logs`, e.g. `for what in ("fields", "role", "role-fields"): dirs +=
  w.run_dirs(w.wave_path("builds", what))`, ideally with the names defined once in `wave_stage_kinds`. Add a test: a
  field-kind wave whose fake build writes a sealed date into its run dir's stdout.log is refused by verify.

- **R2** `scripts/wave_stage_kinds.py:73-81` (with `scripts/wave_kinds.py:154-159`, `:434-444`) | minor (fail-open
  validation) | The module contract (`wave_kinds.py:8-10`) says the rule flags "come from the exes' rule tables, never
  from rule_cell.constants.flags nor the template's change.flags (review F-8)". Validation refuses a raw flag in
  `constants.flags` only when it is a static `COMPOSITION_FLAGS` flag (`rules_problems`). Preflight's `owned_flags`
  checks only the template's `change.flags`. So a NAV rule's x-flag written raw in `rule_cell.constants.flags`
  (probe: `new-nav-rule.json` with `constants.flags.nav["--aim-leverage"] = "2.0"`) is ACCEPTED, and then
  `merged_constants` silently replaces it with the derived `1.5`. A raw x-flag of a NAV row the manifest does not
  name (e.g. `--vol-target vol-target-v1`) passes too. That rule then runs with no `params_schema` check, which is
  the bypass D1-PSCHEMA exists to close. | In `preflight_kind` (kind rule), refuse every `rule_cell.constants.flags`
  entry that `owned_flags` would refuse, i.e. also run `WK.owned_flags(flags, {"change": {"flags":
  rc.constants.flags}})`. Preferably refuse any flag that is an x-flag (rule or param) of any NAV row, or any
  `COMPOSITION_FLAGS` flag, whether or not that rule is named. Add the probe as a refusal test.

**Suggested**

- **S1** `scripts/run_bounded_research.py:226-231, 245-254` | minor | A persistent `PermissionError` on the lock's
  create (an unwritable or ACL-locked `--host-claims` dir) is treated as "held". `reclaim` returns True on
  `FileNotFoundError`, and also when `remove` failed, so the 3-reclaim budget is spent on nothing. The acquire then
  ends as `LockTimeout("host claims lock ... held for N s")` -> `prelaunch-admission-timeout`, a retryable
  AUTO_OUTCOME. `--auto-attempt` then retries a configuration error up to MAX_ATTEMPTS times, and the message says
  "held" when nothing holds the lock. Base surfaced it as runner-error. Every other lock case I traced maps
  truthfully: a dead holder is reclaimed; an abandoned unreadable lock older than 5 s is reclaimed; a fresh
  unreadable or live readable lock is held, bounded by min(30 s, wait left); on Windows a live writer's open handle
  blocks `remove`, so the unreadable path cannot steal a live lock (the double-reclaim race is parked, E2-4). | Do as
  `backtest_integrity.ledger_append:1117` does: when `PermissionError` persists to the deadline and `not
  lock.exists()`, raise a non-`LockTimeout` error ("cannot create ... not writable"). Make `reclaim` return True only
  when it actually removed the lock.
- **S2** `scripts/wave_context.py:164-170` | minor | Buffered consoles and process rows are written only after the
  `with ThreadPoolExecutor` block. A `KeyboardInterrupt` / `BaseException` in the main thread skips the replay, so
  consoles of finished commands are lost. On resume `bundle_once` (bundle.json exists) does not run again, so its
  console is never re-made. The run-dir logs are still scanned, so this is a record gap, not a seal gap. | Put the
  replay in `try/finally`.
- **S3** `scripts/wave_kinds.py:257-262, 284` | minor | `schema_problems` raises `TypeError` on a JSON-Schema type
  list (`"type": ["string", "null"]`), a crash instead of a problem. A non-object `properties` is silently ignored
  (`[]` -> accepted). | Return a problem for any `type` that is not a known string, and for a non-dict `properties` /
  `required` / `items`.
- **S4** `scripts/wave_kinds.py:81, 358-361` | minor | `PLACEHOLDER_RE = [A-Z]` with `fullmatch` treats only a
  single letter as a placeholder. C1's table today uses `S` (`strategy_risk_target.cpp:379`), but a row like
  `--risk-target SIGMA` would be emitted literally (probe: `{'nav': {'--risk-target': 'SIGMA'}}`). | Use
  `[A-Z][A-Z0-9_]*`.
- **S5** `scripts/wave_kinds.py:214` | minor | `field.output` `./<fields dir>` passes the "is the wave's fields dir"
  check (only `rstrip("/")` is applied). It fails closed later (`run_build` treats it as built, then `built()`
  refuses the missing rows), but with a misleading message. | Compare normalised paths (`Path(...).as_posix()` after
  dropping `.`).
- **S6** `scripts/wave_stage_kinds.py:147-173` | minor | `run_build` reuses an output dir whenever its
  `manifest.json` exists ("resumed"), without checking that it was built by this command: the registry, role and
  reuse pins, or this wave's `builds/` receipt. Two manifests naming one `field.output` silently share a build.
  `built(..., names=None)` (the role) checks neither `status` nor the seal. | Accept an existing output only when the
  latest `builds/<what>-run<k>/receipt.json` is completed with this command's `argv_sha256`, and check the role
  manifest's seal / end (the builder enforces it, but the wave should verify).
- **S7** `.superpowers/sdd/platform-p9-20261003/task-E2-report.md:169` | minor (report) | "That is the only plan
  change" is incomplete. `wave_stage_util.lock_exes` (`:48-57`) now also locks when the parent spec pins
  `exes_sha256` without `driver.lock_exes`, which adds `lock --exes --write` lines and pins to a v1 wave's plan and
  run. No committed manifest qualifies today (verified), but root should know it is intended. | List it in "How root
  verifies".
- **S8** `scripts/wave_kinds.py:100-103` | minor | `in_code` is a second copy of `wave_manifest.in_code_pathspec`
  (`:183-187`), in the lane whose job was removing such copies. The import cycle explains why. | Move the rule into
  `research_tree` and import it in both places.
- **S9** tests | minor | No test covers: the leverage-bounds refusal at preflight (`wave_stage_kinds.py:83-86`), the
  persistent-PermissionError lock (S1), or the build seal scan (R1). | Add them with R1 / R2.

## `defa34c2` (frozen D1 rows)
This is legitimate decoupling. The two assertions unit-test the validator (`schema_problems` + `checked_params`) on
D1's schema shape, and the frozen copies are verbatim. Without the freeze, D2's legitimate re-pin of the fixture
would turn them red. That would push someone to edit the expected messages, which is the outcome to avoid. What
they stop guarding is the claim that "the LIVE IC table refuses the manifest". That claim is supposed to become false
at D2, so nothing real is lost. Nothing in pytest yet asserts the positive: that the live table (after D2) accepts
`new-combination-rule.json`. The report's post-merge command covers it with the real exe. Root could add that live
check as a pytest at D2's merge.

## Other checks (no finding)
- **Trial accounting.** No E2 path appends to a ledger. Appends stay in nav_summ (summ phase) and
  `cycle_admission.ledger_admissions` (gate), both through `backtest_integrity.ledger_append`, deduped by a
  content-keyed `trial_id`. The construction id comes from the daily-series SHA; the admission id from candidate,
  dsl / library pin, role pin and window id. Neither involves an attempt or a run dir, so `--attempt k`,
  `--auto-attempt`, a resume or a judge retry cannot double-append. A refused preflight (v2 kind problems, the
  walk-forward refusal) happens before anything that runs a statistic: the listings are static tables, and
  `budget_check` / N only read. The runner's `output.mkdir(exist_ok=False)`, `free_run_dir`, explicit `-run<k>`
  (failed -> stop) and contiguous `attempt-k` dirs still mean no run dir is overwritten.
- **Identity (k = 1, and the command after `--`).** `runner()` adds `--attempt k` only for k >= 2, and before
  `--output`, where base's `retarget` already put it. `retarget` replaces the value, never adds a second `--attempt`.
  So an OR-4 sub-dir argv is unchanged for k = 1 run dirs. `argv_sha256` and every resume key
  (`check_receipt_argv`, `check_binding`, `write_binding`) use `step_args` = argv after `-- <exe>`, which does not
  include the head. `step_key` is the phase name. The wave digests no cycle plan, and nothing reads a receipt's
  `attempt`. I found no counterexample. Intended changes: a `-run<k>` (k >= 2) step, including an in-progress
  multi-pass fit (pass j is labelled attempt j, as documented in the runner docstring), and S7.
- **v1 manifests.** `kind()` is None, so every `SK.*` helper returns the v1 value. `cell_doc`'s new `is_template`
  guard cannot fire after v1 preflight's template check. `preflight_inputs` / `preflight` keep `template_sha256` for
  every v1 rule wave, because `rule_cell` is required there. The only v1 differences are the schema-error text, which
  now mentions v2, and S7.
- **Moved record writers** (`cycle/gate.py`: `write_verdict`, `admission_trials`, `copy_ledger`). These are
  AST-identical to base. `ledger_state`'s read order is equivalent, and `ledger_view` / preflight `ledger_state`
  return the same keys in the same order.
- **Judge.** `side_by_side` keeps `max_workers = 3` and the launches are unchanged, so each bounded process still
  holds its host claim. The driver additionally keeps at most the three calls' captured outputs until they all end.
  Those outputs were already fully in memory through `capture_output`. Results and the first failure come back in
  call order. `test_judge_records_in_call_order` forces reverse completion with events and compares the records.
- **Content digest.** `TIME_KEYS` at any depth also drops configured `seconds` caps echoed in outputs. These are
  benign, since the manifest SHA is an input.

## What root must check at merge
1. Run `scripts/tests` and `atx-engine/tools/test_stage_chain.py` as two pytest invocations (the conftest window
   trap). After R1 / R2, re-review the fix SHA.
2. Plan identity with the REAL IC exe present. The lane's hash-only identity run was done in a pool without built
   exes, where `exe_capabilities` returns `frozenset()` on both sides, so it never compared `--help` probing with
   `--list-rules --json`. Re-run the committed-spec `plan` and `plan --screen` identity, base vs head, with the
   `1239a5ff`-built IC exe on disk. Expect byte-identical output (the base fixture's capabilities include
   `no-composition` and `marginal`). Compare only the 130 original keys.
3. Do not merge under a running wave with `receipt_digest: "content"`. Nested time keys now move content digests, so
   an in-flight chain reads as stale (exit 3, E1-STALE).
4. The post-merge items in the report: D2's composition fixture, C1's NAV-rule and horizon fixtures, the tiny-world
   kinds runs, and the classes literal `atx.research-wave/v2`.
5. PM7-35's Python copy (`wave_rules.py:50-51`) is still live. Retire it only after the B1 identity.

## Checked
- [x] Python lane: `.agents/cpp/agent.md` §10 not applicable (no C++). The determinism, error-path and contract items
  were applied.
- [x] Diff stays inside the brief / P14 scope. The cross-lane edits (`.gitattributes`, `test_no_python_mirror.py`)
  are listed. `backtest_integrity.py` is untouched.
- [x] Report evidence matches the claims: the suite was re-run under seeds 0 and 1, and spot checks were re-derived
  (M1a-RED registry, D1 rows, move-only AST, EOL).
