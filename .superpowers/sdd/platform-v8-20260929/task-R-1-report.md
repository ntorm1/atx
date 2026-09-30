# Task R-1 report: composition `ew-theme-std-v1` (S-1 + S-4 + tier re-grade)

Lane R1, worktree `C:/atx-wt/pool-10`, branch `feat/platform-v8-r1-20260929` (base: lane B, `6c7cb27e`; task W0-2 is
`39bacec7`). Nothing was built or run on real data (lane rules). Python tests ran on synthetic data only.

## What was built

### Rule as coded (registration verbatim in `atx-impl/tools/composition_rules.py` docstring and `RULE_TEXT`)

Split of work between fitter and runner:

- **Fitter (Python, `composition_rules.py`)** turns rules 1 (within-theme shares), 3 (1/T), 4 (cap) and 5 (re-grades)
  into one weight per admitted member: `w_k = (1/T) * score_k / sum_{theme(k)} score`, then the member cap `1/(2T)`.
  T = themes with >= 1 admitted non-degenerate member (the ew-theme-v1 admission and theme table, unchanged).
- **Runner (C++, `IcThemeRule::standardise`)** applies rules 1-3 per date: per theme the sum over present members of
  `w_k * s_k * rank_k` (missing member = neutral 0, its weight stays in the fixed divisor, i.e. no redistribution
  inside the theme; the fixed divisor `W_theme` cannot change the per-date order so it is not applied), then that
  composite is re-ranked (centred tied rank, [-.5, .5]) over the names with at least one present member and added to
  the blend as `W_theme * rank`, `W_theme` = sum of the theme's `w_k`. Without a binding cap `W_theme = 1/T` exactly
  (rule 3); a binding cap moves mass pro rata to the other themes (rule 4), so their `W_theme` grows.
- Cap (`member_cap`): repeat until no member exceeds `cap * (1 + 1e-12)`: members above the cap are set to it and
  frozen; each theme's excess goes to the unfrozen members of the OTHER themes pro rata to current weight. Infeasible
  (no other-theme receiver) is refused (`FitError` in the fitter).
- Re-grades `TIER_REGRADES_V8` = `res_mom_12_1 B+->B-`, `ear B+->C+`, `sue C+->C+` (declared no-op), `ins_opp B-->C+`,
  applied to the source tier; a source tier that is neither the declared from- nor to-grade is refused. Tiers come
  from `atx-impl/strategies/alphas/registry.json` (A-1 schema `atx.alpha-registry/v1`, per id, `tier_scores`) when
  it exists, else the library/recipe `tier` the fitter already reads, scored with `TIER_SCORES_DECLARED`
  `{A 1.0, A- .9, B+ .8, B .7, B- .55, C+ .4}`. The registry is NOT on this branch yet (lane A-1), so today the
  library tier is used; checked the v7.1 library: the four re-graded ids carry exactly the declared from-tiers and
  every tier in it is scored. No tier or weight reads a TRAIN statistic.

### C++

- `atx-engine/include/atx/engine/combine/group_rerank.hpp` (new, header-only generic kernel, namespace
  `atx::engine::combine`): `RankedName = std::pair<f64, usize>`; `for_each_centered_rank(std::vector<RankedName>&,
  Apply&&)`; `accumulate_group_cell(f64& cell, f64 contribution)` (NaN = no member present sentinel);
  `[[nodiscard]] Status add_group_rerank(span<const f64> plane, usize names, usize begin, usize end, f64 weight,
  span<f64> out, std::vector<RankedName>& scratch)`. Header-only because the IC composition TU is `/O2` in Debug
  while the engine library is `/Od`.
- `atx-engine/tests/combine/combine_group_rerank_test.cpp` (new; the combine group globs `*_test.cpp`).
- `atx-impl/src/strategy_ic_composition.hpp/.cpp`: `enum class IcThemeRule : u8 { redistribute, standardise }`;
  trailing defaulted `IcThemeRule rule = redistribute` on `ic_composition_working_bytes` and
  `IcComposition::create`; new private `add_standardised`; `Impl` gets `std_theme`, `std_mass`, `std_plane`.
  The existing pinned and v6 paths are untouched (a second path, selected only when themes are passed with
  `standardise`). Working bytes: standardise = 1 f64 plane per theme (v6 redistribute keeps 2).
- `atx-impl/src/strategy_ic_detail.hpp`: `theme_standardise_rule = "ew-theme-std-v1"`; `PinnedWeights` gets
  `std_themes`, `std_theme_count`, `standardise` and `theme_rule()`, `composition_themes()`,
  `composition_theme_count()`; `method_recipe(..., bool standardised=false)`; `admit(..., IcThemeRule rule=redistribute)`.
