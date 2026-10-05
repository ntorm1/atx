# Lane E2 re-review 1 (fix round 1)

## Verdict
**CHANGES_REQUIRED** (1 Required, 5 Suggested). Spec verdict: R1, W1-I1, W1-I2 (a), E2-5, E2-6, E2-7, S1-S7 and S9
are compliant. R2 is not closed (R2b). Quality verdict: good. The fixes are small and well placed, and the new tests
have teeth: R1's test fails when the build scan is removed (mutation probe below). R2b is the same fail-open as R2,
reached by two routes the fix does not check. A rule cell can still run an unnamed rule with no `params_schema` check.
One more check closes it.

## Reviewed SHA
Head `05a0fcd1` (WIP `fe140597`, handoff `f692ea36`), base of this round `5ca50ef5` (code `defa34c2`, review
`780a4934`), lane base `1239a5ff`, branch `feat/p9-e2-20261003`, pool-17. Read from the diff `5ca50ef5..05a0fcd1`
(15 files, +987 / -205; code part 1,030 lines), written to a scratch file and read from there. Every fix needs a new
review.

## Evidence
Run inside `C:/atx-wt/pool-17` at `05a0fcd1`, tree clean. Explicit paths, `PYTHONDONTWRITEBYTECODE=1`,
`-p no:cacheprovider`, `--basetemp` in this session's scratchpad (removed after). Only `scripts/tests` files, no
`atx-engine/tools` test in the same session.
```
python -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/pt-k scripts/tests/test_wave_kinds.py
-> 50 passed in 60.83s; exit 0
python -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/pt-d scripts/tests/test_wave_driver.py -k "cache_path_bound or parent_nav or lock_exes or claims_lock or judge_records or launch_waits or two_launches or adopt_never or k_p9_10"
-> 13 passed, 15 deselected in 43.23s; exit 0
python -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/pt-s scripts/tests/test_research_spec.py -k "add_alpha_on_a_v8 or add_alpha_child_of_a_labelled or whole_file_passes"
-> 4 passed, 83 deselected in 291.05s; exit 0   (the 3 stand_in_ic tests + the whole-file re-run)
```
The lane's full-suite logs (`logs/e2f1-{s0,s1,c0,c1}.log`) match the report: 434 passed / 4 skipped (the 4 live-input
gates of before) under seeds 0 and 1, and `test_stage_chain.py` 9/9 twice, as separate invocations. The added lines
add no skip / xfail mark and no ledger or trial-id code. Their only literals dated 2024 or later are the synthetic
T2-SYN ones (a fixture `seal.exclusive_end` of `2024-01-01` and the fake post-seal log line `2024-06-28`).

Probes (scratch pytest files outside the repo that import the lane's own fake-world helpers from
`scripts/tests/test_wave_kinds.py`; synthetic only, nothing dated 2024 or later opened, no 2020-2023 output read):
```
R1 mutation (wave_seal.build_bases -> []): test_build_logs_are_seal_scanned FAILS -> the test has teeth
R2 control  constants.flags {nav: {--vol-target: vol-target-v1}}           preflight exit 3 (refused, as fixed)
R2b         constants.set {nav.flags: [--vol-target, vol-target-v1]}        preflight 0, spec 0; cell change.set
            carries "nav.flags": ["--vol-target", "vol-target-v1"] (an unnamed NAV rule, no params_schema check)
R2b         constants.set {fit.flags: [--theme-erc, theme-erc-v1]}          preflight 0, spec 0 (unnamed composition)
R2b         constants.flags {fit: {--theme-erc=theme-erc-v1: true}}         preflight 0, spec 0
R2b         constants.flags {fit: {--theme-er: theme-erc-v1}}               preflight 0, spec 0
R2b         constants.flags {fit: {--composition=ew-theme-std-v1: true}}    preflight 0, spec 4 (research_cycle's
            R6B-C-4 guard, which covers --composition on v8 specs only; the stop comes after register)
fitter argparse (fit_composition_weights.parse_args, default allow_abbrev): "--composition ew-theme-v6
            --composition=ew-theme-std-v1" -> ew-theme-std-v1; "--compos ew-theme-std-v1" -> ew-theme-std-v1;
            "--theme-er theme-erc-v1" -> theme_erc = theme-erc-v1
raw_rule_flags with AL-COMB's IC table (pool-19 fixture, before MR-4): fit --theme-hedge -> [] (passes); naming
            theme-cov-hedge-v1 in rules -> refused ("no fitter flag in wave_kinds.COMPOSITION_FLAGS")
W1-I1 test depth: test_cache_path_bound_w1_i1's cycle half needs len(--basetemp) > 34
            (basetemp 31 chars -> deep 247 -> `assert deep > 250` fails; 58 chars -> 274 passes)
```

