# Lane D1 review

## Verdict
APPROVE

0 blocker, 0 major, 7 minor.

The rule table, the ordered stage list, `--list-rules --json` (K-P9-6, P7 envelope), the registry theme table, the
fitter's plugin list and the P8 rank-helper unification all meet brief D1. Flag-absent identity holds by construction
for every output I traced: the recipe, the combined manifest, the sleeve manifest, the admission bytes, the plan and
the composition blend bits. The built-in 13-theme default (deviation 3) keeps identity. B1 needs no source adaptation
(see "B1 adaptation"). Every finding is minor and none blocks the merge.

## Reviewed SHA
`757ba5824c3da8a3b1fec4637cd775c66b8f740c` (base `d7c1c520`). The tree was `C:/atx-wt/pool-20` on branch
`feat/p9-d1-20261003`, and HEAD was this SHA. `git status --porcelain` was empty before and after my runs. I also
read B1 at `01f20405c61bfdcfb3d3ec2141c39821fff860e6` in `C:/atx-wt/pool-14`, whose tree was clean. Diff stat: 22
files, +2994 / -618.

## Evidence
I ran the lane's pytests in pool-20 with `PYTHONDONTWRITEBYTECODE=1` and `-p no:cacheprovider`. They wrote no file.

```
cd C:/atx-wt/pool-20/atx-impl/tools
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider test_fit_composition_rule_plugins.py test_composition_resid.py
...............................                         [100%]
31 passed, 17 subtests passed in 5.94s
exit_code=0

"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider test_fit_composition_weights.py \
  test_fit_composition_weights_pool.py test_fit_composition_weights_store.py test_composition_ic_shrink.py \
  test_composition_rules.py test_composition_theme_erc.py test_composition_theme_tsmom.py test_composition_two_speed.py
200 passed in 58.19s
exit_code=0
```
The two runs total 31 + 200 = 231, which matches the report's Evidence 2.

I also ran two independent read-only checks. The scripts are in my scratchpad, outside every tree.
- **`chk_fixture.py`.** It extracts every `R"(...)"` params_schema literal and both recipe statements from
  `strategy_ic_rules.cpp`. For all 8 rows, each literal is JSON-equal to the committed
  `tests/fixtures/composition_rules_list.json` (`schema_ok True`). Each `recipe_sha256` equals the SHA-256 of
  `json.dumps({"composition","key","value"}, separators=(",",":"), sort_keys=True)`, the nlohmann compact sorted
  dump (`sha_ok True`). `redistribute_text` and `standardise_text` are byte-equal to the base
  `strategy_ic_admission.cpp` statements (`True True`). exit 0.
- **`moved.py`.** Of the 386 lines removed from `strategy_ic_admission.cpp`, every non-comment line that is not
  verbatim in `strategy_ic_rules.cpp` or `strategy_ic_admission.cpp` is one of three things:
  - the intentionally restructured `method_recipe`, `admit` or dispatch lines;
  - the old `StandardiseRule` table;
  - a `const Library&` to `const RuleInputs&` signature change.

  Every refusal message, verify body, `theme_indices`, `standardise_block`, `redistribution_block` and
  `composition_recorded_rule` moved verbatim. exit 0.

The registry's `themes`, in file order, are the 13 names of the base `theme_resid_order`, in the same order. They
are also `builtin_registry_themes` and the keys of `two_speed_half_lives`.

## Findings
path:line | severity | problem | required fix
- `atx-impl/src/strategy_ic_rules.cpp:400-458` (params_schema literals) | minor | **params_schema describes the
  weights-file block.** Each `params_schema` is the JSON Schema of the block the fitter writes. It requires fitted
  keys: `themes`, `ic_shrink.members`, `theme_erc.covariance`, `order` and `blocks`. It is not a schema for the
  rule parameters that a K-P9-9 wave manifest `rules: [{name, params}]` can carry, and K-P9-9 says those are
  "validated against K-P9-6". As published, E2 cannot validate manifest `params` against it, because every schema
  except two-speed-v1's refuses `{}`. (A smaller mismatch: erc `"sweeps":{"const":10000}` accepts `10000.0`, which
  verify refuses.) K-P9-6 and P7 do not define the field, so D1's reading is defensible. | Get a PM ruling on what
  `params_schema` describes before E2 is dispatched. If it is the manifest params, re-pin the 8 literals and the
  fixture (no identity impact).