- `atx-impl/src/strategy_ic_admission.cpp`: shared `theme_indices` (the v6 checks, messages unchanged, count message
  names the block); `composition_standardise` parses `theme_standardise` = exactly
  `{"rule":"ew-theme-std-v1","rerank":true|false,"themes":{id: theme}}` (themes validated also when rerank is false);
  `theme_redistribution` and `theme_standardise` are exclusive; v2 requires one of the two blocks; `theme_standardise`
  requires schema v2; `weights_summary.standardise` = `"ew-theme-std-v1"` or `"ew-theme-std-v1;rerank-off"`;
  recipe `composition` string gains `...;theme-rerank-centered-tied-over-names-with-a-present-member;
  theme-weight-sum-of-member-weights` and `composition_standardise` only when rerank is true.
- `atx-impl/src/strategy_ic_runner.cpp`: `score_role(..., IcThemeRule)`, combined manifest key
  `composition_standardise` (rerank true only), admission budget uses `composition_theme_count()` + `theme_rule()`,
  help text.

### Python

- `atx-impl/tools/composition_rules.py` (new): `STD_RULE_ID`, `TIER_SCORES_DECLARED`, `TIER_REGRADES_V8`,
  `CAP_TOLERANCE`, `REGISTRY_PATH`, `RuleError`; `load_registry`, `resolve_tiers`, `apply_regrades`, `tier_weights`,
  `member_cap`, `StdFit`, `ew_theme_std(ids, themes, prior_tiers, registry_path=None, error=RuleError)`,
  `attach_std(document, fit)` (schema v2, `theme_standardise` {rule, rerank true, themes}, `provenance.std` with the
  regrade table and statuses, tier source and registry SHA-256 when present, cap passes, module SHA-256),
  `identity_document(accepted)` and CLI verb `identity-weights`.
- `atx-impl/tools/fit_composition_weights.py`: `--composition ew-theme-std-v1` (a prior composition: needs
  `--orientation prior`; reuses the ew-theme-v1 admission, screen and theme table).

## How root verifies

### Build targets and gtest filters

```powershell
powershell scripts\atx-build.ps1 build atx-engine-combine-tests
build\bin\atx-engine-combine-tests.exe --gtest_filter=GroupRerank.*
powershell scripts\atx-build.ps1 build atx-impl-strategy-ic-tests      # owning target of strategy_ic_*_test.cpp
build\bin\atx-impl-strategy-ic-tests.exe --gtest_filter=CompositionV8.*:StrategyIcComposition.*:StrategyIcRunner.*
powershell scripts\atx-build.ps1 build atx-equity-strategy-ic          # the runner exe for the cells
```

(If the IC tests live in a differently named target on the integration branch, it is the one that owns
`atx-impl/tests/strategy_ic_composition_test.cpp` and `strategy_ic_runner_test.cpp`.)

New gtests: `GroupRerank.{CenteredTiedRanksFollowTheBlockFormula, AccumulateReplacesTheSentinelThenSums,
RerankAddsWeightedRanksOverPresentNamesOnly, EveryGroupEntersWithTheSameDispersion, RefusesBadShapesRangesAndWeights}`;
`CompositionV8.{OneMemberThemeHasSameDispersionAsOthers, MemberCapRedistributes, StandardiseNeedsThemesAndPinnedWeights}`
(composition test file); `CompositionV8.{IdentityWithReRankAndCapOffIsEwThemeV1,
ThemeStandardiseRefusalsPrecedeAnyPayloadOrOutput}` (runner test file). Every existing StrategyIcComposition /
StrategyIcRunner test must still pass (v6 messages kept).

Python:

```
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_composition_rules.py atx-impl/tools/test_fit_composition_weights.py
```

`test_composition_rules.py`: `test_tier_weights_sum_to_theme_share`, `test_member_cap_redistributes`, declared
constants, regrade table, registry-first/library fallback, one-member theme capped, refusals, fitter end to end
(weights follow the rule; the document is what the runner reads), identity graft.

### Identity cell (step 3, before the trial)

Identity device: `rerank: false` in the `theme_standardise` block makes the runner use the plain pinned-weights path,
so with the accepted ew-theme-v1 weights (equal within-theme shares, no cap) the blend is the ew-theme-v1 blend by
construction; only the weights pin (and so `composition_weights_sha256`, `run_recipe_sha256`,
`summary.weights.standardise`) differs. Using the v7.1 cell as the example parent (substitute the current parent):

```
"C:/Program Files/Python312/python.exe" atx-impl/tools/composition_rules.py identity-weights ^
  --weights build-equity/mega-weights-v71-ew/composition_weights.json ^
  --weights-sha256 7b0a59c96284c85cda08abe89852169757f3730a37df6c33b72e602d8c29010f ^
  --out build-equity/mega-weights-v8-r1-identity.json
```