## Item by item

**(a) R1 seal scan of the builds: CLOSED.**
- `wave_seal.wave_logs` (`wave_seal.py:81-82`) adds `Wave.run_dirs` of every `wave_stage_kinds.build_bases(w)`.
  `run_dirs` lists `-run` and `-run<k>` in attempt order, each followed by its `attempt-k` sub-dirs
  (`wave_context.py:128-143`). So a retry (`free_run_dir` -> `-run2`) and an OR-4 attempt sub-dir are read.
- `run_build` refuses a name outside `BUILDS` (`wave_stage_kinds.py:185-186`). `BUILDS` is defined once and read by
  both the scan and the build, and `run_build` is the only writer under `<out_dir>/builds/`.
- The builders' own output dirs (`field.output`, `role.output`) are data, not logs. They are not scanned, as no
  cell phase output is.
- v1: no `builds/` dir exists, so `run_dirs` returns `[]` and the scan's file list and receipts are unchanged.
- The test fails with the scan removed (mutation probe above).
- The v2 field / role verify plan line names the builds.

**(b) R2 raw rule flags: NOT CLOSED (R2b).**
- The fix is fail-closed for the canonical spelling in both places it checks: `rule_cell.constants.flags` and the
  template's `change.flags`. It covers the named rules' derived flags and every table rule's flags: composition
  flags, NAV x-flags and NAV param x-flags (`wave_kinds.py:411-440`, `wave_stage_kinds.py:79-85`). `--cadence` stays
  free.
- Route 1 bypasses it with canonical spelling: `rule_cell.constants.set` (merged into `change.set` unchecked,
  `wave_steps.py:251-252`) and the template's `change.set` may set any dotted key (`research_spec.py:188-190`). That
  includes `nav.flags`, `fit.flags`, `ic.w_flags` or a whole section.
- Route 2 slips through the `fit` section in spellings the Python fitter's argparse reads: `--theme-erc=V`,
  `--theme-er V`. `raw_rule_flags` matches keys exactly, `apply_flags` accepts any key that starts with `--`, and
  `fit_composition_weights.py:2681` keeps argparse's `allow_abbrev`.
- The NAV and IC exes parse exact keys and refuse unknown or duplicate flags (`strategy_nav_replay.cpp:3318-3458`),
  so a non-canonical NAV / IC spelling fails loudly at run, not silently.
- Route 2 on a NAV-only template: the reused fit output then fails OR-3's receipt-argv check at run. On a template
  that renames fit (a composition rule), the unnamed rule runs.
- AL-COMB's flags (MR-4), before MR-4 lands:
  - Naming `theme-cov-hedge-v1` / `mom-volman-v1` is refused (fail closed).
  - A raw `--theme-hedge` / `--mom-volman` passes, because `table_flags` drops an IC row that `COMPOSITION_FLAGS`
    lacks (N1).
  - Today's IC table lists exactly the 8 `COMPOSITION_FLAGS` ids (pool-17 and D2's pool-20 fixtures), so the gap
    opens only if AL-COMB's C++ table is built without MR-4.

