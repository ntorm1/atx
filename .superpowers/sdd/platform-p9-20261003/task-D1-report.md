# Lane D1 report

## Outcome
DONE_WITH_CONCERNS. The composition rule table, the ordered stage list, `--list-rules --json` (K-P9-6), the registry
theme table and the fitter's plugin list are in place. Flag-absent runs are value-preserving by construction: same
parse order, messages, recipe and manifest keys, call sequence and admission bytes. The concerns: the C++ is written
and unbuilt (lane rule), and two merge interactions are listed under open risks.

## Branch / SHA
`feat/p9-d1-20261003`. The code commits are `b1764d22` (C++), `f672819c` (gtests and fixture) and `85b19345` (fitter
and pytest). The report is committed on top of them. All work is committed and the leased tree is clean.

## Frozen base / lease
- base_sha=d7c1c520c3caa162ef453349b256669ce8de3c80
- worktree=C:\atx-wt\pool-20; lease_name=pool-20
- lease_run_id=p9-d1-20261003; agent=p9-d1; owner_kind=heartbeat
- keeper_pid=11120; acquired=2026-10-03T10:09:54.1489497Z

Root leased the pool. The lane did not lease or release it.

## Acquisition receipt
Root acquired the lease. The lane only read its status with `powershell -NoProfile -File scripts/lease-worktree.ps1
-Status` (exit 0):
```
pool-20  [cold]  branch=feat/p9-d1-20261003  LEASED run_id=p9-d1-20261003 | agent=p9-d1 | owner_kind=heartbeat | owner=alive | keeper_pid=11120 | branch=feat/p9-d1-20261003 | acquired=2026-10-03T10:09:54.1489497Z
```

## Files changed

### New
- `atx-impl/src/strategy_ic_rules.hpp` and `strategy_ic_rules.cpp`. Contents:
  - The `CompositionRule` table, eight rows in the v8 parse order: within-theme-v1, ew-theme-std-v1, ic-shrink-v1,
    ic-shrink-aim-v1, theme-erc-v1, theme-resid-v1, theme-tsmom-v1, two-speed-v1.
  - Each row carries id, version, block_key, stage, parse, verify, recipe_key, recipe_text, working_bytes,
    params_schema, incompatible and requires_any, plus the R6B-C-5 metadata rerank_off, also_written_by and
    identity_source.
  - `parse_composition_rules` runs each block key's shared parse once, in table order, then refuses incompatible
    pairs.
  - `running_rules`, `composition_stages` (the ordered stage list), `rule_recipe_json` and `list_rules_json` (the
    envelope required by P7).
  - The theme table: `theme_table_from_registry` reads the registry in document order and refuses duplicate keys. It
    also holds `builtin_theme_table` and `theme_table(cfg)`.
  - The grouping and verify code moved verbatim from `strategy_ic_admission.cpp`.
- `atx-impl/tests/strategy_ic_rules_test.cpp`, with the gtests named in the brief plus `.ListRulesJsonPinned` and
  `.StageListIsTheV8CallSequence`.
- `atx-impl/tests/fixtures/composition_rules_list.json`: the pinned K-P9-6 envelope.
- `atx-impl/tools/test_fit_composition_rule_plugins.py`: pytest for the plugin list and the registry theme read.

### Modified, C++
- `strategy_ic_composition.{hpp,cpp}`:
  - Adds `IcStageKind`, `IcStage`, `IcCompositionStages`, `IcComposition::create_from_stages` (create, then
    `schedule_theme_masses`, then `set_theme_sleeves`) and `ic_composition_stage_bytes`.
  - Ranking now goes through engine `group_rerank.hpp` `for_each_centered_rank` (P8). The local `sort_ranks` and
    `each_centered_rank` copies are removed. `RankedName` is the same `std::pair<f64,usize>`, so the envelope is
    unchanged.
