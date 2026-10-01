# Task R-11 report: composition rule `theme-resid-v1` (lane ORTH, Ruling E-38 slot 50)

Lane ORTH, worktree `C:/atx-wt/pool-4`, branch `feat/platform-v8-orth-20260930` from `fd2ff7a8`. Nothing was built
(lane rule 2) and no real data was read. Python tests ran on synthetic data only. The C++ test vectors were also run
through a loop port of the kernel and of the composition (same operation order): every expected value held.
Commits: `19cc08ef` (engine kernel and its tests), `75534a36` (the C++ rule, wiring and tests), `121bfb15` (fitter
path, numpy reference, cell template), then this report.

## 1. Registration (every constant fixed blind, before any read; E-30 precedent)

Hypothesis: theme composites residualised against the preceding themes carry uncorrelated alpha and combine better
than the raw composites.

| item | registered value |
|---|---|
| theme order source | `composition_resid.REGISTERED_THEME_ORDER` = `fit_composition_weights.PRIOR_THEMES` (v4 pre-registration list + the v7 appended theme) = the alpha registry's `themes` table in file order: value, profitability_quality, investment_issuance, earnings_momentum, price_momentum, low_risk, short_interest, reversal_seasonality, options_implied, ownership_flow. Restricted to the themes with a weighted member. A theme outside the list is refused. A frozen constant; no runtime read (tests pin it against `PRIOR_THEMES` and the registry). |
| composite | the parent's standardised theme composite z_t (ew-theme-std-v1): per session the centred tied rank, over the names with a present member, of the sum of present w_k s_k rank_k. A lone present name has z = 0. |
| regression | theme at position t >= 2: cross-sectional least squares of z_t on an **intercept** and z_1 .. z_{t-1} (the preceding *standardised composites*, not their residuals), over the names where t is present. A preceding theme absent for a name enters as 0, its neutral value. |
| solver and tolerance | modified Gram-Schmidt with one re-orthogonalisation pass. A regressor whose part outside the span of the intercept and the earlier kept regressors is at most 1e-10 of its centred norm is dropped (the residual is unique). A residual at most 1e-10 of the centred norm of z_t is exactly 0: theme t adds nothing that session. |
| re-standardisation | the residual is re-ranked: centred tied rank in [-.5, .5] over the same names (the ew-theme-std-v1 standardisation). |
| first theme | keeps z_1: it adds exactly what ew-theme-std-v1 adds for it (bit for bit). |
| combination | blend = sum over themes of W_t x re-standardised composite; W_t = the parent's theme share (sum of the theme's member weights). Member weights inside each theme, signs, tiers and the member cap are the parent's, unchanged. |
| domain | defined only on a parent whose weights file carries `theme_standardise` with rerank true; the fitter refuses `--theme-resid` otherwise. |
| acceptance | pre-registration rule 5: paired S2 net dSR > 0 against the parent AND mechanics. The task names no further mechanical criterion; planned turnover per unit gross is reported beside it and gates nothing. |

## 2. What was built

### C++, generic (atx-engine)
- `atx-engine/include/atx/engine/combine/group_residualise.hpp` (new, header-only like `group_rerank.hpp`, because the IC
  TUs are /O2 and the engine library is /Od in Debug): `kResidualSpanTolerance = 1e-10`; `struct ResidualFit {usize
  rank; bool spanned;}`; `[[nodiscard]] Result<ResidualFit> residualise_in_place(span<f64> y, span<f64> columns,
  f64 tolerance = kResidualSpanTolerance)`. Allocation free; refuses n == 0, ragged columns, non-finite values and a
  tolerance outside (0, 1), leaving both spans untouched.
- `atx-engine/tests/combine/combine_group_residualise_test.cpp` (new; combine group glob).

### C++, strategy (atx-impl)
- `atx-impl/src/strategy_ic_theme_resid.{hpp,cpp}` (new): `[[nodiscard]] Status add_theme_residualised(span<vector<f64>>
  planes, span<const f64> mass, usize names, span<f64> out, vector<pair<f64,usize>>& row)` (per date: standardise
  every theme row in place, then theme 0 adds W_0 z_0, theme t adds W_t x re-rank of the residual); and
  `ic_detail::composition_residualise(const Json&, const Library&, PinnedWeights&)` (parses the block, remaps each
  weighted candidate's theme index to its theme's position in `order`).
- Weights file block (schema v2, beside `theme_standardise` with rerank true):
  `"theme_residualise": {"rule": "theme-resid-v1", "order": [theme, ...]}`; `order` must name each weighted theme of
  the standardise block exactly once.