**(c) W1-I1: COMPLIANT.**
- **Layout.** The model matches the C++ (read at pool-17, S2's token at pool-18):
  - `cache_root` = `<dir>/<vm_identity>`. Release (equity-rel) identity:
    `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0`. S2 appends `_audit` (`strategy_ic_signal_cache.cpp`, pool-18
    `:107-111`).
  - `cache_key` = `/<role sha 64>[/fp_<16>]` (`:165-182`).
  - IC-result entry = `<entry dir>/ic<v>_<16>/<id>.<dsl16>.json` (`strategy_ic_runner.cpp:444-446`,
    `strategy_ic_result_cache.cpp:134`), with `ic_result_semantics_version = 1` (`:25`).
  - `PartialFile` = `.<16 hex>.partial` in the same dir, with no id in it (`strategy_ic_signal_cache.cpp:394-400`).
  - The marginal pair cache is shallower and id-free (`strategy_marginal_pair_cache.cpp:133, 169`).
- **The bound is conservative.**
  - `fp_` is always counted.
  - The Debug identity is the legacy root (one level fewer), so the Release bound covers it.
  - The equity-rel preset sets no AVX2 / FMA / fast-math.
  - `w_flags` are included in the audit-exact test.
  - `226 + len(id)` under root's v8 cache is re-derived exactly.
- **Before any compute.**
  - research_cycle calls `cache_path_check` inside `steps()` (`cycle/phases.py:564`). `run` re-resolves `steps()`
    before every phase (`cycle/cli.py:403`), so the first call refuses before the fields / ref / u phases.
  - The wave refuses at preflight (stage 01) before register (02). This holds for every shared-store parent (all 18
    v8 specs). It does not hold for per-version-store parents (N3).
- **Exe-on-disk gate (E2-5).** It tests `effective_exes(spec)["ic"]`, the same path the u / w / marginal argv
  launches. A real run cannot launch without that file, and `steps()` re-checks before u. So a refusal cannot be
  hidden from compute.
- **Plan.** A hash-only plan with no library on disk reads `None` -> no ids -> nothing refused, and the plan bytes
  are unchanged within the bound. The wave's `plan` uses `preflight_plan`, which does not call the check.
- **Id regex.** `CAND_ID_RE` stays `{0,63}`.

**(d) W1-I2 (a): COMPLIANT.**
- `exe_problem` (`wave_stage_record.py:81-88`) refuses a cell with `ref == "none"` in three cases: the receipts
  differ (`equal is False`), the pins name two NAV exes (`moved`), or exactly one side records an exe. Both hashes
  are in the message.
- "Neither side records" cannot be reached by a real run. The bounded runner has written
  `executable_sha256=digest(command[0])` into every receipt since its first commit (`257ffd3b`,
  `run_bounded_research.py:475`). Both NAVs are runner-made, so a missing hash means a non-runner NAV, which the
  one-sided refusal catches.
- `ref == "none"` covers every template cell: rule, leverage, horizon and role. `research_spec` drops `ref` from
  every template, and add-alpha library cells keep theirs. Every template cell pairs against the parent spec's own
  NAV, so nothing legitimate is refused.
- The refusal is keyed on the exe file hash. Any relink of `atx-equity-strategy-targets.exe` therefore refuses every
  template-cell wave until its parent NAV is re-run on that exe: a NAV-neutral merge, Debug vs Release, or another
  pool's build. That is the ruling as written; see root check 7.
- The 5-param test drives a real fake rule wave to verify.

**(e) S1-S7, S9: as claimed.**
- S1: `ClaimsDirError` is a RuntimeError, not a `LockTimeout`, so it is recorded as runner-error
  (`run_bounded_research.py:533`). `reclaim` is True only when it removed the lock, and `remove` never raises. The
  residual race is in N5.
- S2: the replay sits in `finally`.
- S3: a type list, an unknown type and malformed keyword values are refused.
- S4: `[A-Z][A-Z0-9_]*`. C1's real rows (`--aim-leverage`, `--vol-target vol-target-v1`, `--risk-target S`,
  pool-2 `strategy_risk_target.cpp`) read the same as before.
- S5: `_norm`; `..` is still refused by `_rel`.
- S6: a reused output must come from this wave's last `<what>` run, completed with this command's
  `argv_sha256(command[1:])`. That is the same digest the runner writes (`:475`), so a genuine resume is accepted.
- S7: the report lists the sticky-pin plan change.
- S9: the leverage-bounds, persistent PermissionError and build-seal tests are present.
- **`stand_in_ic`** (`test_research_spec.py:1162-1168`):
  - It is function-scoped and patches `cycle.phases.CACHE_PATH_MAX`, the global `cache_path_problem` reads at call
    time. `monkeypatch` undoes it after each test, so it cannot leak.
  - `RC.CACHE_PATH_MAX` is a separate binding, used only by an assertion in `test_wave_driver`.
  - The diff only adds three `@pytest.mark.usefixtures` lines; no assertion or expected value changes.

**(f) Plan identity and accounting: COMPLIANT.**
- I compared by AST. Base `verify_plan`'s line at `5ca50ef5` == the test's `V1_VERIFY_LINE` == head's v1 output
  (305 chars, byte-equal). v2 differs, and field / role name the builds.
- k = 1 identity: `runner()` is untouched this round.
- Trial accounting: no ledger code in the diff; dedupe by `trial_id` is unchanged.
- Expected values: one moved by ruling. `test_lock_exes_pins_and_verify_compares` lost "rule cell: recorded only"
  (`None`) for W1-I2 (a)'s refusal. `owned_flags`' assertion moved to `raw_rule_flags` with the same expectation.
  Neither was edited to fit.
- Seal: only synthetic T2-SYN literals (above).

## Findings
`path:line | severity | problem | required fix`

