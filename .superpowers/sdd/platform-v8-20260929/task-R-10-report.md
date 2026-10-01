# Task R-10 report: composition rule `ic-shrink-v1` (lane COMB2, cell slot 49 of Ruling E-38)

Worktree `C:/atx-wt/pool-11`, branch `feat/platform-v8-comb2-20260930` from `fd2ff7a8`. Nothing built (lane rule), no
real data opened; Python tests ran on synthetic data only. No read of 2020-2023 or later.

| commit | content |
|---|---|
| `9dbabf54` | engine kernel `atx/engine/combine/group_shrink.hpp` + `combine_group_shrink_test.cpp` |
| `b8a86b1b` | C++ rule `strategy_ic_shrink.{hpp,cpp}`, theme_standardise rule table, runner records, gtests, shared fixture |
| `d0813fc5` | Python fitter path `composition_ic_shrink.py`, `--composition ic-shrink-v1`, tests |
| `42614cc2` | cell template `scripts/specs/v8/r10.json`, spec tests |
| `6b6145f3` | test fix (feasible admitted case), explicit `u8` cast in the kernel |

## 1. Registration (fixed blind by the lane, declared before any cell read; E-30 precedent)

Hypothesis: inside each theme, member weights proportional to a shrunk estimate of each member's IC combine better
than the parent's tier weights (ew-theme-std-v1).