(prints `{"out", "sha256", ...}`; refuses an existing `--out` or a non-ew-theme-v1 source.) Then re-run the parent's
exact IC `w` argv (`mega-v71w-train-ew-run1/receipt.json` `command`) with only `--output
build-equity/mega-v8-r1-identity-w` and `--composition-weights build-equity/mega-weights-v8-r1-identity.json
--composition-weights-sha256 <printed sha256>` changed. Byte-identical against the parent pass:
`train_combined.f64`, `train_combined_member.u8`, `train_combined_finite.u8`, `train_combined_ids.u64`,
`train_combined_sessions.i64`, `train_planned_targets.csv`, and the `__combined__` rows of `train_daily_ic.csv`
(runner test `CompositionV8.IdentityWithReRankAndCapOffIsEwThemeV1` checks the same set on synthetic data, also that
rerank true changes the blend). `train_combined.json` differs only in `composition_weights_sha256` and
`run_recipe_sha256`.

### Trial fit (step 3)

The parent's fitter argv (`mega-weights-v71-ew-run1/receipt.json`) with `--composition ew-theme-std-v1` (keep
`--orientation prior --screen v4-prior-v1`) and a new `--output`/`--work-dir`. Output: schema v2 with the
`theme_standardise` block (rerank true); `provenance.std` records tier source, regrade statuses, cap passes. The IC
`w` pass then takes that file as `--composition-weights`; recipe and combined manifest carry
`composition_standardise: ew-theme-std-v1`.

## Deviations from the brief (with reasons)

1. Brief line ranges (`strategy_ic_composition.cpp:174-196,235-239`, `fit_composition_weights.py:193,1366,1399-1406`)
   were for the pre-split files; the edits sit where the code now is (see cross-lane list).
2. Identity switch is the block's `rerank` flag (false = plain pinned path), not a separate CLI flag: the brief says
   the rule is selected by the weights file; the flag makes the identity run need no new runner argument.
3. "Cap off" in the identity cell = the source ew-theme-v1 weights (no cap was ever applied to them); the fitter
   itself always applies the cap (the rule has no cap-off switch; adding one would be an unregistered variant).
4. Infeasible cap (all excess in a theme with no other-theme receiver, e.g. T = 1) is refused rather than left
   uncapped.
5. `pooled_date_bands` in the composition TU repeats the pinned path's band split (quotient/remainder bands, one
   ranked row per worker) instead of refactoring the pinned `add` onto it: the brief forbids touching the existing
   path (byte identity); a follow-up could route the pinned path through it.
6. The within-theme mean's divisor is not applied (order-preserving constant per theme; applying it could only
   introduce rounding ties); documented in the kernel header.

## Cross-lane edits

- `atx-impl/tools/fit_composition_weights.py` (lane C edits it concurrently), 10 added lines, nothing removed:
  - after line 149 (`import numpy as np  # noqa: E402`): `import composition_rules  # noqa: E402 ...` (+ blank), lines 150-151;
  - after line 175 (`COMPOSITIONS = (...)`): `PRIOR_COMPOSITIONS, COMPOSITIONS = (... + (composition_rules.STD_RULE_ID,), ...)`, lines 176-177;
  - in `fit_prior`, before the `else:` of the composition dispatch (`weights, theme_table = ew_theme_weights(...)`):
    `elif args.composition == composition_rules.STD_RULE_ID:` + 3 lines, lines 1934-1937;
  - in `fit_prior`, before `files[OUTPUT_WEIGHTS] = canonical_bytes(document)`:
    `if args.composition == composition_rules.STD_RULE_ID: composition_rules.attach_std(document, std)`, lines 2004-2005.
- `atx-impl/tools/test_fit_composition_weights.py` line 1949: the `PRIOR_COMPOSITIONS` pin gains `"ew-theme-std-v1"`.
- `atx-impl/src/strategy_ic_{admission,runner}.cpp`, `strategy_ic_detail.hpp`, `atx-impl/tests/strategy_ic_runner_test.cpp`
  (appended tests + helpers `std_block`, `combined_rows`, `vee_library` at the end of the file) -- lane B's split files.
- No pinned source file edited: `dsl_vm_sources` / `ic_result_sources` list engine files only and none includes
  `group_rerank.hpp`; no digest changed.

## Open risks

- **Memory:** standardise adds 8 B per cell per theme to the composition admission. v7.1 lo1 TRAIN (1,155 x 5,627,
  T = 10): +496 MiB over ew-theme-v1 (v6 redistribute would add 992 MiB). A 4-year 2020-2023 role adds more
  (~T x D x N x 8). The runner refuses before any payload or output if the budget is exceeded (tested), so the
  risk is a refused cell at `--max-memory-mib 1536`, not a crash; root should read the admission line first.
- C++ was written without compiling (lane rule); /W4 /WX self-review done (no unused names, no sign conversions,
  `[[nodiscard]]` results consumed with `ATX_TRY_VOID`).
- Registry path is implicit (`atx-impl/strategies/alphas/registry.json`); when A-1 lands, tiers switch from the
  library to the registry. `provenance.std` records which source and its SHA-256, so a changed tier source is visible.
- The per-theme re-rank fold in `finish()` is serial (T x D x N sort work once per role).
- Git warns LF->CRLF on the new files (repo attributes); content is LF.