**Required**

- **R2b** `scripts/wave_stage_kinds.py:75-86` (with `scripts/wave_kinds.py:431-440`, `scripts/wave_steps.py:251-252`,
  `scripts/research_spec.py:188-190`) | minor (fail-open validation; R2 not closed) |
  - **Problem.** A rule wave can still give its cell a rule flag that the exes' tables own and that the manifest
    never names. It then runs with no `params_schema` check and no record in `rules`, which is what F-8 /
    D1-PSCHEMA forbid. There are two routes, both proven by the probes above (preflight 0, spec 0).
  - Route (i): `rule_cell.constants.set` / the template's `change.set` replace a section's flag list (`nav.flags`,
    `fit.flags`, `ic.w_flags`) or a whole section. `raw_rule_flags` never sees them.
  - Route (ii): in `fit`, spellings the fitter's argparse reads (`--theme-erc=V`, the abbreviation `--theme-er V`)
    do not equal the owned key. R6B-C-4 covers only `--composition` on v8 specs, and stops after register.
  - **Fix**, in the rule branch of `preflight_kind`, one of:
    - (a) Refuse `constants.set` / `change.set` / `change.unset` keys that are a `FLAG_SECTIONS` section,
      `<section>.flags` or `ic.w_flags`. And match given flag keys the way `cycle.spec.argparse_values` reads them:
      key head before `=`, and for `fit` any key that is a prefix of an owned flag.
    - (b) Better: resolve the would-be cell (`wave_steps.rule_cell_doc` + `research_spec.resolve`, as the spec stage
      does). Then refuse any owned option (`table_flags` + the derived flags, read with `argparse_values` semantics
      for `fit`) whose occurrences in the cell's flags differ from the parent's, unless the derived flags set it to
      that value. This one check covers set, unset, flags and spellings.
  - **Tests.** Add the four probes as refusal tests: `set nav.flags --vol-target`, `set fit.flags --theme-erc`,
    `fit --theme-erc=V`, `fit --theme-er V`. Also assert that `--composition=V` is refused at preflight, not at
    spec.

**Suggested**

- **N1** `scripts/wave_kinds.py:411-419` | minor | `table_flags` silently skips an IC row whose id `COMPOSITION_FLAGS`
  lacks. A rule added to the IC table without its `COMPOSITION_FLAGS` row (MR-4 for AL-COMB's `theme-cov-hedge-v1`
  / `mom-volman-v1`, or any later one) therefore has its raw fitter flag accepted, although naming it is refused. |
  In the rule preflight, refuse when the IC table lists an id `COMPOSITION_FLAGS` does not map ("cannot verify raw
  flags: add its row to wave_kinds.COMPOSITION_FLAGS"). Today's table lists exactly the 8 mapped ids, so nothing
  changes now, and a missed MR-4 becomes a loud stop.
- **N2** `scripts/wave_stage_kinds.py:69-86` | minor | Only kind `rule` is guarded. `horizon.flags` (hand-written by
  design) and the `rule_cell.constants` of leverage and role waves can set a NAV rule's flag (e.g. `--vol-target`)
  unchecked, because horizon and role waves read no rule table at all. This was outside R2's scope, but it is the
  same F-8 exposure. | Read the NAV table (and the IC table for `fit`) for every template kind and apply R2b's check.
  The horizon fixture's `--cadence` is not owned, so it stays valid.
- **N3** `scripts/wave_stage_preflight.py:42-55` (with `scripts/research_add_alpha.py:186-187`) | minor | The wave's
  W1-I1 check uses the parent spec's IC cache dir. add-alpha drops `ic.cache` for a parent not on the shared stores,
  so that cell's cache is the derived `<out base>/candidate-cache/<role16>-research-window-v2` (64 chars). That is
  23-27 chars deeper than the explicit dirs of v61, v61-ops, v70, v71, v70-lo3 and v7u-lo3. Preflight then admits
  ids the cell's research_cycle refuses after register (still before compute). All 18 v8 specs are on the shared
  stores, so P9's lineage is not affected. | Build the check's `Cycle` on the spec without `ic.cache` when
  `not research_add_alpha.shared_stores(spec)` (import the rule, no copy), or check both dirs and keep the longer.
- **N4** `scripts/tests/test_wave_driver.py:936-939` | minor (test robustness) | The test needs pytest's tmp path to
  be deep. With a `--basetemp` of 34 characters or fewer, `assert deep > RC.CACHE_PATH_MAX` fails (computed: a
  31-char basetemp gives 247). Root's gates use the session scratchpad (about 95 chars), so the test is green today.
  | Make the depth deterministic: pad the root, e.g. `tmp_path / ("d" * max(1, 80 - len(str(tmp_path)))) / "c"`, or
  monkeypatch `CP.CACHE_PATH_MAX` relative to the computed prefix. Add a positive case: an id that fits is not
  refused.
- **N5** `scripts/run_bounded_research.py:237-240` | minor | `ClaimsDirError` is decided on the last exception
  only, while the docstring says "a PermissionError that lasts the whole wait". Suppose a lock is held
  (FileExistsError) for the whole wait and released in the final iteration (Windows delete-pending ->
  PermissionError, then gone). That is reported as an unwritable dir (runner-error, not retried) instead of
  `LockTimeout`. The race is narrow. | Keep a flag across the loop: denied with no lock on every failed try, and
  raise `ClaimsDirError` only then.

Not re-raised (ruled): S8 and the S6 role-seal part (E2-7); the stale v1 verify plan line (E2-6); the double-reclaim
race (E2-4).

## What root must check at merge
1. **Composition fixture after D2.** D2's merge request 1 (flip `test_wave_kinds.py:247-251, 311-312`, line numbers
   at `5ca50ef5`) is superseded by `defa34c2`. Those asserts are now `:293-298` and `:366-367`, and they read the
   frozen `d1_table()`, so D2's re-pin turns nothing red. Do not flip them. Instead, after D2, run the report's
   post-merge command on the live table (expect `({'fit': {'--composition': 'ew-theme-std-v1'}}, [])`). Preferably
   add it as a pytest on `ic_table(False)`, plus `new-combination-rule.json` to the spec stage on the live table.