1. **IC estimate** `ic_k` = the fitter's admission row `train_mean` of member k: the mean of `s_k * f_k` over the live
   decisions of the fit window the parent fitter already uses (TRAIN; under `--era` the pooled era decisions), `s_k`
   the prior sign (+1), `f_k` the factor return of the member's neutralised gross-1 centred-rank book (a
   dispersion-scaled residual rank IC, 1-day label). No new window, no new read. Members: the admitted non-degenerate
   members (the parent rule's member set).
2. **Shrinkage**, James-Stein toward the theme's equal weight, fixed **intensity 0.5**:
   `shrunk_k = 0.5 * mean_{theme} ic + 0.5 * ic_k`.
3. **Floor 0**: `p_k = shrunk_k` if `> 0`, else 0.
4. **Within-theme share** `a_k = p_k / sum_theme p`; a theme with no positive `p` takes the shrinkage target, the
   equal share `1 / n_theme` (theme kept at 1/T). Equivalently, with no floored member and a positive theme mean,
   `a_k = 0.5 / n + 0.5 * ic_k / sum ic`.
5. **Theme share 1/T** (T = themes with >= 1 member): `w_k = a_k / T`; then the **member cap 1/(2T)** of
   ew-theme-std-v1 rule 4 (excess pro rata to the other themes' uncapped members, repeated to a fixed point; cap
   tolerance 1e-12 relative, as `composition_rules.CAP_TOLERANCE`). Infeasible cap: refused.
6. **Standardisation unchanged**: the IC runner applies ew-theme-std-v1's per-date re-rank. The runner re-applies the
   rule to the recorded inputs and refuses any weight off by more than **1e-12 absolute**.
7. **Parent**: defined on an ew-theme-std-v1 parent only (R-1 accepted). The template maps only that composition; an
   ew-theme-v1 parent (no standardisation to keep) or an ew-theme-std-aim-v1 parent (R-3's gains would be dropped) is
   refused at load: root rules before any run.
8. **Acceptance** (prereg rule 5): paired S2 net dSR > 0 against the parent AND mechanics AND planned turnover per unit
   gross not higher than the parent's (the composition-cell criterion of R-1, plan 12.1; the brief named none).

## 2. What was built

- `atx-engine/include/atx/engine/combine/group_shrink.hpp` (header-only, generic): `GroupShrinkShares
  {shrunk, share, equal}`; `group_shrink_shares(estimate, group, groups, intensity, minimum)` (steps 2-4);
  `cap_across_groups(weights, group, groups, cap, tolerance) -> Result<usize passes>` (step 5's cap). Sums in member
  order, one expression per step, so the Python port reproduces it.
- `atx-impl/src/strategy_ic_shrink.{hpp,cpp}` (the rule, beside `strategy_ic_composition.cpp`): `ic_shrink_rule =
  "ic-shrink-v1"`, `ic_shrink_intensity = 0.5`, `ic_shrink_floor = 0.0`, `ic_shrink_cap_tolerance = 1e-12`,
  `ic_shrink_weight_tolerance = 1e-12`; `IcShrinkFit {shrunk, share, weights, equal_theme, cap, cap_passes}`;
  `ic_shrink_weights(ic, theme, themes)` (1..256 members, 1..32 themes). Added to `atx-impl-core` and to the IC TUs'
  `/O2` + no-PCH flag set.
- **Rule table** (`strategy_ic_admission.cpp`): `StandardiseRule {id, rerank_off, verify}`, rows
  `{ew-theme-std-v1, true, null}` and `{ic-shrink-v1, false, verify_ic_shrink}`. `standardise_block` accepts a row
  (ic-shrink-v1 needs rerank true; the old refusal text is kept as the prefix of the new one);
  `composition_standardise` runs the row's verify after `theme_indices`, before any payload, and records the row id.
  Block of the rule: `theme_standardise {rule: "ic-shrink-v1", rerank: true, themes: {weighted id: theme},
  ic_shrink: {intensity: 0.5, floor: 0.0, members: {id: {theme, ic}}}}` (members include a floored member at weight
  0). Verify: constants equal, members known with valid theme and finite ic, rule on the members in library order,
  every weight within 1e-12 of it (0 for non-members), every weighted candidate a member with its member theme.
- Runner records: `method_recipe(..., std::string_view standardised)`, `PinnedWeights::standardise_rule()`,
  `score_role(..., std_rule = theme_standardise_rule)`, `save_combined_artifact(..., std::string_view)`: recipe
  `composition_standardise`, combined manifest `composition_standardise` and summary
  `composition_weights.standardise` name the rule (`"ic-shrink-v1"`); for ew-theme-std-v1 the strings, hence the
  bytes, are unchanged. Help text gains two lines. The marginal verb reads the block unchanged (`ic_weights_themes`).
- `atx-impl/tools/composition_ic_shrink.py`: `RULE_ID`, `INTENSITY`, `FLOOR`, `RUNNER_TOLERANCE`, `MAX_THEMES`,
  `shrunk_shares`, `ic_shrink(ids, themes, ics, error)` -> `composition_rules.StdFit` (cap via
  `composition_rules.member_cap`), `attach(document, fit)` (schema v2, block, `provenance.ic_shrink`: constants, member
  ICs, theme means, shrunk ICs, shares, floored members, equal-share themes, cap passes, module SHA-256).
  `fit_composition_weights.py`: import, registration in `PRIOR_COMPOSITIONS`/`COMPOSITIONS`, one `elif` in `fit_prior`
  (ICs = `rows[k]["train_mean"]` of the active members), one attach line.
- `scripts/specs/v8/r10.json`: template, `parent` null (root sets the last accepted cell), `nominal_parent`
  `r1-comp-v8.json`, fit `--composition {"ew-theme-std-v1": "ic-shrink-v1"}`, renames fit, card, w, nav, monitor
  outputs; inherits E-28's 3,072 MiB w pass from R-1.
- Shared fixture `atx-impl/tests/fixtures/ic_shrink_v1.json`: 12 members, 4 themes, exact fractions (one floored
  member, one equal-share theme, one capped member, non-equal shares in every other theme).

## 3. How root verifies

Build and gtests (never compiled here):

```powershell
powershell scripts\atx-build.ps1 build atx-impl-strategy-ic-tests
build\bin\atx-impl-strategy-ic-tests.exe --gtest_filter=IcShrinkV1.*:GroupShrink.*:GroupCap.*:CompositionV8.*:StrategyIcComposition.*:StrategyIcRunner.*:MarginalIc.*
powershell scripts\atx-build.ps1 build atx-engine-combine-tests
build\bin\atx-engine-combine-tests.exe --gtest_filter=GroupShrink.*:GroupCap.*:GroupRerank.*
powershell scripts\atx-build.ps1 build atx-equity-strategy-ic
```

New gtests: `GroupShrink.{SharesShrinkTowardTheGroupMeanFloorAndNormalise, IntensityRunsFromProportionalToEqualShares,
PositiveMinimumKeepsEveryMember, RefusesBadShapesAndValues}`, `GroupCap.{ExcessGoesProRataToTheOtherGroups,
RepeatsToAFixedPoint, ToleranceInfeasibleCapsAndRefusals}`, `IcShrinkV1.{RegisteredConstants,
WeightsFollowTheWrittenRule, SharedFixtureFractions, FixtureTellsWrongRulesApart, RefusesBadInputsAndAnInfeasibleCap}`,
`CompositionV8.{IcShrinkRunsTheStandardisationUnchangedAndRecordsItsRule, IcShrinkRefusalsPrecedeAnyPayloadOrOutput}`.
Every existing `CompositionV8` / `StrategyIcRunner` / `MarginalIc` test must pass unchanged (refusal texts kept).

Python (ran here):

```
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_composition_ic_shrink.py atx-impl/tools/test_composition_rules.py atx-impl/tools/test_fit_composition_weights.py atx-impl/tools/test_fit_composition_weights_pool.py atx-impl/tools/test_fit_composition_weights_store.py
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests
```

Results: 137 passed (fitter suites, 9 new); scripts/tests 165 passed, 4 skipped.

Python equals C++: both `IcShrinkV1.SharedFixtureFractions` and `test_shared_fixture_fractions` assert the fixture's
exact fractions to 1e-15 (same file).

Identity (flag absent):
- Code level: `CompositionV8.IcShrinkRunsTheStandardisationUnchangedAndRecordsItsRule` (the same weights under
  ew-theme-std-v1 and ic-shrink-v1 give byte-identical combined `.f64`, member/finite masks, planned targets and IC
  rows; manifests and recipes equal except the rule name and the weights/recipe pins); the existing
  `CompositionV8.IdentityWithReRankAndCapOffIsEwThemeV1` pins the ew-theme-std-v1 recipe/manifest/summary strings;
  `test_flag_absent_paths_never_touch_the_rule` (ew-theme-std-v1 and ew-theme-v1 fits byte-identical with the new
  module's functions made to fail).
- Run level, after the build: re-run any landed weighted IC pass with the new `atx-equity-strategy-ic` (exact argv of
  its `receipt.json` `command`, only `--output` new, e.g. `build-equity/mega-v8-r10-identity-w`); expected
  byte-identical `recipe.json`, `train_combined.{f64,json}`, `train_combined_member.u8`, `train_combined_finite.u8`,
  `train_planned_targets.csv`, `train_daily_ic.csv`. Use the R-1 w pass when it exists (ew-theme-std-v1 block), else
  the v7.1 weighted pass (`mega-v71w-train-ew-run1/receipt.json`).
- Fitter: the parent's fit argv on the new script gives the same `composition_weights.json` and `admission.json` except
  `script_sha256` (the file changed); C-1 store records are keyed by producer fingerprints, which this lane did not
  touch, so `--reuse` hits are unchanged.

Cell: set `"parent"` in `r10.json` to the last accepted cell's spec (it must carry `--composition ew-theme-std-v1`),
`research_cycle.py lock scripts/specs/v8/r10.json --write`, `plan`, `run`.

## 4. Deviations

1. The brief's "rule table" did not exist in C++ (one hard-coded theme_standardise rule); it is now the
   `standardise_rules` table in `strategy_ic_admission.cpp`. ic-shrink-v1's C++ role is verification: the runner
   re-applies the rule to the block's recorded ICs (the weights file stays the one source of the weights).
2. The generic arithmetic sits in an atx-engine header (lane rule: generic machinery in atx-engine); the rule and its
   constants in atx-impl.
3. Template file name `r10.json` as the brief says (other templates carry a descriptive suffix); nominal parent is
   R-1, not B0c, because the rule is defined only on an ew-theme-std-v1 parent.
4. Acceptance's mechanical criterion was not in the brief; registered as R-1's (turnover per unit gross not higher).

## 5. Cross-lane edits

- `atx-impl/tools/fit_composition_weights.py`: +1 import, +2 registration lines after R-3's, +4 lines `elif` in
  `fit_prior` before the ew-theme-v1 `else`, +2 attach lines after the STD_RULES attach (FIX-3 and ORTH edit this file).
- `atx-impl/tools/test_fit_composition_weights.py`: `PRIOR_COMPOSITIONS` pin gains `"ic-shrink-v1"` (1 line).
- `scripts/tests/test_research_spec.py`: `NULL_PINS` and `EXPECTED_CHANGES` gain `r10.json`; the template-diff test's
  composition swap is a (parent value, cell value) map; new `test_r10_maps_only_the_r1_composition_and_inherits_its_w_cap`.
- `atx-impl/src/strategy_ic_{admission,runner}.cpp`, `strategy_ic_detail.hpp`, `strategy_ic_runner.hpp` (comment),
  `atx-impl/CMakeLists.txt`, `atx-impl/tests/CMakeLists.txt` (`strategy_ic_shrink_test.cpp` and the engine test join
  `atx-impl-strategy-ic-tests`), `strategy_ic_runner_test.cpp` (tests appended at the end). Lane ORTH
  (`theme-resid-v1`) will touch the same table, `standardise_block`, `method_recipe` and the end of the runner test:
  expect a textual conflict; the table is meant to take its row.

## 6. Open risks

- Never compiled: written for clang-cl `/W4 /WX` (no narrowing, explicit casts, no shadowing, every `Result` consumed).
- Parent: if R-3 is accepted before slot 49, the parent composition is ew-theme-std-aim-v1 and r10 refuses at load;
  root rules (skip, or a gains-on-top variant built and registered before any read).
- The IC estimate is the fitter's 1-day factor-return mean, not the runner's 21-day rank IC (the brief: "the fitter's
  IC estimate ... no new read"). Within a theme its ratios are dispersion-weighted IC ratios.
- Shrinkage at 0.5 with 2-5 members per theme: two-member themes with unequal ICs always bind the cap 1/(2T), so
  part of the within-theme tilt moves to other themes (as under ew-theme-std-v1's tier shares).
- Python and C++ order the cap's theme redistribution differently (names vs first appearance) when two themes spill in
  one pass: last-bit differences only, inside the runner's 1e-12.
- Memory: as ew-theme-std-v1 (one f64 plane per theme); E-28's 3,072 MiB w pass is inherited through R-1.