- `strategy_ic_detail.hpp`:
  - Adds `PinnedWeights::rules` and `Role::memory`. `memory` has a default member initializer so that
    `strategy_research_role.cpp`'s six-field aggregate stays warning-free.
  - Removes `theme_rule()` and `standardise_rule()`.
  - New signatures: `method_recipe(cfg, parallel_ic, pinned_signs, const PinnedWeights*)`,
    `admit(..., const PinnedWeights&)`, a new `within_budget`, and `composition_weights(cfg, lib, const ThemeTable&)`.
- `strategy_ic_admission.cpp`:
  - `method_recipe` is driven by the table.
  - `admit` documents the memory model (worst candidate's VM slots plus the theme planes) and records its seven named
    terms.
  - `within_budget` refuses with the v8 message.
  - `composition_weights` calls `parse_composition_rules`.
- `strategy_ic_runner.{hpp,cpp}`:
  - `score_role` and `save_combined_artifact` take `PinnedWeights` in place of the 20-parameter tail.
  - New options `--theme-registry FILE` and `--theme-registry-sha256 SHA`. `summary.json` and the plan record
    `theme_registry_sha256` only when the option is given.
  - `--list-rules --json`: exit 0. Any other argv given with `--list-rules` exits 2.
  - `--plan-only` over the cap prints the plan, with `within_budget`, `memory_terms` and
    `admission: refused-required-bytes-exceed-max-working-bytes`, then refuses with the unchanged message (exit 1).
- `strategy_ic_theme_resid.{hpp,cpp}`: `theme_resid_order` is removed. The registered order now comes from the theme
  table. Messages are unchanged.
- `strategy_ic_theme_tsmom.cpp` and `strategy_ic_two_speed.cpp`: the parses take `RuleInputs`. two-speed-v1 also
  refuses a weighted theme that is not in the theme table.
- `strategy_two_speed.hpp`: comment only. The half-lives are the rule's parameters and the theme set is the theme
  table's.

### Modified, Python
- `atx-impl/tools/fit_composition_weights.py`:
  - `PRIOR_WEIGHT_RULES` and `prior_weight_rule` replace the if/elif in `fit_prior`.
  - `MODIFIER_MODULES` and `PARENT_CHECKED_MODIFIERS` drive `add_argument` and `check_args`.
  - `PRIOR_THEMES` and `V7_APPENDED_THEMES` are read from `registry.json` at import (`registered_prior_themes`, which
    checks the V4 prefix). `V7_APPENDED_THEMES_OF_RECORD` is the fallback when the file is absent.
- `atx-impl/tools/test_composition_resid.py`: the regex now reads `builtin_registry_themes` in `strategy_ic_rules.hpp`.

### Cross-lane edits
- `atx-impl/CMakeLists.txt`: one appended block. It adds `target_sources(atx-impl-core PRIVATE
  src/strategy_ic_rules.cpp)` plus the same Debug `/O2` and SKIP_PCH flags as the other `strategy_ic_*.cpp` files.
- `atx-impl/tests/CMakeLists.txt`: one appended registration, `target_sources(atx-impl-strategy-ic-tests PRIVATE
  strategy_ic_rules_test.cpp)` (ruling P2).
- `atx-impl/tools/test_composition_resid.py`: the C++ pin path (above).

## Evidence
1. The new pytest plus the theme-resid pins, run from `C:/atx-wt/pool-20/atx-impl/tools`:
   `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider test_fit_composition_rule_plugins.py
   test_composition_resid.py`
   exit_code=0
   ```
   ...............................                         [100%]
   31 passed, 17 subtests passed in 5.24s
   ```
2. Every fitter and composition test, same directory. The baseline at d7c1c520 was 223 passed, exit 0; the extra 8
   are the new pytest.
   `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider test_fit_composition_weights.py
   test_fit_composition_weights_pool.py test_fit_composition_weights_store.py test_composition_ic_shrink.py
   test_composition_resid.py test_composition_rules.py test_composition_theme_erc.py test_composition_theme_tsmom.py
   test_composition_two_speed.py test_fit_composition_rule_plugins.py`
   exit_code=0
   ```
   ........................................................................ [ 31%]
   ........................................................................ [ 62%]
   .......................................................                  [ 86%]
   ................................                                         [100%]
   231 passed, 17 subtests passed in 64.13s (0:01:04)
   ```
3. Producer fingerprints after the fitter edit, against the base values recorded before any edit. Command:
   `python <scratchpad>/fp_now.py`, which imports the pool-20 fitter and calls `producer_fingerprint` and
   `horizon_fingerprint`. exit_code=0
   ```
   FACTOR_PRODUCERS 962cd507af511fe3a907ab63bb5109c1f2044dde1eca0cfa13ea59715dda0f4a OK
   CONTEXT_PRODUCERS 70e1eda51539c3536d9f46cc5c9f7091222b294fbd8c074bb07d689d46fe71b3 OK
   AIM_PRODUCERS f1fb0d3a5183a723bc561cec124121e28a07a54abe667be6ab85f18b6cf80bec OK
   AIM_ERA_PRODUCERS 9a880f97b42fdae67d859c8d505a611e896e1cdeab73077429c23e72292fba34 OK
   horizon a80e06f9d3aaac1604cdadfd1aa949cc8feeeedd76bfeadf73757ba7cfd58fce OK
   PRIOR ('value', ..., 'merger_arbitrage') OK
   ```
4. C++ schema literals and recipe texts against the generator that wrote the fixture: `python
   <scratchpad>/check_literals.py`, exit_code=0. All 8 params_schema literals, `redistribute_text`, `standardise_text`
   and `grouping_bytes` printed `OK`. The regenerated fixture is JSON-equal to the committed one (`fixture equal`,
   exit_code=0).

The C++ was not compiled or run (lane rule 1). Lines were checked by script: no new line exceeds 100 columns.

## How root verifies

**Builds.** Run these through root's equity presets / wrapper:
- `atx-equity-strategy-ic`
- `atx-impl-strategy-ic-tests`
- `atx-impl-strategy-target-tests` (for `TwoSpeed.*`)
- `atx-impl-tests`, which picks up `strategy_ic_rules_test.cpp` through its glob

**New gtests:**
```
atx-impl-strategy-ic-tests --gtest_filter=CompositionRules.*:IcAdmission.*:ThemeRegistryRunner.*
```

**Regressions on the touched code:**
```
atx-impl-strategy-ic-tests --gtest_filter=StrategyIcRunner.*:CompositionV8.*:ThemeResidRunner.*:ThemeTsmomRunner.*:TwoSpeedRunner.*:NoComposition.*:FieldCaps.*:Workers.*:StrategyIcComposition.*:ThemeResid.*:ThemeTsmom.*:IcShrinkV1.*:IcShrinkAimV1.*:ThemeErcV1.*:MarginalIc.*
atx-impl-strategy-target-tests --gtest_filter=TwoSpeed.*
```
`StrategyIcRunner.AdmissionReportsRequiredBytes` was updated: under `--plan-only` the log now holds the printed plan,
and the refusal and message are unchanged.

**K-P9-6.** `bin\atx-equity-strategy-ic.exe --list-rules --json` must be JSON-equal to
`atx-impl/tests/fixtures/composition_rules_list.json` (`ListRulesJsonPinned` asserts it). Compare JSON, not bytes:
the fixture's line endings follow autocrlf.

**X-5 identity, flag absent.** Repeat R0-3's procedure (progress.md: X-5 under v8-16d) with D1's exe and fitter:
- `u` (`--no-composition`) and `w` (the weighted run): byte-identical, except the timing/cache-only files R0-3
  already allows.
- `fit` (`fit_composition_weights.py` with X-5's argv): identical after the PM7-30 substitutions. Its
  `script_sha256` changes because the file changed. Producer fingerprints are unchanged (Evidence 3), so the store
  records are reused.

**Flag present.** Append `--theme-registry atx-impl/strategies/alphas/registry.json --theme-registry-sha256
<sha256 of that file>` to X-5's `w` argv. Expected: every output file byte-identical except that `summary.json` gains
`theme_registry_sha256`. `ThemeRegistryRunner.RegistryOrderIsTheRegisteredOrder` pins this on synthetic data.

## Deviations from brief
1. **`working_bytes` is a documentation string.** It names the composition envelope term. The bytes themselves are
   computed by `ic_composition_stage_bytes` from the stage kinds, so the v8 envelope arithmetic stays in one place.
2. **The two-speed half-life table stays.** It is two-speed-v1's registered parameters, not a theme list.
   two-speed-v1 now checks each weighted theme against the theme table, so the theme set comes from the table.
   `TwoSpeed.RegisteredTable` is unchanged.
3. **A built-in theme table stays.** Without `--theme-registry` the exe uses the registry's 13 themes at the base,
   which keeps the flag-absent output identical until E2 passes the flag on every IC argv. `test_composition_resid.py`
   pins it as the registry's prefix, and `ThemeTableFromRegistry` pins it as a prefix in C++. It retires with E2.
4. **`--plan-only` over the cap behaves differently.** It now prints the plan, then exits 1 with the same message.
   Before, it exited 1 with no stdout. The brief asked for this. Consumers that check `returncode` first, such as
   `generate_library.exe_plan`, are unaffected. Over the cap, plan-only now runs field binding before refusing, so a
   run that has both an over-cap role and a bad fields pin reports the fields error.
5. **The riders' exclusions are still refused by their parses first, with the v8 messages.** This covers resid with
   tsmom or sleeves. The table's `incompatible` lists the same pairs as metadata and as a backstop.
   `refuse_incompatible` decides only redistribution against standardisation, with the v8 message, after all parses,
   as v8 did.
6. **The fitter reads `PRIOR_THEMES` from the registry at import.** A malformed registry, or one that breaks the
   frozen V4 prefix, raises `FitError` at import (loud). An absent file falls back to the list of record.

## Open risks
- **B1.** B1 includes `strategy_ic_detail.hpp` (progress: slot B1 6 before D1 7). If B1's code calls `admit`,
  `method_recipe` or `composition_weights`, uses `theme_rule()` / `standardise_rule()`, or declares the moved
  `composition_*` parses, it needs the new signatures at merge. Adding `Role::memory` with a default initializer keeps
  existing six-field `Role{...}` aggregates compiling.
- **`Role::memory` and the warning check.** The change relies on clang's `-Wmissing-field-initializers` skipping a
  field that has a default member initializer. If the build flags `strategy_research_role.cpp:121` regardless, the
  one-token fix is appending `,{}` to that brace-init.
- **S1.** S1 also appended one `target_sources` line to `atx-impl/CMakeLists.txt`, which will conflict textually at
  the tail. Root resolves it; both lines stay.
- **The `--list-rules` scan.** It looks at every argv token, so a path argument literally named `--list-rules` would
  be taken as the flag.
- **The fixture.** It was generated by a scratchpad script that mirrors the C++ table. `ListRulesJsonPinned` is the
  authority. If a gtest diff appears, regenerate the fixture from the exe output after reviewing the diff.

## Ledger candidates
- P9 D1: the IC runner's composition rules live in one table, `strategy_ic_rules.cpp` (K-P9-6 `--list-rules
  --json`). A new rule is one row plus its parse and verify. The theme order comes from `--theme-registry`, or the
  built-in 13-theme table when the flag is absent.
- P9 D1: `fit_composition_weights.PRIOR_THEMES` is `registry.json`'s themes table at import (V4 prefix checked).
  Registering a theme is one edit to the registry. The fitter follows automatically; the exe follows through
  `--theme-registry`.
- clang `-Wmissing-field-initializers` skips fields that have a default member initializer. Adding a field `T x{};`
  to an aggregate keeps older shorter brace-inits warning-free (`Role::memory`).