2. **NAV-rule and horizon fixtures (C1 is in pool-2).** With the built NAV exe, expect `new-nav-rule.json
   ({'nav': {'--aim-leverage': '1.5'}}, [])`. Plan `new-horizon.json` on tiny-world. Problems not empty = stop; do
   not edit a fixture to fit.
3. **The literal `atx.research-wave/v2`** goes into `classes.json` (SQL2-CLS).
4. **Base-vs-head hash-only plan identity, with the real IC exe on disk, in pool-2.**
   - W1-I1 now runs at plan whenever the exe and the library are on disk. `v70-lo3` is 249 at pool-2: a 1-char
     margin. Run it from pool-2 (or `C:/atx`), never from a longer root.
   - Compare only the 130 original keys.
   - The v1 verify plan line is byte-equal to base (checked here by AST, and pinned by `V1_VERIFY_LINE`).
5. **PM7-35's Python copy** (`wave_rules.py:50-51`) retires only after root's identity against B1's C++ predicate.
6. **Do not merge while a wave runs.**
   - Not under a `receipt_digest: "content"` wave (nested time keys, E1-STALE).
   - Not under a v1 template-cell wave between nav and verify whose parent NAV came from another exe: verify now
     refuses it (W1-I2 (a)).
7. **W1-I2 sequencing.** (a) is keyed on the exe hash. The order is: the last relink of
   `atx-equity-strategy-targets.exe`, then the P9-B0 re-base, then the (b) re-pin, then the first wave, with no
   NAV-exe rebuild in between. If (b) adds `exes_sha256` to the parent specs, it must name the exe the cells run;
   a differing pin refuses every template cell.
8. **pytest.** Keep `scripts/tests` and `atx-engine/tools` tests as separate invocations, with a `--basetemp` longer
   than 34 chars (N4).
9. **`test_cycle_e2e` with `ATX_EQUITY_BIN`.** The Release canary under `%TEMP%` now passes through W1-I1 (the exe is
   on disk). The wave-1 review measured about 242 chars, so the margin is about 8 chars (about 2 with audit-exact).
   A refusal there is exit 2 at plan, not a canary defect.
10. **MR-4 and B2-8.**
    - Land MR-4 in the same merge as AL-COMB's C++ table (N1).
    - Place B2's call-site pin hunks onto `scripts/cycle/` (Ruling B2-8; E2 merges first).
11. After R2b, re-review the fix SHA.

## Checked
- [x] Python lane: `.agents/cpp/agent.md` section 10 does not apply (no C++). Determinism, error paths and contracts
  were applied, and the C++ the lane cites was read for W1-I1.
- [x] The diff stays inside E2's files. The one test-side edit (`test_research_spec.py`, `stand_in_ic`) is listed.
- [x] The report's claims match the code, the logs and the re-runs. The W1-I1 margin note and the
  `226 + len(id)` derivation were re-derived.