- Wiring: `IcThemeRule::residualise` (strategy_ic_composition.hpp); composition create/finish/working bytes (+N x
  (8T + 8) B scratch); `PinnedWeights::residualise` + `theme_rule()`; `method_recipe(..., residualised)` adds
  `recipe.composition_residualise`; combined manifest `composition_residualise`; `summary.composition_weights.residualise`;
  `--help` text. All keys absent without the block.
- C++ rule-table registration: one line in `strategy_ic_detail.hpp`
  (`theme_residualise_rule="theme-resid-v1"`) and one enumerator.
- CMake: `src/strategy_ic_theme_resid.cpp` in `atx-impl-core` and in both /O2 property lists (same flag set as every
  strategy_ic_*.cpp); `atx-impl-strategy-ic-tests` gains the two test files.

### Python
- `atx-impl/tools/composition_resid.py` (new): `RULE_ID`, `BLOCK`, `REGISTERED_THEME_ORDER`, `SPAN_TOLERANCE`,
  `RULE_TEXT`, `theme_order`, `resid_block`, `attach` (block + `provenance.resid`), `add_argument`, `apply` (the fitter
  hook), and the numpy reference of the runner rule (`centred_tied_ranks`, `residual` by `lstsq`, `blend`).
- Fitter path: `fit_composition_weights.py --theme-resid theme-resid-v1` on the parent's fit argv. The parent's
  composition is fitted unchanged and the block is attached; summary key `theme_residualise`.
- `atx-impl/tools/test_composition_resid.py` (new).
- Cell template `scripts/specs/v8/r11.json`: nominal parent `r1-comp-v8.json`; change `fit --theme-resid
  theme-resid-v1`; fit, card, w, nav, monitor renamed; the u pass and E-28's 3,072 MiB are inherited.

## 3. How root verifies

```powershell
powershell scripts\atx-build.ps1 check atx-impl\src\strategy_ic_theme_resid.cpp
powershell scripts\atx-build.ps1 build atx-engine-combine-tests
build\bin\atx-engine-combine-tests.exe --gtest_filter=GroupResidualise.*:GroupRerank.*
powershell scripts\atx-build.ps1 build atx-impl-strategy-ic-tests
build\bin\atx-impl-strategy-ic-tests.exe --gtest_filter=GroupResidualise.*:ThemeResid.*:ThemeResidRunner.*:CompositionV8.*:StrategyIcComposition.*:StrategyIcRunner.*:MarginalIc.*
powershell scripts\atx-build.ps1 build atx-equity-strategy-ic
```

New gtests: `GroupResidualise.{TwoGroupsMatchTheClosedFormAndAreOrthogonal, TwoRegressorsMatchCramerAndAreOrthogonal,
DependentColumnsAreSkipped, SpannedDependentBecomesExactlyZero, NoRegressorIsTheDemeanedVector,
RefusesBadShapesNonFiniteValuesAndTolerances}`; `ThemeResid.{TwoThemesMatchTheClosedForm,
ThreeThemesEqualTheRegisteredRule, FirstThemeIsTheStandardisedCompositeBitForBit, EnvelopeAddsOnlyTheRegressionScratch,
KernelRefusesMismatchedShapes}`; `ThemeResidRunner.{ResidualisesInTheBlockOrderAndRecordsTheRule,
BlockRefusalsPrecedeAnyPayloadOrOutput}`. Every existing CompositionV8 / StrategyIcComposition / StrategyIcRunner /
MarginalIc test must pass unchanged (flag absent).

Brief's tests:
- closed-form two-theme fixture, residual orthogonal to the preceding composite to 1e-12:
  `GroupResidualise.TwoGroupsMatchTheClosedFormAndAreOrthogonal` (kernel), `ThemeResid.TwoThemesMatchTheClosedForm`
  (composition against the closed form), Python `test_two_theme_residual_is_the_closed_form_and_orthogonal`.
- Python equals C++: `ThemeResid.ThreeThemesEqualTheRegisteredRule` and Python `test_three_themes_equal_the_registered_rule`
  build the same 3-theme fixture (missing members, an absent theme, a sign -1, a nonmember, a session where theme b
  equals theme a and so adds nothing) and both compare against the same exact values of the rule (rational
  arithmetic). The numpy reference uses `lstsq`, the C++ Gram-Schmidt.
- flag absent identity: `ThemeResid.FirstThemeIsTheStandardisedCompositeBitForBit`,
  `ThemeResid.EnvelopeAddsOnlyTheRegressionScratch`, `ThemeResidRunner.ResidualisesInTheBlockOrderAndRecordsTheRule`
  (one theme: combined, finite mask and planned targets byte-identical to ew-theme-std-v1; no rule keys without the
  block), Python `FitterEndToEnd.test_resid_is_the_parent_weights_plus_the_block` (the file minus the block and
  `provenance.resid` is the parent file byte for byte).

