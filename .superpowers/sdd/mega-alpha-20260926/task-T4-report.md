# Task T4 report: construction options (price-risk neutralize, no-trade band) + daily GMV turnover

Status: implemented and committed, not compiled or run (lane rule: root builds).
Worktree `C:/atx-wt/pool-5`, branch `feat/mega-alpha-nav-t4-20260927`, on top of the T2 fix `ca4fa80c`
(root base `9003f273`). Commit: `6a38fe39` feat(strategy): neutralize, no-trade band, daily GMV turnover
[mega T4]. Seven files, +1260/-125. No CMake change (all files already registered). Nothing pushed.

## Files
| File | Change |
|---|---|
| `atx-impl/src/strategy_target_replay.hpp` | `TargetNeutralize {None, PriceRiskV1}`; `TargetReplayConfig` gains `neutralize`, `price_risk` (T3 `PriceExposureConfig`, CLI uses its defaults), `neutralize_max_amplification{5}`, `neutralize_max_excluded_share{0.5}`, `band_multiple{0}`; `TargetReplayInput::volume{}` (optional); `NeutralizeOutcome`, `ConstructionDay`; `TargetReplayDay::construction`. Includes `strategy_price_exposures.hpp` (light). |
| `atx-impl/src/strategy_target_replay_detail.hpp` | New seams: `form_desired`, `construction_active`, `construction_rule_id`, `construction_recipe_json`, `construction_summary_json`, `construction_scratch_bytes`, `construction_csv_columns`, `write_construction_csv`, `parse_neutralize`, `neutralize_outcome_label`, `sorted_quantile` (still no nlohmann; JSON travels as text). |
| `atx-impl/src/strategy_target_replay.cpp` | Validation, band in `update_weights`, shared `form_desired`, admission, recipe/CSV/summary construction output, CLI flags, detail wrappers. |
| `atx-impl/src/strategy_nav_replay.hpp` | `NavTurnoverLimits` (+ constants .20/.30), `NavReplayDay::{pretrade_gross_dollars, one_way_turnover_gmv, construction}`, `replay_nav_scenarios`, `NavSummary` daily GMV fields, `summarize_nav(result, limits = {})`, `run_nav_replay(cfg, limits, progress)` overload. |
| `atx-impl/src/strategy_nav_replay.cpp` | Lockstep scenario books, extension point now calls the shared construction, GMV turnover column + stats + ceilings, recipe/summary/CSV additions, CLI flags. |
| `atx-impl/tests/strategy_target_replay_test.cpp` | 4 new fixtures (b, c, d, a/CLI); 7 existing unchanged. |
| `atx-impl/tests/strategy_nav_replay_test.cpp` | 3 new fixtures (a, e, f); `Panel::target()` now also passes `volume` (ignored on the default path); 10 existing unchanged. |

