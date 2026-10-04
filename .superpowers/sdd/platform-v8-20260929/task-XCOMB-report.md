# Task XCOMB report: combination and capacity rules (`theme-erc-v1`, `inv-vol-v1`)

Lane XCOMB of expansion X (alpha generation). Worktree `C:/atx-wt/pool-13`, branch `feat/platform-v8-xcomb-20261002`,
base `3c6ae225`. Brief: `task-X-briefs.md` (Rules for every X lane, Lane XCOMB), `lane-rules.md`.

Commits:
- `30b719e0` feat(composition): theme-erc-v1 (rule 1, combination).
- `068b4a4d` feat(nav): inv-vol-v1 (rule 2, capacity).
- this report.

The lane is blind. Nothing was built (the C++ is written to compile first time under `/W4 /WX`, copying the owning
files' idiom; root builds). No real data was run. The Python tests ran on synthetic data only. No return, IC or Sharpe
output was opened beyond status 6 and the ledger.

## 1. Rules registered

Every constant below was fixed before any cell read. Each rule is one hypothesis and one trial.

| rule id | hypothesis | registered constants | judging criterion |
|---|---|---|---|
| `theme-erc-v1` (combination, fit) | Theme shares that give every theme sleeve an equal share of the blend's risk combine better than the parent's equal theme shares 1/T. ERC uses second moments only, with no mean and no IC, and those moments are estimated far more precisely than means. | Members and within-theme shares a_k are the parent's.<br>The sleeves r_t = sum_k a_k s_k f_k on the parent fit's TRAIN decisions; C = their sample covariance (n - 1), themes in sorted order.<br>ERC by cyclical coordinate descent, exactly **10000** sweeps from the inverse-volatility point; refused unless max_t abs(c_t / mean c - 1) <= **1e-10**.<br>w_k = a_k b_theme(k), then the member cap **1/(2T)**.<br>Runner check to **1e-12**; shares sum to 1 within 1e-12. | Paired S2 net dSR > 0 against the parent (pre-registration rule 5), AND mechanics, AND turnover per unit gross not higher than the parent's. Turnover per unit gross = tau_gmv_mean / mean_gross_leverage_all_rows, S2, executed. |
| `inv-vol-v1` (capacity, NAV) | Sizing each name's desired weight inversely to its execution volatility (h proportional to score / sigma, Grinold and Kahn) moves dollars toward the names whose square-root impact per traded dollar (proportional to sigma) is lowest. The cost per traded dollar then falls and net Sharpe at 4x NAV rises. | r_i becomes r_i x median / max(s_i, **.25** x median) **before** the demean.<br>s_i = the sample SD that the S2 cost model reads for the decision's fills: session **d + 1**, the NAV liquidity window (63, at least 20 pairs), rows <= d.<br>median = the median over the members with a finite s_i > 0 (the middle one, or the mean of the two middle ones).<br>A member without s_i takes the median (multiplier 1). With no s_i at all, the ranks are left as they are. | Paired S2 net dSR > 0 against the parent, AND mechanics, AND net Sharpe at 4x NAV higher than the parent's (`capacity_curve.csv`, the x4 book), AND S2 `cost_bps_traded` lower than the parent's. `cost_bps_traded` (nav_summ) = 1e4 x sum trade_cost_dollars / sum traded_dollars over executed sessions. |
| rule 3 | Not used (section 6). | none | none |

PM6-6 gross matching applies to both cells:
1. A calibration run at the parent's L. This is mechanics only: the all-rows S2 gross G_cal, with no return read.
2. The trial at L = L_parent x G_parent / G_cal, to 4 decimals, set by root as `nav.leverage` in a `-gm` copy.

Both templates say this.

## 2. What was built

### 2.1 `theme-erc-v1` (commit `30b719e0`)

**Engine kernel `atx-engine/include/atx/engine/combine/group_erc.hpp`** (header-only, as `group_shrink.hpp`):
- `group_erc_shares(covariance, groups, sweeps) -> {share, contribution, dispersion}`.
- It solves Spinu's problem by cyclical coordinate descent. The positive root is written without cancellation. There is no early exit, so the sequence is a fixed function of the inputs and a port reproduces it bit for bit.
- It refuses:
  - bad shapes;
  - non-finite, asymmetric or non-positive-diagonal input;
  - an indefinite covariance (non-positive iterates or contributions).

**Runner rule `atx-impl/src/strategy_ic_theme_erc.{hpp,cpp}`:**
- Constants: `theme_erc_rule`, `theme_erc_sweeps` 10000, `theme_erc_dispersion` 1e-10, and the tolerances.
- `theme_erc_weights(share, theme, themes, covariance)` takes 1..256 members and 1..32 themes. It checks that the within-theme shares sum to 1, runs the kernel, applies the dispersion test, sets w = a x b, then applies `cap_across_groups` 1/(2T).

**Rule table (`strategy_ic_admission.cpp`).** `standardise_rules` gains the row `{theme-erc-v1, rerank on, verify_theme_erc}`. `verify_theme_erc`:
1. Reads `theme_standardise.theme_erc {sweeps, dispersion, members {id: {theme, share}}, covariance {themes, matrix}}`.
2. Re-applies the rule.
3. Refuses any weight off by more than 1e-12, any weighted non-member, and any `themes` entry that is not the member's theme.

Every refusal is named and comes before any payload. R6B-C-5 is kept: `provenance.rule` must map to the block's row.

**Fitter (`atx-impl/tools/composition_theme_erc.py`, wired in `fit_composition_weights.py`):**
- `--theme-erc theme-erc-v1` runs on the parent's fit argv.
- Parents are `ew-theme-std-v1` or `ic-shrink-v1`. The flag is refused with `--theme-resid` and under `--era`.
- Within-theme shares come from the parent's `weights_before_cap`. Sleeves come from the fitter's signed, zero-filled factor series on the TRAIN mask.
- The Python arithmetic follows the C++ operation order.
- The weights file carries the runner block and `provenance.theme_erc`. The fit summary carries `theme_erc {parent_rule, theme_shares}`.

**Shared fixture `atx-impl/tests/fixtures/theme_erc_v1.json`:**
- Exact fractions: a 3-theme covariance, theme shares 1/6, 1/2, 1/3, and 9 members.
- One member is capped (value_a, 1/5 to 1/6).
- Python max error 2.8e-17.

**Template `scripts/specs/v8/x-theme-erc.json`:**
- Nominal parent `r1-comp-v8.json`, parent null.
- The downstream outputs are renamed and `fit.flags` gets `--theme-erc theme-erc-v1`.
- The w pass inherits Ruling E-28's 3,072 MiB from an R-1 chain.

**Tests:**
- `GroupErc.*` (5).
- `ThemeErcV1.*` (5): constants, shared fixture fractions, the written rule, and wrong rules told apart (equal 1/T, inverse vol, no cap, the matrix read in first-appearance order).
- `CompositionV8.ThemeErcRunsTheStandardisationUnchangedAndRecordsItsRule` and `CompositionV8.ThemeErcRefusalsPrecedeAnyPayloadOrOutput`.
- Python `test_composition_theme_erc.py` (15). Of these, `test_shared_fixture_fractions` is the Python-equals-C++ check against the same fixture `ThemeErcV1.SharedFixtureFractions` reads, and `test_flag_absent_paths_never_touch_the_rule` is the flag-absent identity.
- Spec test `test_x_theme_erc_appends_its_flag_to_the_parents_fit`.

### 2.2 `inv-vol-v1` (commit `068b4a4d`)

**Engine kernel `atx-engine/include/atx/engine/book/inverse_vol.hpp`** (header-only):
- `scale_inverse_vol(values, sigma, eligible, floor_fraction, scratch) -> {scaled, filled, floored, median, max_multiplier}`.
- Equal volatilities give a multiplier of exactly 1, so they are the identity bit for bit. No usable volatility leaves the row untouched (median NaN).
- Ineligible names are never read or written. Bad input is refused before anything changes.

**Rule wiring.** NAV construction options have no separate table: an option is registered as a `TargetReplayConfig` field, its validation, its rule-id suffix and its recipe/summary keys, as for R-4 and R-5.
- `TargetReplayConfig::inv_vol`, `inv_vol_on()`, and the registered `inv_vol_floor_fraction = 0.25`.
- `validate_config`: aim-partial-v5 only, and not with `hold_band`. Both act between the ranks and the demean, so combining them would be a new rule.
- `form_desired`, with the rule on: `member_ranks`, then the kernel on `DesiredState::sigma`, then the unchanged `demean_gross_one`, locate zeroing, neutralization and ADV cap.
- `replay_targets` refuses the rule: it is a NAV replay option.
- Rule id `+inv-vol-v1`.
- Recipe keys `vol_scale`, `vol_scale_floor_fraction` and `vol_scale_rule` (the declaration).
- Summary block `construction.vol_scale`: scaled decisions, decisions without volatility, scaled/filled/floored totals and shares, the median volatility's spread, and the largest multiplier.
- `ConstructionDay` gets five counters. There is no new CSV column.
- `DesiredState` gets `sigma` and `sigma_sorted`, charged in `desired_state_bytes`.

**NAV replay (`strategy_nav_replay.cpp`):**
- `form_desired_target` fills `sigma[i] = window_liquidity(x, x.volume, cfg, d + 1, i).sigma` for members. These are the S2 cost model's own volatilities for the fills (NaN where the scenario fallback applies).
- `same_shared` requires equal `inv_vol`.
- The CLI takes `--vol-scale inv-vol-v1`. Any other value is a usage error.
- The help text gains one fragment.

**v7 hook (`strategy_nav_v7.cpp`):**
- `--vol-scale` passes through to the replay, so spo-v3's aim and every `--capacity-curve` book are shaped too.
- spo-v1 and spo-v2 refuse it, as they refuse `--hold-band` and `--adv-hold-q`.

**Template `scripts/specs/v8/x-inv-vol.json`:**
- Nominal parent `base-b0c.json`, parent null.
- `nav.output` is renamed and `nav.flags` gets `--vol-scale inv-vol-v1`.
- `--capacity-curve` is the parent's.

**Tests:**
- `BookInverseVol.*` (5): odd and even dyadic closed forms, wrong rules told apart (mean versus median, lower middle, no floor), the identity, and refusals.
- `InvVol.DesiredIsTheScaledRankOfTheFillSessionVolatility` checks `nav_decide` against an independent recomputation from the definition. It uses the sigma `nav_decide` reports for row d + 1, with one name floored (multiplier 4) and one filled. It also checks a row with no history, which must equal the target without the rule bit for bit.
- `InvVol.EqualVolatilitiesReplayTheBookBitForBit`. Prices are power-of-two multiples of one path, so the returns are bit-identical. The rule's replay then equals the replay without it in every NAV, fill, cost and plan field.
- `InvVol.NavRunRecordsTheRuleInBothPassesAndDecideRefusesIt` checks:
  - the recipe and summary keys in the main and capacity passes;
  - the flag-absent recipe has none of them;
  - the daily CSV headers are equal;
  - decide refuses the run as a recipe pin mismatch, and the control verifies;
  - the help text.
- `InvVol.RefusedOutsideItsDomain`:
  - the target replay refuses the rule;
  - baseline-v1 and the hold band are refused in both replays, decide and the CLI;
  - `form_desired` refuses it without the sigma row;
  - spo-v1 refuses the flag;
  - controls run.
- Spec test `test_x_inv_vol_appends_its_flag_to_the_parents_nav`. `check_registered_change` also covers `x-inv-vol.json`: its `nav_delta`, and `--capacity-curve` must be present.
- There is no Python path (a NAV construction option, like R-4 and R-5), so there is no Python-equals-C++ test. The dyadic closed form plus the `nav_decide` recomputation stand in.

## 3. How root verifies

Build and gtests (never compiled here):

```powershell
powershell scripts\atx-build.ps1 configure
powershell scripts\atx-build.ps1 check atx-impl\src\strategy_ic_theme_erc.cpp
powershell scripts\atx-build.ps1 check atx-impl\src\strategy_target_replay.cpp
powershell scripts\atx-build.ps1 build atx-impl-strategy-ic-tests
build\bin\atx-impl-strategy-ic-tests.exe --gtest_filter=GroupErc.*:ThemeErcV1.*:CompositionV8.*:StrategyIcComposition.*:StrategyIcRunner.*:MarginalIc.*:IcShrinkV1.*:GroupShrink.*:GroupCap.*
powershell scripts\atx-build.ps1 build atx-impl-strategy-target-tests
build\bin\atx-impl-strategy-target-tests.exe --gtest_filter=InvVol.*:BookInverseVol.*:AdvHold.*:HoldBand.*:BookTargetShaping.*:StrategyLive.*:StrategyTargetReplay.*:TargetReplayV5.*:TargetReplayV6.*:StrategyNavReplay.*:NavWarmStart.*:ConstructionGrid.*:NavBookWorkers.*:SpoV3.*:RiskTarget.*
powershell scripts\atx-build.ps1 build atx-engine-combine-tests
powershell scripts\atx-build.ps1 build atx-engine-book-tests
powershell scripts\atx-build.ps1 build atx-equity-strategy-ic
powershell scripts\atx-build.ps1 build atx-equity-strategy-targets
```

The engine group targets glob the new test files after the configure. The `--help` texts changed, so any test that pins the full help text would need updating; none was found.

Python (ran here, synthetic):

```
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_composition_theme_erc.py atx-impl/tools/test_composition_ic_shrink.py atx-impl/tools/test_composition_rules.py atx-impl/tools/test_composition_resid.py atx-impl/tools/test_fit_composition_weights.py atx-impl/tools/test_fit_composition_weights_pool.py atx-impl/tools/test_fit_composition_weights_store.py
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests
```

Results:
- Fitter suites: 209 passed, 15 of them new.
- `scripts/tests`: 177 passed, 19 failed, 4 skipped.
  - All 19 failures are the pre-existing `test_research_spec.py` cascade. `r3-aim-gain-gm.json` is missing from `NULL_PINS`; adding it is the integrator's PM7-4 job.
  - At base `3c6ae225` the same cascade is 17 failed / 34 passed. The two new templates join it through `test_every_v8_spec_loads_and_plans`.
  - With a local, uncommitted `NULL_PINS["r3-aim-gain-gm.json"] = CHILD_NULLS`, the file gives 57 passed. I added that line, ran the file, and removed it before committing.

Identity, flag absent:

**theme-erc-v1**
- Code level: `test_flag_absent_paths_never_touch_the_rule` (fits without the flag are byte-identical with the module's functions made to fail). The existing `CompositionV8` identity tests are unchanged.
- Run level, after the build: re-run the parent's weighted IC (w) pass with the new `atx-equity-strategy-ic`. Use the exact argv of its `receipt.json` `command`; only `--output` is new (e.g. `build-equity/mega-v8x-identity-w`). These must be byte-identical: `recipe.json`, `train_combined.{f64,json}`, `train_combined_member.u8`, `train_combined_finite.u8`, `train_planned_targets.csv` and `train_daily_ic.csv`.
- Re-run the parent's fit argv (`fit_composition_weights.py ...` without `--theme-erc`). It must give the same `composition_weights.json` and `admission.json` except `script_sha256`, because the fitter file changed.

**inv-vol-v1**
- Code level: with the flag off, `form_desired` takes the old `desired_target` branch, and no key, column or rule-id suffix is written. `InvVol.NavRunRecordsTheRuleInBothPassesAndDecideRefusesIt` asserts the off recipe has no `vol_scale*` key and that the daily headers are equal. `InvVol.EqualVolatilitiesReplayTheBookBitForBit` shows the on-path is exact when every multiplier is 1.
- Run level: re-run the parent's NAV argv with the new `atx-equity-strategy-targets`. Use the exact `command` of its nav phase; only `--output` is new (e.g. `build-equity/mega-nav-v8x-identity`). Every `daily_*.csv`, `events_*.csv`, `recipe.json` and `summary.json`, and the capacity pass's files, must be byte-identical to the parent's. This is the R-4 identity comparison.

Cells: root sets `"parent"` in `x-theme-erc.json` or `x-inv-vol.json` to the book's spec at V8-F. For theme-erc that spec's fit must carry `--composition ew-theme-std-v1` or `ic-shrink-v1` and no `--theme-resid`. Then:
1. `research_cycle.py lock ... --write`.
2. The PM6-6 calibration run, then the `-gm` copy with `nav.leverage`.
3. `plan` and `run`.

## 4. Deviations

1. The rule-table registration is not literal for inv-vol-v1. NAV construction options have no rule table; the option is registered the way R-4 and R-5 are (section 2.2).
2. inv-vol-v1 has no Python fitter path. Therefore it has no Python-equals-C++ test (section 2.2).
3. Decide is out of scope for inv-vol-v1, following the precedent of R-8. `strategy_live.cpp` has no `nav.vol_scale` deploy key, so a deploy manifest pinned to an inv-vol run is refused as a recipe pin mismatch (tested). The construction itself already runs inside `nav_decide`. Making the rule deployable needs only the deploy key and a `decision.json` record.
4. theme-erc-v1 is defined on ew-theme-std-v1 and ic-shrink-v1 parents only. The aim variants need R-3, which was not accepted. It is undefined on an R-11 (`--theme-resid`) parent: the fitter refuses that combination, and root rules on it.
5. Rule 3 is not used (section 6).

## 5. Cross-lane edits

**Rule 1:**
- `atx-impl/src/strategy_ic_admission.cpp`: the rule-table row, `verify_theme_erc`, and the extended `standardise_block` message, whose old text is kept as its prefix.
- `atx-impl/src/strategy_ic_runner.cpp`: two help lines.
- `atx-impl/tools/fit_composition_weights.py`: the import, the flag, `check_args`, the `fit_prior` branch, attach and the summary.
- `atx-impl/tests/strategy_ic_runner_test.cpp`: two appended `CompositionV8` tests.
- `atx-impl/CMakeLists.txt`: the source, plus the two Debug /O2 + `SKIP_PRECOMPILE_HEADERS` lists.
- `atx-impl/tests/CMakeLists.txt`: the ic tests.
- `scripts/tests/test_research_spec.py`.

**Rule 2:**
- `atx-impl/src/strategy_target_replay.{hpp,cpp}` and `strategy_target_replay_detail.hpp`.
- `atx-impl/src/strategy_nav_replay.cpp`: `form_desired_target`, `same_shared`, the CLI, the help and a comment.
- `atx-impl/src/strategy_nav_replay.hpp`: the grid contract comment.
- `atx-impl/src/strategy_nav_v7.cpp`: the spo-v1/v2 refusal list and help.
- `atx-impl/tests/strategy_live_test.cpp`: the appended `InvVol` tests.
- `atx-impl/tests/CMakeLists.txt`: the target tests.
- `scripts/tests/test_research_spec.py`: `NULL_PINS`, `EXPECTED_CHANGES`, `nav_delta`, the E-29 capacity tuple, and a new test.

**New files:**
- `atx-engine/include/atx/engine/combine/group_erc.hpp`, `atx-engine/include/atx/engine/book/inverse_vol.hpp`.
- `atx-engine/tests/combine/combine_group_erc_test.cpp`, `atx-engine/tests/book/book_inverse_vol_test.cpp`.
- `atx-impl/src/strategy_ic_theme_erc.{hpp,cpp}`.
- `atx-impl/tests/strategy_ic_theme_erc_test.cpp`, `atx-impl/tests/fixtures/theme_erc_v1.json`.
- `atx-impl/tools/composition_theme_erc.py`, `atx-impl/tools/test_composition_theme_erc.py`.
- `scripts/specs/v8/x-theme-erc.json`, `scripts/specs/v8/x-inv-vol.json`.

I did not touch `atx-db/`, the PM root, `pool-10`, the warehouse or any broker.

## 6. Rule 3 not used

Every registered rule is one more trial in X's deflated Sharpe count. I left the slot open rather than spend it on a weak prior. The candidates I considered each fell short:
- **A variant of a rejected or in-measurement rule:**
  - a slower aim or an aim gain (R-3);
  - a trade or no-trade band (R-4);
  - a liquidity cap on holdings (R-5);
  - a risk-model tracking or optimizer rule (R-6).
- **Already measured:** industry demeaning (v6 C5).
- **Too much new per-date plumbing outside this lane's files:** volatility-managed theme sleeves, theme-level factor momentum, and a signal half-life by theme speed (the latter needs a per-theme horizon estimate this lane cannot register blind).

The PM may assign the slot.

## 7. Open risks

1. **Nothing was compiled.** First-compile risk is in:
   - the two new header-only kernels;
   - the `std::array<StandardiseRule,4>` table;
   - the new `ConstructionDay` and `DesiredState` fields;
   - the nested JSON initialisers in `inv_vol_summary`;
   - the appended tests, which use only helpers already in their files.

   The local idiom was copied (ATX_TRY with `const auto`, `Result` `->`, named test namespaces in the engine groups).
2. **theme-erc: linear sleeves.** The covariance is that of linear sleeves of the members' factor books. The runner blends per-date re-ranked theme composites, which are not the same object, so the realised risk shares may differ from equal. The v8x cell measures the outcome, not the risk shares.
3. **theme-erc: in-sample covariance.** The covariance is estimated on the same TRAIN decisions the cell is judged on. It uses second moments only (no mean or IC), but it is still in-sample.
4. **inv-vol: possible absorption by neutralization.** `price-risk-v1` / `ind-v2` project out the book's linear vol exposure after the scaling. The gross reallocation toward low-volatility names survives, but part of the tilt may be absorbed. `construction.vol_scale` reports the multipliers, not the absorbed part.
5. **inv-vol: risk falls at matched gross.** PM6-6 matches gross, not risk, and the rule lowers book risk at a given gross. The net Sharpe comparisons are scale-free, but the 4x NAV book also trades less impact per dollar, so the dSR and capacity gains share one cause.
6. **inv-vol: workspace charge.** `ConstructionDay` grows by 40 bytes, which raises the NAV workspace charge per day and book by that amount. Flag-absent outputs are unchanged, but a run sized at the exact edge of `--max-bytes` could now be refused.
7. **Spec test cascade.** The pre-existing `r3-aim-gain-gm.json` spec failure (PM7-4) also fails `test_every_v8_spec_loads_and_plans` for both new templates until the integrator adds that pin.