Python:
```
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_composition_resid.py atx-impl/tools/test_composition_rules.py atx-impl/tools/test_fit_composition_weights.py scripts/tests/test_research_spec.py
```
Results on this branch: `atx-impl/tools` 475 passed, 2 skipped (10 new); `scripts/tests` 165 passed, 4 skipped
(`test_research_spec.py` 33, one new). C++ not built.

### Identity runs (flag absent, before the cell)
1. IC w pass: re-run the accepted parent's w-pass argv (its receipt `command`) on the new build with only `--output
   build-equity/mega-v8-r11-identity-w` changed. Byte-identical against the parent pass: `train_combined.f64`,
   `train_combined_member.u8`, `train_combined_finite.u8`, `train_combined_ids.u64`, `train_combined_sessions.i64`,
   `train_combined.json`, `train_planned_targets.csv`, `train_daily_ic.csv`, `recipe.json` (no block, so no key).
2. Fitter: re-run the parent's fit argv into a new `--output`. `admission.json` and `composition_weights.json` are
   identical except `script_sha256` (provenance and inputs: the fitter file changed by four lines); the weights,
   signs and blocks are identical.
3. The cell: set `parent` in `r11.json` to the last accepted cell's spec, `lock --write`, run. The fit output must
   carry `theme_residualise.order` = the registered order restricted to the parent's themes.

## 4. Deviations from the brief (with reasons)

1. The Python fitter path is a fitter flag (`--theme-resid theme-resid-v1`), not a new `--composition` id. The rule
   keeps the parent's composition (ew-theme-std-v1, ew-theme-std-aim-v1, or a standardised ic-shrink-v1), so "the
   parent's theme shares, member weights unchanged" holds whatever R-1/R-3/R-10 left accepted. It also adds no row to
   `COMPOSITIONS`, so it cannot conflict with COMB2's rule-table line. Registration in the fitter is four one-line
   edits (import, one `require`, one `apply`, one `add_argument`).
2. The regressors are the preceding standardised composites, as the brief words it, not the preceding re-standardised
   residuals. With an intercept both span the same space before the re-rank; after the re-rank they differ.
3. The registered order is a frozen constant, not read from the registry at run time.

## 5. Cross-lane edits

- `atx-impl/tools/fit_composition_weights.py`: +4 lines (import after `import composition_rules`; one `require` in
  `fit` after the `--recipe` check; `composition_resid.apply(...)` after `attach_std`; `add_argument` before
  `return p.parse_args`).
- `scripts/tests/test_research_spec.py` (A2's): `NULL_PINS["r11.json"]`, `EXPECTED_CHANGES["r11.json"]`,
  `FIT_APPENDED` (one line each); the fit-flag assertion appends `FIT_APPENDED`; new test
  `test_r11_appends_theme_resid_to_the_parents_fit`. COMB2's r10.json needs the same two dict lines.
- `atx-impl/tests/strategy_ic_runner_test.cpp`: helper `resid_block` and two tests, inserted after
  `ThemeStandardiseRefusalsPrecedeAnyPayloadOrOutput` (not at the file end).
- `atx-impl/CMakeLists.txt` (+3 lines), `atx-impl/tests/CMakeLists.txt` (+2 lines).
- `strategy_ic_{composition.hpp,composition.cpp,detail.hpp,admission.cpp,runner.cpp}`: the wiring in section 2.

## 6. Open risks

- C++ never compiled (lane rule). Written against /W4 /WX, idioms copied from `group_rerank.hpp` and
  `strategy_ic_composition.cpp`.
- R-11 is undefined if R-1 is rejected (no standardised composite; the fitter refuses). It needs a ruling like E-37:
  slot 50 unused, or a definition on ew-theme-v1, which would also add standardisation (two changes in one cell).
- COMB2 (`ic-shrink-v1`): R-11 composes with it only if its weights file carries `theme_standardise` with rerank
  true. Otherwise the fitter refuses `--theme-resid`. Check at integration 6.
- Marginal verb (K6) is unchanged. With a theme-resid parent, `--themes` regresses on the ew-theme-std-v1 theme terms
  (not residualised). The `book_composite` regressor is the actual blend. This matters for R-12's screen only if R-11
  is accepted.
- Cost: `finish()` residualises serially. Per session the work is the sum over t of t^2 n, twice. At T 10, n 5,600
  and 1,155 sessions that is about 10 GFlop at /O2: seconds to tens of seconds per role. Memory: +N(8T + 8) B per role
  (about 0.5 MB).
- The registered order is the v4 list order. It was not chosen for the hypothesis: value keeps its full composite and
  later themes keep only what the earlier ones do not span.