- `atx-impl/src/strategy_ic_two_speed.cpp:42-44` | minor | **New refusal untested.** "weighted theme ... is not a
  theme of the theme table (two-speed-v1)" has no test. It cannot fire flag-absent (all 13 half-life themes are
  built-in). It is reachable only with a `--theme-registry` that omits a weighted theme which has a half-life. | Add
  a parse test: `parse_composition_rules` with a registry ThemeTable that lacks one weighted fast or slow theme,
  asserting the message.
- `atx-impl/tools/fit_composition_weights.py:468` (`PRIOR_THEMES = registered_prior_themes()`) and `:480`
  (`require_resid_order`) | minor | **The R6C-4 early refusal is vacuous until E2.** With `PRIOR_THEMES` read from
  the registry at import, `require_resid_order` compares the registry with itself. A theme appended to
  `registry.json` now enters theme-resid-v1's fitted order. The fitter fits, and then the flag-absent exe (built-in
  13) refuses the weights file. The refusal therefore moves from "before anything is computed" (PM5-12's purpose)
  to after the fit, and only the pytest pin `test_composition_resid.py:268` catches it, at test time. | Accept as
  part of deviation 3 until E2 passes `--theme-registry` everywhere. Either add a ledger line now ("registering a
  theme needs the C++ built-in table edit until E2"), or make `require_resid_order` also compare against
  `builtin_registry_themes`.
- `atx-impl/tools/test_fit_composition_rule_plugins.py:157` (also `test_composition_resid.py:268`) | minor |
  **These pytests pin today's data.** `PRIOR_THEMES == V4_THEMES + V7_APPENDED_THEMES_OF_RECORD` pins today's
  registry contents, not the contract. The first theme registered (the case this change exists for) fails both
  tests, so registration still needs 3 edits until E2: the registry, the list of record and the C++ built-in. This
  contradicts the comment "a theme is registered in one place" (`fit:296-299`). | Say so in the test docstring and
  the comment, or retire the list-of-record equality when E2 lands.
- `atx-impl/src/strategy_ic_runner.cpp:811-819` | minor | **`--list-rules` matches any token.** The pre-scan matches
  `--list-rules` anywhere in argv, including as an option value (e.g. `--output --list-rules`). The report lists
  this. | Test only `argv[1]` and `argv[2]` when `argc==3`, and leave the general parse to refuse it elsewhere.
- `atx-impl/src/strategy_ic_runner.cpp:676-727` (deviation 4) | minor | **An over-cap plan now prints a complete K1
  plan.** It prints `candidates[]` rows and then exits 1; at base it printed nothing. Exe-driven consumers check
  `returncode` first (`generate_library.py:494-496`, through `exe_plan`, which `research_add_alpha.py` and
  `generate_from_spec.py` also use). A saved stdout of a refused plan passed through `--plan-json`, however, now
  loads as a valid K1 plan, and the `admission` key is ignored. | Note it in the K1 contract text, or have the
  saved-plan loaders (not D1-owned) refuse a plan that carries `admission`.
- `.superpowers/sdd/platform-p9-20261003/task-D1-report.md` (Evidence, "no new line exceeds 100 columns") | minor |
  **The 100-column claim is false.** About 40 added lines in `strategy_ic_rules.cpp` exceed 100 columns (up to 117),
  most of them moved verbatim from `strategy_ic_admission.cpp`. Others: `strategy_ic_two_speed.cpp:43` (105),
  `strategy_ic_rules.hpp:132` (102) and `strategy_ic_runner.cpp:840` (101). The owning files already use lines over
  100 columns, and this is not a `/WX` hazard. | Correct the claim. No code change is needed.

## Specific checks

### Flag-absent identity (fit, u, w, IC bytes)
- **Recipe.**
  - The table rows reproduce the v8 keys exactly: `composition`, `composition_redistribution`,
    `composition_standardise`, `composition_residualise(_order)`, `composition_schedule` and
    `composition_sleeves`.
  - `running_rules` excludes a rerank-false standardise row. Base wrote nothing for it either, because
    `standardise_rule()` was empty.
  - Every rider parse (`theme_resid.cpp:150`, `theme_tsmom.cpp:66`, `two_speed.cpp:26`) already refuses when
    `std_themes` is empty. The base guard "riders only inside `if (!standardised.empty())`" is therefore
    equivalent.
  - `Json` is `nlohmann::json` (sorted keys), so the row iteration order cannot move bytes.
  - `RecipeTextPinned` pins the literals.
- **Combined manifest.**
  - Base wrote `composition_schedule` only when `rule==standardise`. tsmom refuses beside residualise, so the two are
    equivalent.
  - The sleeves rule `continue`s, so the `composition_sleeves` object written later is not overwritten.
  - `sleeve_fast` is non-empty exactly when `set_theme_sleeves` ran (`composition.cpp:428`). The new condition
    "sleeves row runs" is therefore the base `combined->sleeve_fast.empty()` test.
  - `pinned_signs` is `!pinned.signs.empty()`, as before.
- **Composition.**
  - `create_from_stages` calls `create(weights, composition_themes(), rule)` and then `schedule_theme_masses`, then
    `set_theme_sleeves`, which is the base call order. `stage_plan` maps the stage list to the base
    `theme_rule()`.
  - The schedule blocks use the same `lower_bound` on `role.session_keys`.
  - `for_each_centered_rank` sorts in place with the same `std::sort` on `(value, index)` and the same rank
    expression. The `finish` path's later `sum` over `p.row` therefore iterates in the same sorted order, which is
    bit for bit the same.
- **Admission.**
  - `ic_composition_stage_bytes` is `ic_composition_working_bytes` with the base `(composition_theme_count,
    theme_rule, !sleeves.empty())`. Every `b.add` keeps its order, width and message. The seven `term()` marks only
    observe `b.used`.
  - In non-plan-only runs, `within_budget` is called at the same two points with the same `enforce` predicate
    (train only when `!validation_only`).
  - Under `--plan-only` with `validation_only`, TRAIN is erased before `over` is computed (`runner.cpp:667`), so
    only scored roles can refuse.
  - `--cache-report` with `--plan-only` is refused (`runner.cpp:623`), so the cache-report early return cannot skip
    the budget.
- **Plan, summary and help.**
  - `within_budget`, `memory_terms` and `admission` appear only when over the cap. `theme_registry_sha256`
    appears only with the flag.
  - `research_cycle.exe_capabilities` (`--help` probe for `--no-composition` and `\bmarginal\b`) gives the same
    set.
- **Fitter.**
  - `PRIOR_WEIGHT_RULES` partitions `PRIOR_COMPOSITIONS`, so dispatch order cannot matter. Each row passes the
    v8 arguments.
  - The v6 empty-weights publish sits at the same point with the same reason.
  - `std` and `v6_detail` are `None` where base left them unbound, and they are read only under the same guards.
  - `PRIOR_THEMES` and `V7_APPENDED_THEMES` equal base values at d7c1c520.
  - Only `script_sha256` moves (PM7-30 substitution).
- **Deviation 3 (built-in 13-theme table): identity kept.**
  - The table equals the registry and base `theme_resid_order`, element for element.
  - The theme table only validates (theme-resid-v1's order, two-speed-v1's set). It never enters any computed
    value.
  - With `--theme-registry` at today's registry, every output except the summary and plan key is therefore
    byte-identical by construction.
  - Drift is loud (a refusal), never silent (see findings 3 and 4).

### Stage list and memory admission: no under-count
Admission and composition both take their stage list from `running_rules(pinned)`, so a running stage cannot be
missed by one and seen by the other. The per-worker envelope is unchanged: `workers x (8 MiB + 64 KiB)`, plus
`workers x n x 1104` (with the 16 B ranked row), plus `workers x d x 64`. Schedule blocks (<= 4096 x 32 x 8 B) sit in
the fixed slack, as at base. The terms sum to `bytes`, which `PlanPrintsRequiredBytesOverCap` asserts.

### `/W4 /permissive- /WX` under clang-cl 18 (read, not built)
I found no hazard.
- **Initializers.**
  - All 8 designated `rule_table` rows set all 15 fields. clang 18 reports a missing designated field, and none is
    missing.
  - `Role::memory{}` has a default member initializer. clang's `-Wmissing-field-initializers` skips such fields
    (`hasInClassInitializer`), so the six-field `strategy_research_role.cpp:121` stays clean.
  - The test aggregates `Candidate{5}`, `IcStage{3}`, `IcCompositionStages{3}`, `Case{4}` and `IcThemeBlock{2}`
    are complete.
- **Switches and unused names.**
  - Each new switch (`stage_plan`, `stage_name`) covers every enumerator.
  - Every anonymous-namespace helper and constant in `strategy_ic_rules.cpp` and `strategy_ic_admission.cpp` is
    used, and `weights` and `blend_signs` in `score_role` remain used.
- **Types, includes and macros.**
  - There are no sign-mixed comparisons.
  - Enum-class relational `>` is valid.
  - `ATX_TRY_VOID` is `do{}while(0)`, so `if (...) ATX_TRY_VOID(...)` is safe.
  - Removing `<array>` and `<string_view>` from `strategy_ic_theme_resid.hpp` breaks no includer.
- **Tests.** The suite names `CompositionRules`, `IcAdmission` and `ThemeRegistryRunner` are unique across both test
  binaries. `ATX_IMPL_TESTS_DIR` is defined for every test target (`tests/CMakeLists.txt:21`).

### Python fitter plugin list keeps today's order
- `MODIFIER_MODULES` = (resid, erc, tsmom, two_speed), the base `add_argument` order.
- `PARENT_CHECKED_MODIFIERS` = (erc, tsmom, two_speed), the base `check_args` order, and it still runs after
  `PRIOR_ONLY` and `require_resid_order`.
- The modifier test asserts both.

### Rulings
- **P7.** Envelope `{schema: atx.composition-rules/v1, capabilities (sorted), rules}`. The rows carry `version`,
  `params_schema` and `incompatible`; the extra fields (`requires_any`, `recipe_key`, `verified`, `working_bytes`)
  are additive.
- **P8.** The local `sort_ranks` / `each_centered_rank` copy is deleted, and `marginal_rank_ic.cpp` is untouched.
- **P2.** One block appended at each CMake tail. Both are listed as cross-lane edits.
- **P12 and P19.** Untouched: `screen_v4` and the weights schemas are unchanged.
- **Scope.** Every file is in plan §2.2 row D1, plus the listed cross-lane CMake and test edits. No forbidden file
  (`strategy_marginal_ic.*`, the NAV files, `atx-db/`) is touched.
- **Blindness.** The lane's commands were pytest on synthetic data, code fingerprints and a literal checker. My own
  commands read code, registry metadata and pytest only.

## B1 adaptation
**At 01f20405, no B1 call site fails to compile or changes meaning after D1 merges, so B1 needs no source
adaptation.**

B1's only translation unit that includes `strategy_ic_detail.hpp` is `atx-impl/src/strategy_factors_verb.cpp`
(`:23`). It uses exactly three `ic_detail` functions, and D1 leaves all three declarations byte-identical:
- `icd::metadata_text` at `strategy_factors_verb.cpp:95`.
- `icd::hash_valid` at `:113` and `:282`.
- `icd::load_pinned_f64` at `:142`, a 7-argument call that matches `detail.hpp:327-328` at D1. The progress note
  says S1 defines it.

B1 never calls the five functions whose signatures D1 changed or removed:
- `admit`, `method_recipe` and `composition_weights` (changed);
- `theme_rule()` and `standardise_rule()` (removed).

It also never calls or declares the `composition_*` parses. It does not aggregate-initialise `ic_detail::Role`:
`strategy_factors_verb_test.cpp:272` declares its own test-local `struct Role`. D1's new names (`ThemeTable`,
`CompositionRule`, `RuleInputs`, `IcStage*`, `StagePlan`) do not collide with any B1 identifier. The B1 pytest
(`test_factor_series_admission.py`) uses `screen_v4`, `Context`, `factor_record`, `admission_csv` and constants that D1
did not touch. Only its `fit:2249-2259` comment line references go stale, which is cosmetic.

**`strategy_research_role.cpp:121`** is a base file, not B1's. It initialises 6 of 7 fields
(`icd::Role role{..., 0U, {}};`), and the seventh, `Role::memory{}`, is exempt from the warning. No change is
required. Appending `,{}` (giving `0U, {}, {}};`) is harmless and mechanical if root wants belt and braces.

**Merge work, all mechanical, which root does at the D1 merge:**
- **`atx-impl/CMakeLists.txt` tail.** A textual conflict among B1 (+8, `strategy_factors_verb.cpp`), S1 (+2) and D1
  (+8, `strategy_ic_rules.cpp`). Keep every block.
- **`atx-impl/tests/CMakeLists.txt` tail.** B1 (+4, target-tests) against D1 (+5, ic-tests). Keep both.
- **Rebuild.** Rebuild B1's targets after the D1 and S1 merges, as progress.md already notes for `load_pinned_f64`.

No lane is needed.

## Checked
- [x] .agents/cpp/agent.md §10 checklist applied to the diff:
  - no UB, narrowing or uninitialised variables;
  - error paths returned;
  - no dangling spans (`IcCompositionStages` borrows pinned weights only for the `create_from_stages` call, and
    `PinnedWeights::rules` points into the static table);
  - switches exhaustive and loops bounded;
  - `[[nodiscard]]` on the new public API.
- [x] Diff stays inside the brief's files in scope. The cross-lane edits are the 2 CMake tails and
      `test_composition_resid.py`, and all are listed.
- [x] Evidence in the report matches its claims. I re-ran both pytest sets (31 + 200 = 231) and checked the fixture
      literals and SHAs independently. The 100-column claim is wrong (minor 7).
