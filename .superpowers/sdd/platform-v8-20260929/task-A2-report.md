# Task A2 report: cycle plumbing follow-ups (lane A2, pool-8)

Branch `feat/platform-v8-a2-20260930` from the root integration branch `feat/platform-v8-20260929` (41ac94fd).
Python and JSON only; nothing built, no real data run. One file of pool-2 was read, as allowed: the DSL-metadata plan
`build-equity/v8-i3-plan-v71.json` (task 2).

| task | commit |
|---|---|
| 1 marginal step argv | `a366bab0` |
| 2 Ruling E-19 plan rows | `d55ad8e1` |
| 3 v8 specs and templates | `7ddcf913` |

## 1. Marginal step argv (`a366bab0`)

The verb's CLI, read from `atx-impl/src/strategy_marginal_ic.cpp`: `dispatch_marginal_ic` takes 11 options, each with
one value (`--candidate-cache --library --library-sha256 --pool --pool-sha256 --role --themes --fields --output
--min-names --max-memory-mib`); `run_marginal_ic` refuses a run without `--candidate-cache --library --pool --role
--output`; `--themes` takes the weights JSON the pool names by `composition_weights_sha256` (and whose
`library_sha256` is the pool's); `--role` must be the pool's role.

`scripts/research_cycle.py`:
- `marginal_step(lib, fd)` builds `exe marginal --candidate-cache <cycle cache> --library L --library-sha256 <pin>
  --pool P --pool-sha256 <pin> --role <inputs.role path> [--themes <inputs.<marginal.themes> path>] --fields <this
  cycle's fields dir> [--min-names <u pass's> | marginal.flags] --output O`; runner binds exe, library, pool, role,
  weights, fields manifest.
- Spec keys: `marginal {output, pool (input key, default reference_combined), themes (input key, optional), flags}`.
  `validate_marginal` (spec load, so plan time): themes names a pinned input; flags are value pairs of
  `MARGINAL_SPEC_FLAGS` (`--min-names` >= 3, `--max-memory-mib` 32..16384, as the verb); the step's own options
  (`MARGINAL_BUILT`) are refused there; a bare `--themes` is refused.
- `check_marginal_bindings` (when the step is planned, exit 3 before any phase runs): the pool manifest's
  `role_manifest_sha256` is `inputs.role`'s pin; with themes, its `composition_weights_sha256` is the themes input's
  pin. Skipped for a hash-only pool.
- New input key `reference_weights`.

`scripts/research_add_alpha.py`: the derived spec gets `inputs.reference_weights` = the parent's fit output
`composition_weights.json` (the file the parent's combined signal was blended with) and `marginal.themes:
"reference_weights"` (no flags).

Tests (`scripts/tests/test_research_cycle.py`): `test_marginal_argv_is_the_verbs_full_cli` reads the option and
required sets from the C++ (the `key == "--x") cfg.member` lines and the members `run_marginal_ic` refuses empty),
pins them (`VERB_OPTIONS`, `VERB_REQUIRED`), checks `MARGINAL_REQUIRED` / `MARGINAL_BUILT` / `MARGINAL_SPEC_FLAGS`
against them, and parses the planned argv with a mirror of the C++ parser (every option known, once, with a value;
the required ones present); `test_marginal_spec_refused_before_any_run` (7 load refusals; weights not the pool's and
a pool of another role refused by `plan` exit 3 and by `run` before any call); `test_screen_stops_before_w` now
asserts the full argv and that the verb ran (the fake runner refuses a marginal call missing a required option);
`test_add_alpha_entry_byte_identical_to_committed` plans the derived spec's marginal step (themes, role, pool pin,
min-names).

## 2. Ruling E-19 plan rows (`d55ad8e1`)

`test_plan_rows_equal_static_validation` now compares, per member, DSL SHA, lookback and extra fields with the v7.1
recipe lineage, the maxima (slots 8, nodes 37, prior bars 272) with the recipe's static validation, requires
`validate_plan` to accept, and checks slots and nodes per member only as within the house budget (slots <=
`max_slots` 7 or the recorded exception, qmj_safety 8; nodes: the registry has no node budget, so 0 < nodes <= the
recorded maximum 37). No recipe or library byte changed (`test_v71_library_byte_identical` passes).

**Result on the real plan** (`ATX_V71_PLAN_JSON=C:/atx-wt/pool-2/build-equity/v8-i3-plan-v71.json`): **PASS**, the
whole generator file 9 passed (also `--check --plan-json` on it). The five figure differences integration 3 found
remain and are now inside the budget: q5_eg nodes 29 vs 28, ftd_fail 5 vs 4, ea_overdue 9 vs 8, sv_flow 7 vs 6,
res_mom_ind slots 4 vs 5 (exe vs recipe estimate).

## 3. v8 specs and templates (`7ddcf913`)

### Mechanism

- `scripts/research_spec.py` (new, 280 lines): a template (`"schema": "atx.research-cycle-template/v1"`) is a cell
  written as its parent's spec plus the registered change: `parent` (null; root sets the last accepted cell's spec
  path), `nominal_parent` (planned on while parent is null), `requires` (open preconditions), `change {unset, set
  (dotted keys), inputs, flags {section: {"--opt": value | true | false | null}}}`, `locked`. Resolution: parent
  (recursively; a cycle refused) without its identity checks against its own parent (`compare`, `ref`,
  `static_check`, `baseline_*`, `reference_daily`, `reference_orientations`, `reference_daily_ic`); unset, set; derived
  inputs from the parent's outputs (`reference_cell` = its NAV cell, the paired reference of plan section 9;
  `reference_admission`; with a marginal section `reference_combined` = its w attempt 1 combined signal and
  `reference_weights` = its fit weights); change.inputs; flags. `nav.output` must differ from the parent's. Every
  output a template does not rename is the parent's and resumes as done, so a template renames exactly the outputs
  downstream of its change (NAV change: nav; composition: fit, card, w, nav, monitor; library: all).
- `run` refuses (exit 3) a null parent, an open `requires` anywhere up the chain, or a `<fill:...>` value (a null
  flag value). `plan` / `status` print `# template ...`, `# requires ...`, `# fill ...`. `lock --write` on a template
  writes the pins of what it adds (change.inputs, change.set fields) and derives (`locked`, with the path); the
  parent's pins stay in the parent's file.
- `research_cycle.py`: `load_spec` resolves templates; `fields.manifest_sha256: null` = as-built dir not yet locked
  (plan computes it and marks it UNLOCKED; `lock --write` fills it; validation still refuses a non-digest string).

### Specs (`scripts/specs/v8/`)

- `base-lo1.json` (B0a), `base-lo3.json` (B0b): runbook section 4 drafts with: runner 180 s / 1,536 MiB and
  `runner.phases` u, w 300 s / 2,560 MiB (IC caps, OD-2); `fit.work_dir` and card `--work-dir` =
  `build-equity/fit-work` (C-1 store base, shared by fitter, card and monitor); `summ.script` =
  `atx-impl/tools/nav_summ.py` (V-1 moved it) with `--protocol v8`; `dsr_n: "ledger+1"`; `"verdict": true`; B0b's
  atx-db stage pins (v2-pit bridge, fundamentals SIC) null so `lock` re-hashes them.
- `base-b0c.json` (template, nominal parent base-lo1): Ruling E-10 as a NAV-only change of the winner (its u, fit,
  card, w reused as done): `--warm-start-sessions 60`, plus `--capacity-curve` (report only; see deviations);
  `requires` the NAV label role (below).
- `r1-comp-v8` (fit `--composition ew-theme-std-v1`), `r2-lib-v80` (library/recipe v80, gate p1-v80 on the 7 READY
  members, marginal on the parent's pool and weights), `r3-aim-gain` (fit `--composition ew-theme-aim-v1`,
  requires), `r4-hold-band` (nav `--hold-band .1`), `r5-adv-hold` (nav `--adv-hold-q .1 --capacity-curve`),
  `r6-spo-v3` (rule spo-v3, `--spo-alpha implied-aim --risk-model <fill> --risk-model-sha256 <fill> --spo-books
  primary`, no capacity curve; requires; E-14 in the description), `r7-lib-v81` (library/recipe v81, gate p1-v81 on 5
  members, marginal; requires fields v11). All nominal parent base-b0c.json.

### Null pins per spec and who fills them (`lock --write` after the named step)

| spec | null pin | filled after |
|---|---|---|
| base-lo1 | inputs.role | R7 (train-2020-2023-lo1) |
| | inputs.identity_bridge | R4 (identity-bridge-r4-v2) |
| | inputs.fund_events | R5 (fundamental-events-v3) |
| | fields.manifest_sha256 | R10 (train-2020-2023-lo1-fields-v9) |
| base-lo3 | inputs.role | R8 (train-2020-2023-lo3) |
| | inputs.identity_bridge | atx-db export identity-bridge-v2-pit (runbook V2PIT 09aac28f..., re-hashed at lock) |
| | inputs.fund_events | R5 |
| | inputs.sic_events | atx-db fundamentals stage (runbook SIC 9f9b2f85..., re-hashed at lock) |
| | fields.manifest_sha256 | R11 (train-2020-2023-lo3-fields-v9) |
| | inputs.reference_cell | cell B0a run (W0-4 step 3) |
| base-b0c | parent | root: base-lo1.json or base-lo3.json (the winner) |
| | locked reference_cell, reference_admission | the winner's cell (lock the template) |
| | requires: NAV label role | a C++ NAV option + R15 (dlret role) |
| r1, r3, r4, r5, r6 | parent | root: last accepted cell's spec |
| | locked reference_cell, reference_admission | the parent cell |
| r2 | change.inputs library, recipe | add-alpha / generate_library v80 (draft E1-E5) |
| | locked reference_combined, reference_weights | the parent's w pass and fit |
| r6 | nav `--risk-model`, `--risk-model-sha256` (fill) | R-6 step 3: risk verb, atx-risk-v1.1 on the 4-year role |
| r7 | change.inputs library, recipe; fields (requires) | draft E6-E10 (v81); fields v11 |

Every template also inherits its parent's null pins until the parent is locked (the plan lists them as UNLOCKED).
`test_every_v8_spec_loads_and_plans` pins this set per spec.

### Tests (`scripts/tests/test_research_spec.py`, 24)

Every v8 spec loads and plans in a fake root (a stand-in file per input; the atx-db paths are re-pointed under the
root, nothing outside it is read) with its null pins reported as UNLOCKED, the fields step "as built ... UNLOCKED",
the fills listed, a template's `run` refused before any call, a base spec with no refusal; each template differs from
its nominal parent exactly by its registered change and the downstream renames (flat diff) and derives its references
from the parent; the base specs carry the ruled settings; a template on the fake tools runs only nav + summ, paired
with the parent (`--reference out/N`); lock / relock / pin mismatch; refusals; flag operations; null fields pin.

## How root verifies

```bash
PY="C:/Program Files/Python312/python.exe"
"$PY" -m pytest -q -p no:cacheprovider scripts/tests/test_research_cycle.py scripts/tests/test_research_spec.py scripts/tests/test_research_ledger.py scripts/tests/test_cycle_e2e.py atx-impl/strategies/test_generate_library.py
ATX_V71_PLAN_JSON=build-equity/v8-i3-plan-v71.json "$PY" -m pytest -q -p no:cacheprovider atx-impl/strategies/test_generate_library.py
```

Here: 129 passed, 4 skipped (3 RESEARCH_CYCLE_LIVE_ROOT, 1 tiny_world live test needing `ATX_EQUITY_BIN`); with the
real plan 9 passed. Root also runs `test_cycle_e2e.py` with `ATX_EQUITY_BIN` (absolute) after the merge. Identity: the
v6.1 / v7.0 / v7.1 plan fixtures pass unchanged (no spec before v8 has a marginal section, a template or a null
fields pin). Real use: after the runbook, `research_cycle.py lock scripts/specs/v8/base-lo1.json --write`, then
`plan` (the header lists every pin), then `run`; for a cell, set `"parent"` in the template, `lock --write` it, `plan`.

## Deviations (with reasons)

1. **Themes from a spec input, not from this cycle's fit.** The verb binds `--themes` to the pool's
   `composition_weights_sha256` and the weights' `library_sha256` to the pool's; the pool is the parent's combined
   signal, so this cycle's own fit output can never bind. The themes file is the parent's fit weights, pinned as
   `inputs.reference_weights`.
2. **`--fields` and `--min-names` are always passed** (`--fields` picks among cache entries of other field payloads;
   `--min-names` defaults to the u pass's so the marginal read uses the IC runner's name floor; the verb's default is
   50).
3. **Templates are delta files resolved at load** (not full copies of B0c): "differ from their parent only by the
   registered change" then holds for whichever parent root sets. B0c is a template too (its parent is the winner).
4. **`--capacity-curve` on B0c** (report only; primary series and scored files unchanged, D-0) so every R cell reports
   4x NAV against its parent (plan section 9; R-3 and R-5 compare 2x / 4x with the parent). Root drops it if E-10 is
   read strictly; R-5 still adds it.
5. **Null as-built fields pin** support (`fields.manifest_sha256: null`) was needed for "every pin left null for
   lock": validation refused null there before.
6. Base specs: IC caps via `runner.phases` (u, w) instead of the runbook's 300 s / 2,560 MiB for every phase;
   `--protocol v8` and `verdict: true` added; runbook output names kept.
7. R-2's 8 `_f49` re-screens are not in the gate: they need a fields dir with `grp_ff12f49` (R2-e); root adds the
   fields and the ids to the template when that dir exists.

## Cross-lane edits (H1 edits research_cycle.py at the same time)

`scripts/research_cycle.py`: INPUT_KEYS (+`reference_weights`), marginal constants, `option_value`,
`validate_marginal`, `validate_v8_keys` (marginal call; `as_built`), `load_spec` + `as_built`, `validate_spec` (two
fields lines), `Cycle.__init__` (one line), `steps()` (the `as_built` line and the marginal call), `marginal_step`,
`check_marginal_bindings`, `fields_step` (as-built branch), `fields_check` (one line), `header` (return line),
`run_cycle` (3 lines at the top), `lock` (template branch, fields pin), module doc. `scripts/research_add_alpha.py`:
`parent_outputs` (`as_built`), derived inputs (`reference_weights`) and marginal block.

## Open risks

1. **B0c is not runnable (blocker, listed as requires).** Ruling E-10 wants the delisting-returns role for NAV labels
   only, but the NAV verb binds `--combined`, `--fields` and `--role` to one role SHA (`admit_saved` / `load_prices`
   in `strategy_target_replay.cpp`; the fields manifest's role pin), and research_cycle has one `inputs.role` for all
   phases. Needs a NAV option for a separate label role (C++), then its input and flag in `base-b0c.json`. Every R
   template inherits the block through its chain until B0c runs.
2. **R-1 memory on the 4-year role:** ew-theme-std-v1 adds 8 B per cell per theme (R-1 report) = about 654 MiB at
   1,405 x 6,100 x 10 themes, so the w pass admission is about 2,606 MiB > the 2,560 MiB cap (it refuses before any
   payload). Root raises the w cap and `--max-memory-mib` for R-1 or rules otherwise.
3. **add-alpha with a v8 parent spec refuses** (and, being atomic, writes nothing): its name template needs the
   parent library token (`v71`) in every parent output name; the B0 / template names lack it. Use the r2 / r7
   templates for the cells, and generate the library with add-alpha `--parent-spec scripts/specs/v71.json` (its
   derived `lib-v80.json` is a 3-year-role byproduct root does not run) or by the draft's registry edits +
   `generate_library.py --plan-json`. add-alpha also drops `fit.work_dir`, so its specs derive
   `fit-work/<role16>-<window>` and the fitter nests `<role16>-<window>` again inside it (a separate, unshared store).
4. **`cache gc` with the v8 specs:** they reference the store base `build-equity/fit-work`; gc lists its children
   `fit-work/<role16>-<window>` as unreferenced, so `--apply` deletes the shared fit/card store (a cache: recompute
   cost only). A one-condition gc fix (keep a derived child whose base a spec names) belongs to lane A.
5. **R-3:** "on top of the R-1 weights" has no code (ew-theme-aim-v1 scales ew-theme-v1 weights); listed as requires.
   **R-6:** part 2 unwritten; `--hold-band` / `--adv-hold-q` are aim-partial-v5 only, so an accepted R-4 or R-5
   parent conflicts with spo-v3 (ruling needed; in requires).
6. Derived `reference_combined` assumes the parent's w attempt 1; if the parent needed attempt 2, set
   `change.inputs.reference_combined` in the template.
7. Templates reuse the parent's outputs by name: a new template that forgets a downstream rename silently reuses the
   parent's pass. The test pins the renames of these 8; new templates must follow the rule.
8. Card on the 4-year role is estimated at about 1.3 GB (runbook) under the 1,536 MiB base cap; add `runner.phases.card`
   if it binds. The marginal verb's own admission formula gives about 354 MiB for 55 candidates on 1,405 x 6,100
   (under its 600 MiB default and the 1,536 MiB runner cap).

Status of these risks after the follow-up below: 1 resolved by Ruling E-25 (pending lane R45's key), 2 by E-28, 3 by
follow-up task 1, 4 by task 2, 5 by E-27 (task 3) and E-26, 8 by task 4. 6 and 7 stand.

# Follow-up (PM, Rulings E-25..E-29): all four tasks done

Step 0: `git merge --no-ff feat/platform-v8-20260929` = `814deafd` (clean).

| task | commits |
|---|---|
| 1 add-alpha on a v8 parent; E3/E4 options | `83a52d36`, `ef87b78c` |
| 2 cache gc keeps the shared store | `8f8764da` |
| 3 R-3 on the R-1 rule (E-27) | `0944bef7` |
| 4 templates for E-25, E-26, E-28, E-29; card caps | `4ef84de4` |

## Task 1: add-alpha with a v8 parent spec

- `--parent-spec` may be a v8 base spec or a template; refused (exit 2, nothing written) while the template's parent
  is null, its chain lists a `requires`, or a value is unfilled (`research_spec.run_refusal`).
- Name rule `derive_name(text, parent, name)`: every token PARENT (generate_library.rename: not inside a longer name or
  number) becomes NAME; a name without the token gets `-NAME` appended. B0a's `mega-v8-b0a-train-u` ->
  `mega-v8-b0a-train-u-v80`; `mega-nav-v8-b0a-lo1-v71-ew-...` -> `...-lo1-v80-ew-...`; the v7 specs are unchanged.
- Stores: a parent with `fit.work_dir == <out_root or build-equity>/fit-work` (C-1) passes `ic.cache` and
  `fit.work_dir` on; a v7 parent (per-version stores) still gets them derived from the role (A-2 behaviour).
- Kept inputs: every parent input except library, recipe, baseline_*, reference_* and the fields-builder inputs (so
  `role` and B0c's `label_role` carry over).
- Library edits, no hand edits: `--removes ID` (X appended, IDs removed with their exceptions: draft E3),
  `--replaces ID` (X in the first ID's roster position, others removed, X inherits their exceptions: draft E4, the
  q5_eg exception moves to q5_eg_f49), `--rescreen` (with one `--replaces`: `libraries/NAME.json` gains
  `"rescreens"`, the gate lists it under `report`, not `admitted`; recipe `trials.admission_trials` excludes it and
  records `rescreens` and `removed_parent_members`), `--exception LIMIT=N --exception-basis TEXT` (Ruling R2-b),
  `--fields DIR` (an as-built fields dir the child pins; the parent's stays `baseline_fields`, so `ref` runs and must
  reproduce the parent's S2 daily; sticky: later adds into the same wave keep it). Optional keys are written only when
  non-empty: v7.1 regeneration byte-identical (`test_v71_library_byte_identical` passes).
- Identity compares after u gain `keys_in: "{input:library}"` when parent members left (research_cycle compare: the
  parent's member rows the child still holds).
- Tests (`test_research_spec.py`): `test_add_alpha_on_a_v8_base_spec_screens_and_runs` (base-lo1 parent, fake tools:
  names, stores, kept inputs, lock, `run --screen` then `run`), `..._template_removes_replaces_rescreens_and_records_
  exceptions` (r4 template parent: null-parent refusal, 7 refusals writing nothing, E3 + two E4 re-screens + sticky
  fields, library/recipe/stub/spec contents, screen and run with ref, the compare fails without `keys_in`),
  `test_derive_name_substitutes_the_parent_token_else_appends`.

## Task 2: cache gc

`research_gc.users`: a candidate `<base>/<child>` is kept when a spec names `<base>` (every window: the monitor reads
all windows of its role). `test_cache_gc_apply_with_the_v8_specs_keeps_the_shared_stores`: `--apply` with the ten v8
specs keeps `fit-work/*` and the lo1 / lo3 caches, deletes the stale ones.

## Task 3: R-3 on the R-1 rule (fitter only; no C++ change)

`composition_rules.ew_theme_std(..., gains=)`: w_k = (1/T) score_k g_k / sum_theme score g, then the member cap
1/(2T). `--composition ew-theme-std-aim-v1` writes an ew-theme-std-v1 weights file (schema v2, the same
`theme_standardise` block with rule "ew-theme-std-v1", which the IC runner already applies) whose weights carry the
gains; provenance.rule `ew-theme-std-aim-v1`, provenance.std `aim_gains`, provenance.aim the aim report block. Gains
of 1 give the ew-theme-std-v1 weights bit for bit. Fitter edits outside the composition functions (H1 overlap): the
composition tuples, `AIM_RULES`, one line in `fit()` (`aim=args.composition in AIM_RULES`), the aim predicate and
dispatch in `fit_prior()`. Tests: `test_aim_gain_composes_with_theme_std` (theme shares 1/T before the cap, score x
gain inside, cap after, gains of 1 bit for bit, refusals), `test_std_aim_fit_is_the_std_rule_on_the_aim_gains`
(fitter end to end); ew-theme-aim-v1 bytes unchanged (`V1BytesUnchangedByV6` ran, not skipped; fitter suite 95
passed). Template flag values may map the parent's value; `r3-aim-gain.json` maps `--composition` ew-theme-v1 ->
ew-theme-aim-v1, ew-theme-std-v1 -> ew-theme-std-aim-v1, and its `requires` is gone.

## Task 4: templates

base-b0c: `change.inputs.label_role` (lo1-dlret; null pin; re-point to lo3-dlret when B0b wins), requires dropped,
`--capacity-curve` kept (E-29). r6: requires dropped (E-26). r1: `runner.phases.w.max_rss_mib 3072` and new spec key
`ic.w_flags {"--max-memory-mib": "3072"}` (applied to ic.flags for the w pass only; validated), inherited down the
chain (tested on R-1 and R-3-on-R-1: u 2,560, w 3,072). base-lo1 / base-lo3: `runner.phases.card` 300 s / 2,560 MiB.
r2 / r7: a `requires` that names the add-alpha spec that runs the cell (never run the template and lib-v8x both).

# STOPPED HERE (owner stop, 2026-09-30)

Done: follow-up tasks 1-4 and this report; tree clean. Not done: nothing of the four tasks. Open, for other lanes:

1. **label_role key (lane R45).** base-b0c and every template on it carry `inputs.label_role`; until R45 adds it to
   `INPUT_KEYS` and the NAV step (`--label-role MANIFEST --label-role-sha256 SHA`), `plan`/`run` of base-b0c and r1..r7
   stop with "spec inputs: unknown key(s) label_role". `test_research_spec.py` registers the key in an autouse fixture
   only when absent (drop it after R45 merges). Nothing else here depends on R45.
2. Remaining `requires`: r2 (runs as `lib-v80.json`), r7 (runs as `lib-v81.json`; fields v11). Remaining fills: r6
   `--risk-model`, `--risk-model-sha256` (R-6 step 3). Every template: `parent` null until root sets it.
3. Design decisions another lane must keep: E3 appends (`--removes`), E4 replaces in place (`--replaces --rescreen`);
   re-screens are 0 admission trials and gate report rows; `--fields` is sticky within a wave; the std-aim weights
   file keeps the runner block `ew-theme-std-v1`; gc keeps every child of a named store base.

## Root command sequence (`PY="C:/Program Files/Python312/python.exe"`, `RC="$PY scripts/research_cycle.py"`)

Each spec: commit it before `run` (clean check); `plan` prints every pin and phase.

- **B0a**, after runbook R4, R5, R7, R10: `$RC lock scripts/specs/v8/base-lo1.json --write`; `$RC plan
  scripts/specs/v8/base-lo1.json`; `$RC run scripts/specs/v8/base-lo1.json`.
- **B0b**, after R5, R8, R11 and B0a's cell: `$RC lock scripts/specs/v8/base-lo3.json --write`; `plan`; `run`.
- **B0c**, after R15 (the winner's dlret role) and R45's label_role merge: set `"parent": "base-lo1.json"` (or
  `"base-lo3.json"` and re-point `change.inputs.label_role` to `build-equity/train-2020-2023-lo3-dlret`) in
  `scripts/specs/v8/base-b0c.json`; `$RC lock scripts/specs/v8/base-b0c.json --write`; `plan`; `run`.
- **R-1, R-3..R-6** (templates): set `"parent"` to the last accepted cell's spec (e.g. `"base-b0c.json"`,
  `"r1-comp-v8.json"`); r6 also fills the two `<fill:...>` values; `$RC lock <template> --write`; `plan`; `run`.
- **R-2** (P = the last accepted cell's spec, e.g. `scripts/specs/v8/base-b0c.json` with its parent set; its cell run):
  1. E1: add registry field rows `ea_days_since`, `inst_own_share` to `atx-impl/strategies/alphas/registry.json`
     (hand edit; clock and basis from the fields-v9 manifest; add-alpha adds alphas, not field rows).
  2. The 7 READY commands of library-v8-draft.md section 7, in order, each with `--parent-spec P` appended (optionally
     `--form`, `--prior-sign-source`, `--formula`, `--domain`, `--deviation` per E2); `earn_surprise_comp` also takes
     `--removes sue --removes droe --removes chtax --exception max_extra_fields=8 --exception-basis "Ruling R2-b"`.
     K1: the parent's IC exe runs `--plan-only` on the parent's role and fields (or pass `--plan-json`).
  3. Fields: build fields v9 + `grp_ff12f49` with `--reuse` from v9 on the parent's role (fields lane / runbook; an
     as-built dir F whose manifest lists grp_ff12f49); E4: add the `grp_ff12f49` registry field row (hand edit).
  4. The 8 re-screens, one call each, in R2-8 table order: `$RC add-alpha --id <x>_f49 --dsl "<R2-8 string>" --theme
     <original's> --tier <original's> --prior-sign <original's> --citation "<original's>; S-12 FF49 financials"
     --origin prior --parent v71 --name v80 --parent-spec P --replaces <x> --rescreen --fields F`.
  5. add-alpha locks `scripts/specs/v8/lib-v80.json` (exit 3 if a pin is missing: `$RC lock
     scripts/specs/v8/lib-v80.json --write`); root registers `libraries/v80.prereg.md` and pins the library and
     slim-recipe SHAs in v8-prereg.md (E5); commit.
  6. `$RC run scripts/specs/v8/lib-v80.json --screen`: gate p1-v80 `admitted` = the 7 trials (require any), `report` =
     the 8 re-screens; marginal IC on P's combined signal with P's weights. Then `$RC run
     scripts/specs/v8/lib-v80.json`: ref first (fields F differ from P's: must reproduce P's S2 daily), w, nav,
     monitor, summ (dsr_n ledger+1: one cell). Recipe: admission_trials 7, rescreens 8, removed_parent_members 11.
- **R-7**: as R-2 with `--parent <v80 if R-2 accepted, else v71> --name v81`, fields v11 as `--fields`, draft E6-E10.

Tests at stop: cycle + spec + ledger + e2e + generator + composition rules 147 passed, 4 skipped (live roots / exe
dirs); fitter 95 passed.