## Key logic (line numbers at 6a38fe39)
- **Config validation** `validate_config` (target cpp :67): neutralize enum, `band_multiple` finite >= 0; when neutralizing, the T3 config contract (`price_risk_valid` :59, mirrors the documented `compute_price_exposures` ranges so a bad recipe is refused once, not at every decision), amplification cap finite > 0, excluded-share cap in [0, 1].
- **Band** `update_weights` (:155). On a rebalance decision `band = band_multiple / N_d`, N_d = members at d; otherwise `band = -1`, which bands nothing (every gap >= 0), so the default arithmetic is the unbanded code path exactly. A banded member (`|desired-current| <= band`) keeps `current`, is counted in `construction.banded_names`, and is left out of `distance` (so v2's fraction is `(budget-spent-forced)/distance_unbanded`). Others move by the rule's fraction. Forced exits unchanged.
- **Neutralization + guard** `form_desired` (:202), the one construction step both replays call.
  - Tied rank (`desired_target`), then `neutralize_price_risk(prices, cfg.price_risk, d, desired, member_d, scratch, stats)`: exposures computed ONCE per decision (M7) from role close/raw/volume/present, reading only sessions <= d.
  - Record: `used`, `excluded`, `excluded_share = excluded_gross/gross`, `amplification = gross/residual_gross` (NaN for a flat target).
  - Outcome, in order: Unavailable with `used < min_names` -> `skipped-too-few-names`; `excluded_share > 0.5` -> `skipped-excluded-share` (this also catches T3 review M2, all gross on excluded rows); other Unavailable -> `skipped-refused`; `amplification > 5` -> `skipped-amplification` (M1); else `applied`.
  - A skip returns `false`: the decision is treated as a non-rebalance (current weights kept, forced exits still applied, v2 spend unchanged by discretionary).
- **Target replay** `replay_targets`: calls `form_desired` on cadence decisions; refuses neutralization without prices+volume. `run_target_replay` (:710) requires `--role` for neutralization and then loads volume (36 B/cell admission). Budget charges the price-risk scratch (`price_risk_scratch_bytes` :91: per name `block*8+128`, plus the market row).
- **NAV extension point** `form_desired_target` (nav cpp :414) forwards to `detail::form_desired`; `plan_decision` (:423) takes the shared desired target and the effective `rebalance`.
- **Lockstep** `run_books` (:492) / `replay_nav_scenarios` (:934). All scenario books step together: each book runs exactly its own MARK -> EXECUTE -> DECIDE -> close sequence on its own state; only the decision's desired target (and its exposures) is formed once and shared, so each book is bit-identical to a single-scenario replay (fixtures assert it). `replay_nav` is the 1-scenario case. `run_nav_replay` runs the three fixed scenarios in one lockstep call: exposures once per decision, not per scenario (M7). NAV volume is authoritative (`x.volume = in.volume`).
- **GMV turnover.**
  - Per session: `pretrade_gross_dollars = sum |held|` after MARK, before EXECUTE (stale names at stale marks). `one_way_turnover_gmv = traded / pretrade_gross` on execution sessions, NaN when the pre-trade gross is 0 (deployment), 0 on non-execution rows. Executed fills only, so forced exits count and planned turnover never does (ruling). Write-offs are not fills.
  - `summarize_nav` (:978): over executed sessions except `deployment_index` with pre-trade gross > 0 -> sessions, mean (session order sum), median/p95 (linear interpolation at (n-1)q, numpy default, `sorted_quantile`), max + its session; zero-GMV sessions counted. `meets_daily_turnover_mean/p95` compare against `limits` (NaN never meets).
- **Admission** `nav_reserve_bytes` (:910): per-book state x3 (lockstep), shared construction per name, the price-risk scratch at max names when neutralizing (about 43 MB with v1 windows), days/events x3 as before. `validate_nav_input` charges `books` x per-book state/days/events + shared + scratch.

## Outputs
- **Rule id**: `<rule>[+neutral-price-risk-v1][+band-<X>]`, with X as the shortest round-trip decimal (e.g. `baseline-target-v1+neutral-price-risk-v1+band-0.5`), in both recipes' `rule`, the NAV summary `rule` and each `construction.rule_id`.
- **Recipes**: construction keys only when non-default: `neutralize`, `desired_target_postprocess: price-risk-v1` (NAV key overwritten), `price_risk{windows, min_pairs, min_names, clip_z, exposures, method}`, `neutralize_guard{max_amplification 5, max_excluded_gross_share .5, amplification, on_skip}`, `band_multiple`, `band`. The target recipe (and its hash) is unchanged with defaults. The NAV recipe always adds `daily_turnover_mean_max`, `daily_turnover_p95_max`, `daily_turnover_definition` and `monthly_turnover_target_status: legacy-reporting-only;retired-2026-09-27` (the brief requires the declared ceilings in recipe.json), so the NAV recipe hash changes once even at defaults.
- **CSVs**: target `daily.csv` byte-identical at defaults; with an option it appends `,rebalance,neutralize,neutralize_used,neutralize_excluded,neutralize_excluded_share,neutralize_amplification,banded_names`. NAV `daily_<S>.csv` keeps all 48 T2 columns first and unchanged, always appends `pretrade_gross_dollars,one_way_turnover_gmv` (`nan` spelled), then the six construction columns when non-default (NAV already has `rebalance`, now effective).
- **Summaries**:
  - NAV per scenario always: `daily_turnover_gmv {definition, basis, sessions, zero_gmv_sessions_excluded, mean, median, p95, max, max_session_ns, quantile, mean_max, p95_max}`, `meets_daily_turnover_mean`, `meets_daily_turnover_p95`.
  - When non-default, both replays add `construction {rule_id, neutralize, band_multiple, decisions, cadence_rebalance_decisions, rebalanced_decisions, neutralize_attempted/applied/skipped_decisions, neutralize_skip_reasons {too-few-names, excluded-share, refused, amplification}, neutralize_used_names_min/median, neutralize_amplification_median/max, neutralize_max_excluded_share, banded_names_total, mean_banded_names_per_rebalance}` (NAV: per scenario, since banding acts on each book's drifted weights).
  - Monthly fields (`months_le_0.30`, `meets_turnover_target_*`) unchanged, and `limitations` now documents them as legacy reporting-only and names the daily GMV ceilings as the limits.
- **CLI**: `--neutralize none|price-risk-v1` and `--band-multiple X` in both modes (targets mode: neutralize requires `--role`); NAV adds `--daily-turnover-mean-max` (.20) and `--daily-turnover-p95-max` (.30). Unknown `--neutralize` -> exit 2; invalid band/ceiling -> run error, exit 1, no output.

## Regression (defaults bit-identical)
- Every pre-existing value is computed by the same arithmetic in the same order:
  - the band-off path is literally the old expressions;
  - `form_desired` with neutralize none is `desired_target` and returns true;
  - lockstep books perform the single-book sequence;
  - GMV fields are read-only additions.
- The NAV daily CSV's first 48 columns, the target CSV/recipe, and every pre-existing NAV summary value are unchanged. Only appended columns/keys (required by the brief) differ, so the root can compare the TRAIN smoke reference on `cut -d, -f1-48` of the daily CSVs and on the pre-existing summary keys. The NAV recipe SHA and the CSV SHAs change because of those appended fields; the limitations string changed as the brief requires.
- Fixtures: all 17 prior fixtures unchanged; new (a) fixtures pin the headers/keys and lockstep == single runs.

## Fixtures (postimplementation)
`strategy_target_replay_test.cpp` (suite `StrategyTargetReplay`):
- (b) `PriceRiskNeutralizedTargetHasZeroExposureAndRecordsAmplification` (:370). On the T3 noisy panel (bit-identical generator, seed 21, 12 names, short windows):
  - At d=60, `detail::form_desired` equals `compute_price_exposures` + `neutralize_target` bit-for-bit, and the amplification bits equal `gross/residual_gross`.
  - The intercept/beta/vol/log-ADV moments on an independently rebuilt clipped-z design are <= 1e-12, and the gross is 1.
  - The replay's day-60 record matches, and gross is ~1 at fraction 1.
  - Decisions 0-19 skip too-few-names (used 0, no position); exactly 20 skips.
- (c) `NeutralizeSkipKeepsWeightsAppliesForcedExitsAndCounts` (:452). Names 0,1 sit out of membership at 40-44 with min_names 11:
  - exactly 25 too-few-names skips;
  - on day 40: forced > 0, discretionary == 0, applied_fraction 0, gross = previous gross minus forced;
  - days 41-44 have turnover 0 and bit-identical gross.
  - Amplification cap 1e-6: no applied decision, total turnover 0, skipped-amplification counted.
- (d) `NoTradeBandKeepsSmallMovesAndLeavesThemOutOfTheBudgetDistance` (:498). Hand-computed on 4 names, band .6/4 = .15:
  - baseline day 0: 2 banded, turnover .75, gross .75;
  - baseline day 1: 2 banded, turnover .625, gross .875, net .125;
  - v2 budget .5: fraction 2/3 (distance .75, banded excluded), turnover .5;
  - unbanded v2: fraction .5;
  - a negative band is refused.
  - Values were checked with a bit-faithful Python model.
- (a/CLI) `ConstructionOptionsAreRecordedOnlyWhenNonDefault` (:536):
  - defaults: exact T2 CSV header, no construction recipe keys, rule `baseline-target-v1`, no summary construction;
  - band 0.5: rule `...+band-0.5`, recipe key, appended columns, summary construction;
  - neutralize without role: InvalidArgument, no output;
  - `--neutralize bogus`: exit 2.

`strategy_nav_replay_test.cpp` (suite `StrategyNavReplay`):
- (a) `DefaultConstructionKeepsT2OutputsAndLockstepMatchesSingleRuns` (:820):
  - on the random gap/nonmember panel, for both rules, the three lockstep books equal three single-scenario replays bit-for-bit (days, construction, GMV fields, events, deployment, participation);
  - the published default run keeps the 48 T2 columns plus exactly the 2 GMV columns, no construction recipe keys, rule `baseline-target-v1`, recipe ceilings .20/.30, legacy monthly field present, daily GMV block + flags present.
- (e) `NeutralizeAndBandRunEndToEndAtCadenceOne` (:873). Noisy panel, cadence 1, fraction 1, neutralize + band 0.5, flat 6 bps + 300 bps borrow:
  - every NAV decision's construction record equals the target replay's (same outcome/used/excluded/amplification bits);
  - effective rebalance == applied, and skipped decisions have zero discretionary and zero banded;
  - no trades before session 21; applied > 0 and banded > 0;
  - lockstep == single for the three fixed scenarios.
  - Pinned `run_nav_replay`: composed rule id, recipe construction keys, per-scenario construction (decisions 68, attempted 68, skipped+applied = 68, too-few-names >= 20, used-min 0) and the construction CSV columns.
  - Target replay on the same artifact neutralizes (rule id + construction).
  - CLI: `--neutralize bogus` exits 2; `--daily-turnover-mean-max 0` exits 1 with no output.
- (f) `DailyGmvTurnoverStatisticsAndCeilingsHandChecked` (:956). NAV 1000, constant prices, no costs, cadence 1, permutations A A B B A C:
  - deployment has GMV 0 (NaN, excluded); sessions 2..6 give {0, .5, 0, .5, 2} at GMV 1000 exactly;
  - mean .6, median .5, p95 1.7, max 2 at session 6;
  - the flags flip at +-1e-9 around the thresholds; defaults fail; a zero ceiling is refused.
  - Forced exit: a present nonmember exited alone is 500 of a 1000 book (.5), counted, planned_forced .5; stats {0, .5, 0}: mean 1/6, p95 .45.

## Decisions to confirm
1. **Only data refusals skip.** The brief says "if neutralization returns an error ... SKIP". I skip on `Unavailable`, the T3 data refusals: too few names, constant exposure, ill-conditioned, spanned. `InvalidArgument`/`OutOfRange` (contract or allocation) abort the replay: they signal a bug, and silently skipping every decision would hide it. The config contract is validated up front.
2. **N_d = members at d** (every member has a desired weight, 0 for member&!ok rows after neutralization). The band is `band_multiple / N_d`, applied only on effective rebalance decisions. A skipped decision has no band.
3. **Skip classification order**: too-few-names, then excluded-share (checked on refusals too, so T3 M2 reports as excluded-share), then refused, then amplification. Flat targets (amplification NaN) are never skipped.
4. **Construction diagnostics** are per scenario in the NAV summary (banding acts on drifted weights). Neutralization outcomes are identical across scenarios.
5. **Price-risk parameters**: `price-risk-v1` means the T3 defaults (252/63/63/126 pairs, min 50 names, clip 5) plus caps 5 / .5. These are fixed on the CLI and fully recorded in the recipe. The API allows other windows (used by the fixtures).
6. **GMV stats set**: executed sessions except the deployment session, with pre-trade gross > 0. Zero-GMV executed sessions (pre-deployment empty sessions) are counted in `zero_gmv_sessions_excluded`.
7. **p95 method**: linear interpolation (numpy default). This differs from T2's participation p95 (histogram upper bound).

## Concerns
- **Not compiled.** The spots most likely to draw a compiler error:
  - brace-init `std::pair{...}` CTAD inside a ternary in fixture (f);
  - nlohmann nested initializer lists in `construction_recipe`/`construction_summary`;
  - the qualified `::atx::impl::strategy::form_desired`/`parse_neutralize` calls from `detail` wrappers, which reach unnamed-namespace functions (the same pattern T2 already uses).
- **Runtime at cadence 1.** T3 recomputes the full 252-interval return block per call: about 2.8M `log` per decision at N=5,600, roughly 20-30 s per replay pass over 754 decisions at /O2. The NAV pays this once for all three scenarios (lockstep); the target replay pays it once too. That fits the 180 s cap, but a rolling exposure update in T3 would be about 250x cheaper. It is out of T4's file scope and noted for the controller.
- **Capped deployment ramp.** Under S2/S3 at $1bn, deployment completes over several sessions. Only the first fill session is excluded, as specified, so the ramp sessions count toward mean/p95; this is documented in `limitations`. S1 (uncapped) deploys in one session.
- **Early decisions skip.** With price-risk-v1 the first ~126 sessions of a role have no beta (min 126 pairs) and skip as too-few-names. This is causal and counted; the TRAIN role has a 399-session warmup, so scored decisions are unaffected.

## Root build / test
Targets: `atx-equity-strategy-targets,atx-impl-strategy-target-tests`. Filter:
`--gtest_filter=StrategyNavReplay.*:StrategyTargetReplay.*:StrategyPrice*` (or `-Ctest -R "StrategyNavReplay|StrategyTargetReplay|StrategyPrice"`).
Expected: 32/32. That is the 25 prior tests (10 NAV, 7 target, 8 price-exposure) plus 3 new NAV and 4 new target fixtures.
